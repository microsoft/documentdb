/*-------------------------------------------------------------------------
 *
 * ruminsert.c
 *	  insert routines for the postgres inverted index access method.
 *
 *
 * Portions Copyright (c) 2015-2022, Postgres Professional
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/generic_xlog.h"
#if PG_VERSION_NUM >= 120000
#include "access/tableam.h"
#endif
#include "storage/predicate.h"
#include "catalog/index.h"
#include "miscadmin.h"
#include "utils/datum.h"

#include "rum.h"

typedef struct
{
	RumState rumstate;
	double indtuples;
	GinStatsData buildStats;
	MemoryContext tmpCtx;
	MemoryContext funcCtx;
	BuildAccumulator accum;
}   RumBuildState;


#if PG_VERSION_NUM >= 120000
#define IndexBuildHeapScan(A, B, C, D, E, F) \
	table_index_build_scan(A, B, C, D, true, E, F, NULL)
#elif PG_VERSION_NUM >= 110000
#define IndexBuildHeapScan(A, B, C, D, E, F) \
	IndexBuildHeapScan(A, B, C, D, E, F, NULL)
#endif

/*
 * Creates new posting tree with one page, containing the given TIDs.
 * Returns the page number (which will be the root of this posting tree).
 *
 * items[] must be in sorted order with no duplicates.
 */
static BlockNumber
createPostingTree(RumState *rumstate, OffsetNumber attnum, Relation index,
				  RumItem *items, uint32 nitems)
{
	BlockNumber blkno;
	Buffer buffer = RumNewBuffer(index);
	Page page;
	int i;
	Pointer ptr;
	ItemPointerData prev_iptr = { { 0, 0 }, 0 };
	GenericXLogState *state = NULL;

	if (rumstate->isBuild)
	{
		page = BufferGetPage(buffer);
		START_CRIT_SECTION();
	}
	else
	{
		state = GenericXLogStart(index);
		page = GenericXLogRegisterBuffer(state, buffer, GENERIC_XLOG_FULL_IMAGE);
	}
	RumInitPage(page, RUM_DATA | RUM_LEAF, BufferGetPageSize(buffer));

	blkno = BufferGetBlockNumber(buffer);

	RumPageGetOpaque(page)->maxoff = nitems;
	ptr = RumDataPageGetData(page);
	for (i = 0; i < nitems; i++)
	{
		if (i > 0)
		{
			prev_iptr = items[i - 1].iptr;
		}
		ptr = rumPlaceToDataPageLeaf(ptr, attnum, &items[i],
									 &prev_iptr, rumstate);
	}
	Assert(RumDataPageFreeSpacePre(page, ptr) >= 0);
	updateItemIndexes(page, attnum, rumstate);

	if (rumstate->isBuild)
	{
		MarkBufferDirty(buffer);
	}
	else
	{
		GenericXLogFinish(state);
	}

	UnlockReleaseBuffer(buffer);

	if (rumstate->isBuild)
	{
		END_CRIT_SECTION();
	}

	return blkno;
}


/*
 * Form a tuple for entry tree.
 *
 * If the tuple would be too big to be stored, function throws a suitable
 * error if errorTooBig is true, or returns NULL if errorTooBig is false.
 *
 * See src/backend/access/gin/README for a description of the index tuple
 * format that is being built here.  We build on the assumption that we
 * are making a leaf-level key entry containing a posting list of nipd items.
 * If the caller is actually trying to make a posting-tree entry, non-leaf
 * entry, or pending-list entry, it should pass nipd = 0 and then overwrite
 * the t_tid fields as necessary.  In any case, items can be NULL to skip
 * copying any itempointers into the posting list; the caller is responsible
 * for filling the posting list afterwards, if items = NULL and nipd > 0.
 */
