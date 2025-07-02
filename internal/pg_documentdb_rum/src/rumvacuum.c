/*-------------------------------------------------------------------------
 *
 * rumvacuum.c
 *	  delete & vacuum routines for the postgres RUM
 *
 *
 * Portions Copyright (c) 2015-2022, Postgres Professional
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "commands/vacuum.h"
#include "postmaster/autovacuum.h"
#include "storage/indexfsm.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"

#include "pg_documentdb_rum.h"

typedef struct
{
	Relation index;
	IndexBulkDeleteResult *result;
	IndexBulkDeleteCallback callback;
	void *callback_state;
	RumState rumstate;
	BufferAccessStrategy strategy;
}   RumVacuumState;


/*
 * Cleans array of ItemPointer (removes dead pointers)
 * Results are always stored in *cleaned, which will be allocated
 * if it's needed. In case of *cleaned!=NULL caller is responsible to
 * have allocated enough space. *cleaned and items may point to the same
 * memory address.
 */
static OffsetNumber
rumVacuumPostingList(RumVacuumState *gvs, OffsetNumber attnum, Pointer src,
					 OffsetNumber nitem, Pointer *cleaned,
					 Size size, Size *newSize)
{
	OffsetNumber i,
				 j = 0;
	RumItem item;
	ItemPointerData prevIptr;
	Pointer dst = NULL,
			prev,
			ptr = src;

	*newSize = 0;
	ItemPointerSetMin(&item.iptr);

	/*
	 * just scan over ItemPointer array
	 */

	prevIptr = item.iptr;
	for (i = 0; i < nitem; i++)
	{
		prev = ptr;
		ptr = rumDataPageLeafRead(ptr, attnum, &item, false, &gvs->rumstate);
		if (gvs->callback(&item.iptr, gvs->callback_state))
		{
			gvs->result->tuples_removed += 1;
			if (!dst)
			{
				dst = (Pointer) palloc(size);
				*cleaned = dst;
				if (i != 0)
				{
					memcpy(dst, src, prev - src);
					dst += prev - src;
				}
			}
		}
		else
		{
			gvs->result->num_index_tuples += 1;
			if (i != j)
			{
				dst = rumPlaceToDataPageLeaf(dst, attnum, &item,
											 &prevIptr, &gvs->rumstate);
			}
			j++;
			prevIptr = item.iptr;
		}
	}

	if (i != j)
	{
		*newSize = dst - *cleaned;
	}
	return j;
}


/*
 * Form a tuple for entry tree based on already encoded array of item pointers
 * with additional information.
 */
static IndexTuple
RumFormTuple(RumState *rumstate,
			 OffsetNumber attnum, Datum key, RumNullCategory category,
			 Pointer data,
			 Size dataSize,
			 uint32 nipd,
			 bool errorTooBig)
{
	Datum datums[3];
	bool isnull[3];
	IndexTuple itup;
	uint32 newsize;

	/* Build the basic tuple: optional column number, plus key datum */
	if (rumstate->oneCol)
	{
		datums[0] = key;
		isnull[0] = (category != RUM_CAT_NORM_KEY);
		isnull[1] = true;
	}
	else
	{
		datums[0] = UInt16GetDatum(attnum);
		isnull[0] = false;
		datums[1] = key;
		isnull[1] = (category != RUM_CAT_NORM_KEY);
		isnull[2] = true;
	}

	itup = index_form_tuple(rumstate->tupdesc[attnum - 1], datums, isnull);

	/*
	 * Determine and store offset to the posting list, making sure there is
	 * room for the category byte if needed.
	 *
	 * Note: because index_form_tuple MAXALIGNs the tuple size, there may well
	 * be some wasted pad space.  Is it worth recomputing the data length to
	 * prevent that?  That would also allow us to Assert that the real data
	 * doesn't overlap the RumNullCategory byte, which this code currently
	 * takes on faith.
	 */
	newsize = IndexTupleSize(itup);

	RumSetPostingOffset(itup, newsize);

	RumSetNPosting(itup, nipd);

	/*
	 * Add space needed for posting list, if any.  Then check that the tuple
	 * won't be too big to store.
	 */

	if (nipd > 0)
	{
		newsize += dataSize;
	}

	if (category != RUM_CAT_NORM_KEY)
	{
		Assert(IndexTupleHasNulls(itup));
		newsize = newsize + sizeof(RumNullCategory);
	}
	newsize = MAXALIGN(newsize);

	if (newsize > RumMaxItemSize)
	{
		if (errorTooBig)
		{
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("index row size %lu exceeds maximum %lu for index \"%s\"",
							(unsigned long) newsize,
							(unsigned long) RumMaxItemSize,
							RelationGetRelationName(rumstate->index))));
		}
		pfree(itup);
		return NULL;
	}

	/*
	 * Resize tuple if needed
	 */
	if (newsize != IndexTupleSize(itup))
	{
		itup = repalloc(itup, newsize);

		memset((char *) itup + IndexTupleSize(itup),
			   0, newsize - IndexTupleSize(itup));

		/* set new size in tuple header */
		itup->t_info &= ~INDEX_SIZE_MASK;
		itup->t_info |= newsize;
	}

	/*
	 * Copy in the posting list, if provided
	 */
	if (nipd > 0)
	{
		char *ptr = RumGetPosting(itup);

		memcpy(ptr, data, dataSize);
	}

	/*
	 * Insert category byte, if needed
	 */
	if (category != RUM_CAT_NORM_KEY)
	{
		Assert(IndexTupleHasNulls(itup));
		RumSetNullCategory(itup, category);
	}
	return itup;
}


static bool
rumVacuumPostingTreeLeaves(RumVacuumState *gvs, OffsetNumber attnum,
						   BlockNumber blkno, bool isRoot, Buffer *rootBuffer)
{
	Buffer buffer;
	Page page;
	bool hasVoidPage = false;

	buffer = ReadBufferExtended(gvs->index, MAIN_FORKNUM, blkno,
								RBM_NORMAL, gvs->strategy);
	page = BufferGetPage(buffer);

	/*
	 * We should be sure that we don't concurrent with inserts, insert process
	 * never release root page until end (but it can unlock it and lock
	 * again). New scan can't start but previously started ones work
	 * concurrently.
	 */

	if (isRoot)
	{
		LockBufferForCleanup(buffer);
	}
	else
	{
		LockBuffer(buffer, RUM_EXCLUSIVE);
	}

	Assert(RumPageIsData(page));

	if (RumPageIsLeaf(page))
	{
		OffsetNumber newMaxOff,
					 oldMaxOff = RumPageGetOpaque(page)->maxoff;
		Pointer cleaned = NULL;
		Size newSize;

		newMaxOff = rumVacuumPostingList(gvs, attnum,
										 RumDataPageGetData(page), oldMaxOff, &cleaned,
										 RumDataPageSize - RumPageGetOpaque(
											 page)->freespace, &newSize);

		/* saves changes about deleted tuple ... */
		if (oldMaxOff != newMaxOff)
		{
			GenericXLogState *state;
			Page newPage;

			state = GenericXLogStart(gvs->index);

			newPage = GenericXLogRegisterBuffer(state, buffer, 0);

			if (newMaxOff > 0)
			{
				memcpy(RumDataPageGetData(newPage), cleaned, newSize);
			}

			pfree(cleaned);
			RumPageGetOpaque(newPage)->maxoff = newMaxOff;
			updateItemIndexes(newPage, attnum, &gvs->rumstate);

			/* if root is a leaf page, we don't desire further processing */
			if (!isRoot && RumPageGetOpaque(newPage)->maxoff < FirstOffsetNumber)
			{
				hasVoidPage = true;
			}

			GenericXLogFinish(state);
		}
	}
	else
	{
		OffsetNumber i;
		bool isChildHasVoid = false;

		for (i = FirstOffsetNumber; i <= RumPageGetOpaque(page)->maxoff; i++)
		{
			PostingItem *pitem = (PostingItem *) RumDataPageGetItem(page, i);

			if (rumVacuumPostingTreeLeaves(gvs, attnum,
										   PostingItemGetBlockNumber(pitem), false, NULL))
			{
				isChildHasVoid = true;
			}
		}

		if (isChildHasVoid)
		{
			hasVoidPage = true;
		}
	}

	/*
	 * if we have root and theres void pages in tree, then we don't release
	 * lock to go further processing and guarantee that tree is unused
	 */
	if (!(isRoot && hasVoidPage))
	{
		UnlockReleaseBuffer(buffer);
	}
	else
	{
		Assert(rootBuffer);
		*rootBuffer = buffer;
	}

	return hasVoidPage;
}


/*
 * Delete a posting tree page.
 */