static IndexTuple
RumFormTuple(RumState *rumstate,
			 OffsetNumber attnum, Datum key, RumNullCategory category,
			 RumItem *items, uint32 nipd, bool errorTooBig)
{
	Datum datums[3];
	bool isnull[3];
	IndexTuple itup;
	uint32 newsize;
	int i;
	ItemPointerData nullItemPointer = { { 0, 0 }, 0 };

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
		newsize = rumCheckPlaceToDataPageLeaf(attnum, &items[0],
											  &nullItemPointer,
											  rumstate, newsize);
		for (i = 1; i < nipd; i++)
		{
			newsize = rumCheckPlaceToDataPageLeaf(attnum, &items[i],
												  &items[i - 1].iptr,
												  rumstate, newsize);
		}
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

		ptr = rumPlaceToDataPageLeaf(ptr, attnum, &items[0],
									 &nullItemPointer, rumstate);
		for (i = 1; i < nipd; i++)
		{
			ptr = rumPlaceToDataPageLeaf(ptr, attnum, &items[i],
										 &items[i - 1].iptr, rumstate);
		}

		Assert(MAXALIGN((ptr - ((char *) itup)) +
						((category == RUM_CAT_NORM_KEY) ? 0 : sizeof(RumNullCategory))) ==
			   newsize);
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


/*
 * Adds array of item pointers to tuple's posting list, or
 * creates posting tree and tuple pointing to tree in case
 * of not enough space.  Max size of tuple is defined in
 * RumFormTuple().  Returns a new, modified index tuple.
 * items[] must be in sorted order with no duplicates.
 */
static IndexTuple
addItemPointersToLeafTuple(RumState *rumstate,
						   IndexTuple old, RumItem *items, uint32 nitem,
						   GinStatsData *buildStats)
{
	OffsetNumber attnum;
	Datum key;
	RumNullCategory category;
	IndexTuple res;
	RumItem *newItems,
			*oldItems;
	int oldNPosting,
		newNPosting;

	Assert(!RumIsPostingTree(old));

	attnum = rumtuple_get_attrnum(rumstate, old);
	key = rumtuple_get_key(rumstate, old, &category);

	oldNPosting = RumGetNPosting(old);
	oldItems = (RumItem *) palloc(sizeof(RumItem) * oldNPosting);

	newNPosting = oldNPosting + nitem;
	newItems = (RumItem *) palloc(sizeof(RumItem) * newNPosting);

	rumReadTuple(rumstate, attnum, old, oldItems, false);

	newNPosting = rumMergeRumItems(rumstate, attnum, newItems,
								   items, nitem, oldItems, oldNPosting);


	/* try to build tuple with room for all the items */
	res = RumFormTuple(rumstate, attnum, key, category,
					   newItems, newNPosting, false);

	if (!res)
	{
		/* posting list would be too big, convert to posting tree */
		BlockNumber postingRoot;
		RumPostingTreeScan *gdi;

		/*
		 * Initialize posting tree with the old tuple's posting list.  It's
		 * surely small enough to fit on one posting-tree page, and should
		 * already be in order with no duplicates.
		 */
		postingRoot = createPostingTree(rumstate,
										attnum,
										rumstate->index,
										oldItems,
										oldNPosting);

		/* During index build, count the newly-added data page */
		if (buildStats)
		{
			buildStats->nDataPages++;
		}

		/* Now insert the TIDs-to-be-added into the posting tree */
		gdi = rumPrepareScanPostingTree(rumstate->index, postingRoot, false,
										ForwardScanDirection, attnum, rumstate);
		rumInsertItemPointers(rumstate, attnum, gdi, items, nitem, buildStats);

		pfree(gdi);

		/* And build a new posting-tree-only result tuple */
		res = RumFormTuple(rumstate, attnum, key, category, NULL, 0, true);
		RumSetPostingTree(res, postingRoot);
	}

	return res;
}


/*
 * Build a fresh leaf tuple, either posting-list or posting-tree format
 * depending on whether the given items list will fit.
 * items[] must be in sorted order with no duplicates.
 *
 * This is basically the same logic as in addItemPointersToLeafTuple,
 * but working from slightly different input.
 */
static IndexTuple
buildFreshLeafTuple(RumState *rumstate,
					OffsetNumber attnum, Datum key, RumNullCategory category,
					RumItem *items, uint32 nitem, GinStatsData *buildStats)
{
	IndexTuple res;

	/* try to build tuple with room for all the items */
	res = RumFormTuple(rumstate, attnum, key, category, items, nitem, false);

	if (!res)
	{
		/* posting list would be too big, build posting tree */
		BlockNumber postingRoot;
		ItemPointerData prevIptr = { { 0, 0 }, 0 };
		Size size = 0;
		int itemsCount = 0;

		do {
			size = rumCheckPlaceToDataPageLeaf(attnum, &items[itemsCount],
											   &prevIptr, rumstate, size);
			prevIptr = items[itemsCount].iptr;
			itemsCount++;
		} while (itemsCount < nitem && size < RumDataPageSize);

		if (size >= RumDataPageSize)
		{
			itemsCount--;
		}

		/*
		 * Build posting-tree-only result tuple.  We do this first so as to
		 * fail quickly if the key is too big.
		 */
		res = RumFormTuple(rumstate, attnum, key, category, NULL, 0, true);

		/*
		 * Initialize posting tree with as many TIDs as will fit on the first
		 * page.
		 */
		postingRoot = createPostingTree(rumstate,
										attnum,
										rumstate->index,
										items,
										itemsCount);

		/* During index build, count the newly-added data page */
		if (buildStats)
		{
			buildStats->nDataPages++;
		}

		/* Add any remaining TIDs to the posting tree */
		if (nitem > itemsCount)
		{
			RumPostingTreeScan *gdi;

			gdi = rumPrepareScanPostingTree(rumstate->index, postingRoot, false,
											ForwardScanDirection,
											attnum, rumstate);

			rumInsertItemPointers(rumstate,
								  attnum,
								  gdi,
								  items + itemsCount,
								  nitem - itemsCount,
								  buildStats);

			pfree(gdi);
		}

		/* And save the root link in the result tuple */
		RumSetPostingTree(res, postingRoot);
	}

	return res;
}


/*
 * Insert one or more heap TIDs associated with the given key value.
 * This will either add a single key entry, or enlarge a pre-existing entry.
 *
 * During an index build, buildStats is non-null and the counters
 * it contains should be incremented as needed.
 */
void
rumEntryInsert(RumState *rumstate,
			   OffsetNumber attnum, Datum key, RumNullCategory category,
			   RumItem *items, uint32 nitem,
			   GinStatsData *buildStats)
{
	RumBtreeData btree;
	RumBtreeStack *stack;
	IndexTuple itup;
	Page page;

	/* During index build, count the to-be-inserted entry */
	if (buildStats)
	{
		buildStats->nEntries++;
	}

	rumPrepareEntryScan(&btree, attnum, key, category, rumstate);

	stack = rumFindLeafPage(&btree, NULL);
	page = BufferGetPage(stack->buffer);

	CheckForSerializableConflictIn(btree.index, NULL, stack->buffer);

	if (btree.findItem(&btree, stack))
	{
		/* found pre-existing entry */
		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, stack->off));

		if (RumIsPostingTree(itup))
		{
			/* add entries to existing posting tree */
			BlockNumber rootPostingTree = RumGetPostingTree(itup);
			RumPostingTreeScan *gdi;

			/* release all stack */
			LockBuffer(stack->buffer, RUM_UNLOCK);
			freeRumBtreeStack(stack);

			/* insert into posting tree */
			gdi = rumPrepareScanPostingTree(rumstate->index, rootPostingTree,
											false, ForwardScanDirection,
											attnum, rumstate);
			rumInsertItemPointers(rumstate, attnum, gdi, items,
								  nitem, buildStats);
			pfree(gdi);

			return;
		}

		/* modify an existing leaf entry */
		itup = addItemPointersToLeafTuple(rumstate, itup,
										  items, nitem, buildStats);

		btree.isDelete = true;
	}
	else
	{
		/* no match, so construct a new leaf entry */
		itup = buildFreshLeafTuple(rumstate, attnum, key, category,
								   items, nitem, buildStats);
	}

	/* Insert the new or modified leaf tuple */
	btree.entry = itup;
	rumInsertValue(rumstate->index, &btree, stack, buildStats);
	pfree(itup);
}