static bool
rumDeletePage(RumVacuumState *gvs, BlockNumber deleteBlkno,
			  BlockNumber parentBlkno, OffsetNumber myoff, bool isParentRoot)
{
	BlockNumber leftBlkno,
				rightBlkno;
	Buffer dBuffer;
	Buffer lBuffer,
		   rBuffer;
	Buffer pBuffer;
	Page lPage,
		 dPage,
		 rPage,
		 parentPage;
	GenericXLogState *state;

restart:

	dBuffer = ReadBufferExtended(gvs->index, MAIN_FORKNUM, deleteBlkno,
								 RBM_NORMAL, gvs->strategy);

	LockBuffer(dBuffer, RUM_EXCLUSIVE);

	dPage = BufferGetPage(dBuffer);
	leftBlkno = RumPageGetOpaque(dPage)->leftlink;
	rightBlkno = RumPageGetOpaque(dPage)->rightlink;

	/* do not remove left/right most pages */
	if (leftBlkno == InvalidBlockNumber || rightBlkno == InvalidBlockNumber)
	{
		UnlockReleaseBuffer(dBuffer);
		return false;
	}

	LockBuffer(dBuffer, RUM_UNLOCK);

	/*
	 * Lock the pages in the same order as an insertion would, to avoid
	 * deadlocks: left, then right, then parent.
	 */
	lBuffer = ReadBufferExtended(gvs->index, MAIN_FORKNUM, leftBlkno,
								 RBM_NORMAL, gvs->strategy);
	rBuffer = ReadBufferExtended(gvs->index, MAIN_FORKNUM, rightBlkno,
								 RBM_NORMAL, gvs->strategy);
	pBuffer = ReadBufferExtended(gvs->index, MAIN_FORKNUM, parentBlkno,
								 RBM_NORMAL, gvs->strategy);

	LockBuffer(lBuffer, RUM_EXCLUSIVE);
	if (ConditionalLockBufferForCleanup(dBuffer) == false)
	{
		UnlockReleaseBuffer(lBuffer);
		ReleaseBuffer(dBuffer);
		ReleaseBuffer(rBuffer);
		ReleaseBuffer(pBuffer);
		goto restart;
	}
	LockBuffer(rBuffer, RUM_EXCLUSIVE);
	if (!isParentRoot)          /* parent is already locked by
	                             * LockBufferForCleanup() */
	{
		LockBuffer(pBuffer, RUM_EXCLUSIVE);
	}

	lPage = BufferGetPage(lBuffer);
	rPage = BufferGetPage(rBuffer);

	/*
	 * last chance to check
	 */
	if (!(RumPageGetOpaque(lPage)->rightlink == deleteBlkno &&
		  RumPageGetOpaque(rPage)->leftlink == deleteBlkno &&
		  RumPageGetOpaque(dPage)->maxoff < FirstOffsetNumber))
	{
		OffsetNumber dMaxoff = RumPageGetOpaque(dPage)->maxoff;

		if (!isParentRoot)
		{
			LockBuffer(pBuffer, RUM_UNLOCK);
		}
		ReleaseBuffer(pBuffer);
		UnlockReleaseBuffer(lBuffer);
		UnlockReleaseBuffer(dBuffer);
		UnlockReleaseBuffer(rBuffer);

		if (dMaxoff >= FirstOffsetNumber)
		{
			return false;
		}

		goto restart;
	}

	/* At least make the WAL record */

	state = GenericXLogStart(gvs->index);

	dPage = GenericXLogRegisterBuffer(state, dBuffer, 0);
	lPage = GenericXLogRegisterBuffer(state, lBuffer, 0);
	rPage = GenericXLogRegisterBuffer(state, rBuffer, 0);

	RumPageGetOpaque(lPage)->rightlink = rightBlkno;
	RumPageGetOpaque(rPage)->leftlink = leftBlkno;

	/*
	 * Any insert which would have gone on the leaf block will now go to its
	 * right sibling.
	 */
	PredicateLockPageCombine(gvs->index, deleteBlkno, rightBlkno);

	/* Delete downlink from parent */
	parentPage = GenericXLogRegisterBuffer(state, pBuffer, 0);
#ifdef USE_ASSERT_CHECKING
	do {
		PostingItem *tod = (PostingItem *) RumDataPageGetItem(parentPage, myoff);

		Assert(PostingItemGetBlockNumber(tod) == deleteBlkno);
	} while (0);
#endif
	RumPageDeletePostingItem(parentPage, myoff);

	/*
	 * we shouldn't change left/right link field to save workability of running
	 * search scan
	 */
	RumPageGetOpaque(dPage)->flags = RUM_DELETED;

	GenericXLogFinish(state);

	if (!isParentRoot)
	{
		LockBuffer(pBuffer, RUM_UNLOCK);
	}
	ReleaseBuffer(pBuffer);
	UnlockReleaseBuffer(lBuffer);
	UnlockReleaseBuffer(dBuffer);
	UnlockReleaseBuffer(rBuffer);

	gvs->result->pages_deleted++;

	return true;
}


typedef struct DataPageDeleteStack
{
	struct DataPageDeleteStack *child;
	struct DataPageDeleteStack *parent;

	BlockNumber blkno;          /* current block number */
	bool isRoot;
} DataPageDeleteStack;

/*
 * scans posting tree and deletes empty pages
 */
static bool
rumScanToDelete(RumVacuumState *gvs, BlockNumber blkno, bool isRoot,
				DataPageDeleteStack *parent, OffsetNumber myoff)
{
	DataPageDeleteStack *me;
	Buffer buffer;
	Page page;
	bool meDelete = false;

	if (isRoot)
	{
		me = parent;
	}
	else
	{
		if (!parent->child)
		{
			me = (DataPageDeleteStack *) palloc0(sizeof(DataPageDeleteStack));
			me->parent = parent;
			parent->child = me;
		}
		else
		{
			me = parent->child;
		}
	}

	buffer = ReadBufferExtended(gvs->index, MAIN_FORKNUM, blkno,
								RBM_NORMAL, gvs->strategy);
	page = BufferGetPage(buffer);

	Assert(RumPageIsData(page));

	if (!RumPageIsLeaf(page))
	{
		OffsetNumber i;

		me->blkno = blkno;
		for (i = FirstOffsetNumber; i <= RumPageGetOpaque(page)->maxoff; i++)
		{
			PostingItem *pitem = (PostingItem *) RumDataPageGetItem(page, i);

			if (rumScanToDelete(gvs, PostingItemGetBlockNumber(pitem), false, me, i))
			{
				i--;
			}
		}
	}

	if (RumPageGetOpaque(page)->maxoff < FirstOffsetNumber && !isRoot)
	{
		/*
		 * Release the buffer because in rumDeletePage() we need to pin it again
		 * and call ConditionalLockBufferForCleanup().
		 */
		ReleaseBuffer(buffer);
		meDelete = rumDeletePage(gvs, blkno, me->parent->blkno, myoff,
								 me->parent->isRoot);
	}
	else
	{
		ReleaseBuffer(buffer);
	}

	return meDelete;
}


static void
rumVacuumPostingTree(RumVacuumState *gvs, OffsetNumber attnum, BlockNumber rootBlkno)
{
	Buffer rootBuffer = InvalidBuffer;
	DataPageDeleteStack root,
						*ptr,
						*tmp;

	if (rumVacuumPostingTreeLeaves(gvs, attnum, rootBlkno, true, &rootBuffer) == false)
	{
		Assert(rootBuffer == InvalidBuffer);
		return;
	}

	memset(&root, 0, sizeof(DataPageDeleteStack));
	root.isRoot = true;

	vacuum_delay_point();

	rumScanToDelete(gvs, rootBlkno, true, &root, InvalidOffsetNumber);

	ptr = root.child;
	while (ptr)
	{
		tmp = ptr->child;
		pfree(ptr);
		ptr = tmp;
	}

	UnlockReleaseBuffer(rootBuffer);
}


/*
 * returns modified page or NULL if page isn't modified.
 * Function works with original page until first change is occurred,
 * then page is copied into temporary one.
 */