/*
 * Extract index entries for a single indexable item, and add them to the
 * BuildAccumulator's state.
 *
 * This function is used only during initial index creation.
 */
static void
rumHeapTupleBulkInsert(RumBuildState *buildstate, OffsetNumber attnum,
					   Datum value, bool isNull,
					   ItemPointer heapptr,
					   Datum outerAddInfo,
					   bool outerAddInfoIsNull)
{
	Datum *entries;
	RumNullCategory *categories;
	int32 nentries;
	MemoryContext oldCtx;
	Datum *addInfo;
	bool *addInfoIsNull;
	int i;
	Form_pg_attribute attr = buildstate->rumstate.addAttrs[attnum - 1];

	oldCtx = MemoryContextSwitchTo(buildstate->funcCtx);
	entries = rumExtractEntries(buildstate->accum.rumstate, attnum,
								value, isNull,
								&nentries, &categories,
								&addInfo, &addInfoIsNull);

	if (attnum == buildstate->rumstate.attrnAddToColumn)
	{
		addInfo = palloc(sizeof(*addInfo) * nentries);
		addInfoIsNull = palloc(sizeof(*addInfoIsNull) * nentries);

		for (i = 0; i < nentries; i++)
		{
			addInfo[i] = outerAddInfo;
			addInfoIsNull[i] = outerAddInfoIsNull;
		}
	}

	MemoryContextSwitchTo(oldCtx);
	for (i = 0; i < nentries; i++)
	{
		if (!addInfoIsNull[i])
		{
			/* Check existance of additional information attribute in index */
			if (!attr)
			{
				Form_pg_attribute current_attr = RumTupleDescAttr(
					buildstate->rumstate.origTupdesc, attnum - 1);

				elog(ERROR,
					 "additional information attribute \"%s\" is not found in index",
					 NameStr(current_attr->attname));
			}

			addInfo[i] = datumCopy(addInfo[i], attr->attbyval, attr->attlen);
		}
	}

	rumInsertBAEntries(&buildstate->accum, heapptr, attnum,
					   entries, addInfo, addInfoIsNull, categories, nentries);

	buildstate->indtuples += nentries;

	MemoryContextReset(buildstate->funcCtx);
}