static Page
rumVacuumEntryPage(RumVacuumState *gvs, Buffer buffer, BlockNumber *roots,
				   OffsetNumber *attnums, uint32 *nroot)
{
	Page origpage = BufferGetPage(buffer),
		 tmppage;
	OffsetNumber i,
				 maxoff = PageGetMaxOffsetNumber(origpage);

	tmppage = origpage;

	*nroot = 0;

	for (i = FirstOffsetNumber; i <= maxoff; i++)
	{
		IndexTuple itup = (IndexTuple) PageGetItem(tmppage, PageGetItemId(tmppage, i));

		if (RumIsPostingTree(itup))
		{
			/*
			 * store posting tree's roots for further processing, we can't
			 * vacuum it just now due to risk of deadlocks with scans/inserts
			 */
			roots[*nroot] = RumGetDownlink(itup);
			attnums[*nroot] = rumtuple_get_attrnum(&gvs->rumstate, itup);
			(*nroot)++;
		}
		else if (RumGetNPosting(itup) > 0)
		{
			/*
			 * if we already create temporary page, we will make changes in
			 * place
			 */
			Size cleanedSize;
			Pointer cleaned = NULL;
			uint32 newN =
				rumVacuumPostingList(gvs, rumtuple_get_attrnum(&gvs->rumstate, itup),
									 RumGetPosting(itup), RumGetNPosting(itup), &cleaned,
									 IndexTupleSize(itup) - RumGetPostingOffset(itup),
									 &cleanedSize);

			if (RumGetNPosting(itup) != newN)
			{
				OffsetNumber attnum;
				Datum key;
				RumNullCategory category;

				/*
				 * Some ItemPointers was deleted, so we should remake our
				 * tuple
				 */

				if (tmppage == origpage)
				{
					/*
					 * On first difference we create temporary page in memory
					 * and copies content in to it.
					 */
					tmppage = PageGetTempPageCopy(origpage);

					/* set itup pointer to new page */
					itup = (IndexTuple) PageGetItem(tmppage, PageGetItemId(tmppage, i));
				}

				attnum = rumtuple_get_attrnum(&gvs->rumstate, itup);
				key = rumtuple_get_key(&gvs->rumstate, itup, &category);

				/* FIXME */
				itup = RumFormTuple(&gvs->rumstate, attnum, key, category,
									cleaned, cleanedSize, newN, true);
				pfree(cleaned);
				PageIndexTupleDelete(tmppage, i);

				if (PageAddItem(tmppage, (Item) itup, IndexTupleSize(itup), i, false,
								false) != i)
				{
					elog(ERROR, "failed to add item to index page in \"%s\"",
						 RelationGetRelationName(gvs->index));
				}

				pfree(itup);
			}
		}
	}

	return (tmppage == origpage) ? NULL : tmppage;
}


IndexBulkDeleteResult *
rumbulkdelete(IndexVacuumInfo *info,
			  IndexBulkDeleteResult *stats, IndexBulkDeleteCallback callback,
			  void *callback_state)
{
	Relation index = info->index;
	BlockNumber blkno = RUM_ROOT_BLKNO;
	RumVacuumState gvs;
	Buffer buffer;
	BlockNumber rootOfPostingTree[BLCKSZ / (sizeof(IndexTupleData) + sizeof(ItemId))];
	OffsetNumber attnumOfPostingTree[BLCKSZ / (sizeof(IndexTupleData) + sizeof(ItemId))];
	uint32 nRoot;

	gvs.index = index;
	gvs.callback = callback;
	gvs.callback_state = callback_state;
	gvs.strategy = info->strategy;
	initRumState(&gvs.rumstate, index);

	/* first time through? */
	if (stats == NULL)
	{
		/* Yes, so initialize stats to zeroes */
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
	}

	/* we'll re-count the tuples each time */
	stats->num_index_tuples = 0;
	gvs.result = stats;

	buffer = ReadBufferExtended(index, MAIN_FORKNUM, blkno,
								RBM_NORMAL, info->strategy);

	/* find leaf page */
	for (;;)
	{
		Page page = BufferGetPage(buffer);
		IndexTuple itup;

		LockBuffer(buffer, RUM_SHARE);

		Assert(!RumPageIsData(page));

		if (RumPageIsLeaf(page))
		{
			LockBuffer(buffer, RUM_UNLOCK);
			LockBuffer(buffer, RUM_EXCLUSIVE);

			if (blkno == RUM_ROOT_BLKNO && !RumPageIsLeaf(page))
			{
				LockBuffer(buffer, RUM_UNLOCK);
				continue;       /* check it one more */
			}
			break;
		}

		Assert(PageGetMaxOffsetNumber(page) >= FirstOffsetNumber);

		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, FirstOffsetNumber));
		blkno = RumGetDownlink(itup);
		Assert(blkno != InvalidBlockNumber);

		UnlockReleaseBuffer(buffer);
		buffer = ReadBufferExtended(index, MAIN_FORKNUM, blkno,
									RBM_NORMAL, info->strategy);
	}

	/* right now we found leftmost page in entry's BTree */

	for (;;)
	{
		Page page = BufferGetPage(buffer);
		Page resPage;
		uint32 i;

		Assert(!RumPageIsData(page));

		resPage = rumVacuumEntryPage(&gvs, buffer, rootOfPostingTree, attnumOfPostingTree,
									 &nRoot);

		blkno = RumPageGetOpaque(page)->rightlink;

		if (resPage)
		{
			GenericXLogState *state;

			state = GenericXLogStart(index);
			page = GenericXLogRegisterBuffer(state, buffer, 0);
			PageRestoreTempPage(resPage, page);
			GenericXLogFinish(state);
			UnlockReleaseBuffer(buffer);
		}
		else
		{
			UnlockReleaseBuffer(buffer);
		}

		vacuum_delay_point();

		for (i = 0; i < nRoot; i++)
		{
			rumVacuumPostingTree(&gvs, attnumOfPostingTree[i], rootOfPostingTree[i]);
			vacuum_delay_point();
		}

		if (blkno == InvalidBlockNumber)        /* rightmost page */
		{
			break;
		}

		buffer = ReadBufferExtended(index, MAIN_FORKNUM, blkno,
									RBM_NORMAL, info->strategy);
		LockBuffer(buffer, RUM_EXCLUSIVE);
	}

	return gvs.result;
}


IndexBulkDeleteResult *
rumvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	Relation index = info->index;
	bool needLock;
	BlockNumber npages,
				blkno;
	BlockNumber totFreePages;
	GinStatsData idxStat;

	/*
	 * In an autovacuum analyze, we want to clean up pending insertions.
	 * Otherwise, an ANALYZE-only call is a no-op.
	 */
	if (info->analyze_only)
	{
		return stats;
	}

	/*
	 * Set up all-zero stats and cleanup pending inserts if rumbulkdelete
	 * wasn't called
	 */
	if (stats == NULL)
	{
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
	}

	memset(&idxStat, 0, sizeof(idxStat));

	/*
	 * XXX we always report the heap tuple count as the number of index
	 * entries.  This is bogus if the index is partial, but it's real hard to
	 * tell how many distinct heap entries are referenced by a RUM index.
	 */
	stats->num_index_tuples = info->num_heap_tuples;
	stats->estimated_count = info->estimated_count;

	/*
	 * Need lock unless it's local to this backend.
	 */
	needLock = !RELATION_IS_LOCAL(index);

	if (needLock)
	{
		LockRelationForExtension(index, ExclusiveLock);
	}
	npages = RelationGetNumberOfBlocks(index);
	if (needLock)
	{
		UnlockRelationForExtension(index, ExclusiveLock);
	}

	totFreePages = 0;

	for (blkno = RUM_ROOT_BLKNO; blkno < npages; blkno++)
	{
		Buffer buffer;
		Page page;

		vacuum_delay_point();

		buffer = ReadBufferExtended(index, MAIN_FORKNUM, blkno,
									RBM_NORMAL, info->strategy);
		LockBuffer(buffer, RUM_SHARE);
		page = (Page) BufferGetPage(buffer);

		if (PageIsNew(page) || RumPageIsDeleted(page))
		{
			Assert(blkno != RUM_ROOT_BLKNO);
			RecordFreeIndexPage(index, blkno);
			totFreePages++;
		}
		else if (RumPageIsData(page))
		{
			idxStat.nDataPages++;
		}
		else if (!RumPageIsList(page))
		{
			idxStat.nEntryPages++;

			if (RumPageIsLeaf(page))
			{
				idxStat.nEntries += PageGetMaxOffsetNumber(page);
			}
		}

		UnlockReleaseBuffer(buffer);
	}

	/* Update the metapage with accurate page and entry counts */
	idxStat.nTotalPages = npages;
	rumUpdateStats(info->index, &idxStat, false);

	/* Finally, vacuum the FSM */
	IndexFreeSpaceMapVacuum(info->index);

	stats->pages_free = totFreePages;

	if (needLock)
	{
		LockRelationForExtension(index, ExclusiveLock);
	}
	stats->num_pages = RelationGetNumberOfBlocks(index);
	if (needLock)
	{
		UnlockRelationForExtension(index, ExclusiveLock);
	}

	return stats;
}