static void
rumBuildCallback(Relation index,
#if PG_VERSION_NUM < 130000
				 HeapTuple htup,
#else
				 ItemPointer tid,
#endif
				 Datum *values,
				 bool *isnull, bool tupleIsAlive, void *state)
{
	RumBuildState *buildstate = (RumBuildState *) state;
	MemoryContext oldCtx;
	int i;
	Datum outerAddInfo = (Datum) 0;
	bool outerAddInfoIsNull = true;
#if PG_VERSION_NUM < 130000
	ItemPointer tid = &htup->t_self;
#endif

	if (AttributeNumberIsValid(buildstate->rumstate.attrnAttachColumn))
	{
		outerAddInfo = values[buildstate->rumstate.attrnAttachColumn - 1];
		outerAddInfoIsNull = isnull[buildstate->rumstate.attrnAttachColumn - 1];
	}

	oldCtx = MemoryContextSwitchTo(buildstate->tmpCtx);

	for (i = 0; i < buildstate->rumstate.origTupdesc->natts; i++)
	{
		rumHeapTupleBulkInsert(buildstate, (OffsetNumber) (i + 1),
							   values[i], isnull[i],
							   tid,
							   outerAddInfo, outerAddInfoIsNull);
	}

	/* If we've maxed out our available memory, dump everything to the index */
	if (buildstate->accum.allocatedMemory >= maintenance_work_mem * 1024L)
	{
		RumItem *items;
		Datum key;
		RumNullCategory category;
		uint32 nlist;
		OffsetNumber attnum;

		rumBeginBAScan(&buildstate->accum);
		while ((items = rumGetBAEntry(&buildstate->accum,
									  &attnum, &key, &category, &nlist)) != NULL)
		{
			/* there could be many entries, so be willing to abort here */
			CHECK_FOR_INTERRUPTS();
			rumEntryInsert(&buildstate->rumstate, attnum, key, category,
						   items, nlist, &buildstate->buildStats);
		}

		MemoryContextReset(buildstate->tmpCtx);
		rumInitBA(&buildstate->accum);
	}

	MemoryContextSwitchTo(oldCtx);
}


IndexBuildResult *
rumbuild(Relation heap, Relation index, struct IndexInfo *indexInfo)
{
	IndexBuildResult *result;
	double reltuples;
	RumBuildState buildstate;
	Buffer RootBuffer,
		   MetaBuffer;
	RumItem *items;
	Datum key;
	RumNullCategory category;
	uint32 nlist;
	MemoryContext oldCtx;
	OffsetNumber attnum;
	BlockNumber blkno;

	if (RelationGetNumberOfBlocks(index) != 0)
	{
		elog(ERROR, "index \"%s\" already contains data",
			 RelationGetRelationName(index));
	}

	initRumState(&buildstate.rumstate, index);
	buildstate.rumstate.isBuild = true;
	buildstate.indtuples = 0;
	memset(&buildstate.buildStats, 0, sizeof(GinStatsData));

	/* initialize the meta page */
	MetaBuffer = RumNewBuffer(index);

	/* initialize the root page */
	RootBuffer = RumNewBuffer(index);

	START_CRIT_SECTION();
	RumInitMetabuffer(NULL, MetaBuffer, buildstate.rumstate.isBuild);
	MarkBufferDirty(MetaBuffer);
	RumInitBuffer(NULL, RootBuffer, RUM_LEAF, buildstate.rumstate.isBuild);
	MarkBufferDirty(RootBuffer);

	UnlockReleaseBuffer(MetaBuffer);
	UnlockReleaseBuffer(RootBuffer);
	END_CRIT_SECTION();

	/* count the root as first entry page */
	buildstate.buildStats.nEntryPages++;

	/*
	 * create a temporary memory context that is reset once for each tuple
	 * inserted into the index
	 */
	buildstate.tmpCtx = RumContextCreate(CurrentMemoryContext,
										 "Rum build temporary context");

	buildstate.funcCtx = RumContextCreate(CurrentMemoryContext,
										  "Rum build temporary context for user-defined function");

	buildstate.accum.rumstate = &buildstate.rumstate;
	rumInitBA(&buildstate.accum);

	/*
	 * Do the heap scan.  We disallow sync scan here because dataPlaceToPage
	 * prefers to receive tuples in TID order.
	 */
	reltuples = IndexBuildHeapScan(heap, index, indexInfo, false,
								   rumBuildCallback, (void *) &buildstate);

	/* dump remaining entries to the index */
	oldCtx = MemoryContextSwitchTo(buildstate.tmpCtx);
	rumBeginBAScan(&buildstate.accum);
	while ((items = rumGetBAEntry(&buildstate.accum,
								  &attnum, &key, &category, &nlist)) != NULL)
	{
		/* there could be many entries, so be willing to abort here */
		CHECK_FOR_INTERRUPTS();
		rumEntryInsert(&buildstate.rumstate, attnum, key, category,
					   items, nlist, &buildstate.buildStats);
	}
	MemoryContextSwitchTo(oldCtx);

	MemoryContextDelete(buildstate.funcCtx);
	MemoryContextDelete(buildstate.tmpCtx);

	/*
	 * Update metapage stats
	 */
	buildstate.buildStats.nTotalPages = RelationGetNumberOfBlocks(index);
	rumUpdateStats(index, &buildstate.buildStats, buildstate.rumstate.isBuild);

	/*
	 * Write index to xlog
	 */
	for (blkno = 0; blkno < buildstate.buildStats.nTotalPages; blkno++)
	{
		Buffer buffer;
		GenericXLogState *state;

		CHECK_FOR_INTERRUPTS();

		buffer = ReadBuffer(index, blkno);
		LockBuffer(buffer, RUM_EXCLUSIVE);

		state = GenericXLogStart(index);
		GenericXLogRegisterBuffer(state, buffer, GENERIC_XLOG_FULL_IMAGE);
		GenericXLogFinish(state);

		UnlockReleaseBuffer(buffer);
	}

	/*
	 * Return statistics
	 */
	result = (IndexBuildResult *) palloc(sizeof(IndexBuildResult));

	result->heap_tuples = reltuples;
	result->index_tuples = buildstate.indtuples;

	return result;
}


/*
 *	rumbuildempty() -- build an empty rum index in the initialization fork
 */
void
rumbuildempty(Relation index)
{
	Buffer RootBuffer,
		   MetaBuffer;
	GenericXLogState *state;

	state = GenericXLogStart(index);

	/* An empty RUM index has two pages. */
	MetaBuffer =
		ReadBufferExtended(index, INIT_FORKNUM, P_NEW, RBM_NORMAL, NULL);
	LockBuffer(MetaBuffer, BUFFER_LOCK_EXCLUSIVE);
	RootBuffer =
		ReadBufferExtended(index, INIT_FORKNUM, P_NEW, RBM_NORMAL, NULL);
	LockBuffer(RootBuffer, BUFFER_LOCK_EXCLUSIVE);

	/* Initialize and xlog metabuffer and root buffer. */
	RumInitMetabuffer(state, MetaBuffer, false);
	RumInitBuffer(state, RootBuffer, RUM_LEAF, false);

	GenericXLogFinish(state);

	/* Unlock and release the buffers. */
	UnlockReleaseBuffer(MetaBuffer);
	UnlockReleaseBuffer(RootBuffer);
}


/*
 * Insert index entries for a single indexable item during "normal"
 * (non-fast-update) insertion
 */
static void
rumHeapTupleInsert(RumState *rumstate, OffsetNumber attnum,
				   Datum value, bool isNull,
				   ItemPointer item,
				   Datum outerAddInfo,
				   bool outerAddInfoIsNull)
{
	Datum *entries;
	RumNullCategory *categories;
	int32 i,
		  nentries;
	Datum *addInfo;
	bool *addInfoIsNull;

	entries = rumExtractEntries(rumstate, attnum, value, isNull,
								&nentries, &categories, &addInfo, &addInfoIsNull);

	if (attnum == rumstate->attrnAddToColumn)
	{
		addInfo = palloc(sizeof(*addInfo) * nentries);
		addInfoIsNull = palloc(sizeof(*addInfoIsNull) * nentries);

		for (i = 0; i < nentries; i++)
		{
			addInfo[i] = outerAddInfo;
			addInfoIsNull[i] = outerAddInfoIsNull;
		}
	}

	for (i = 0; i < nentries; i++)
	{
		RumItem insert_item;

		/* Check existance of additional information attribute in index */
		if (!addInfoIsNull[i] && !rumstate->addAttrs[attnum - 1])
		{
			Form_pg_attribute attr = RumTupleDescAttr(rumstate->origTupdesc,
													  attnum - 1);

			elog(ERROR, "additional information attribute \"%s\" is not found in index",
				 NameStr(attr->attname));
		}

		memset(&insert_item, 0, sizeof(insert_item));
		insert_item.iptr = *item;
		insert_item.addInfo = addInfo[i];
		insert_item.addInfoIsNull = addInfoIsNull[i];

		rumEntryInsert(rumstate, attnum, entries[i], categories[i],
					   &insert_item, 1, NULL);
	}
}


bool
ruminsert(Relation index, Datum *values, bool *isnull,
		  ItemPointer ht_ctid, Relation heapRel,
		  IndexUniqueCheck checkUnique
#if PG_VERSION_NUM >= 140000
		  , bool indexUnchanged
#endif
#if PG_VERSION_NUM >= 100000
		  , struct IndexInfo *indexInfo
#endif
		  )
{
	RumState rumstate;
	MemoryContext oldCtx;
	MemoryContext insertCtx;
	int i;
	Datum outerAddInfo = (Datum) 0;
	bool outerAddInfoIsNull = true;

	insertCtx = RumContextCreate(CurrentMemoryContext,
								 "Rum insert temporary context");

	oldCtx = MemoryContextSwitchTo(insertCtx);

	initRumState(&rumstate, index);

	if (AttributeNumberIsValid(rumstate.attrnAttachColumn))
	{
		outerAddInfo = values[rumstate.attrnAttachColumn - 1];
		outerAddInfoIsNull = isnull[rumstate.attrnAttachColumn - 1];
	}

	for (i = 0; i < rumstate.origTupdesc->natts; i++)
	{
		rumHeapTupleInsert(&rumstate, (OffsetNumber) (i + 1),
						   values[i], isnull[i], ht_ctid,
						   outerAddInfo, outerAddInfoIsNull);
	}

	MemoryContextSwitchTo(oldCtx);
	MemoryContextDelete(insertCtx);

	return false;
}
