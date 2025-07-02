/*-------------------------------------------------------------------------
 *
 * rumbtree.c
 *	  page utilities routines for the postgres inverted index access method.
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
#include "miscadmin.h"
#include "storage/predicate.h"

#include "pg_documentdb_rum.h"

/*
 * Locks buffer by needed method for search.
 */
static int
rumTraverseLock(Buffer buffer, bool searchMode)
{
	Page page;
	int access = RUM_SHARE;

	LockBuffer(buffer, RUM_SHARE);
	page = BufferGetPage(buffer);
	if (RumPageIsLeaf(page))
	{
		if (searchMode == false)
		{
			/* we should relock our page */
			LockBuffer(buffer, RUM_UNLOCK);
			LockBuffer(buffer, RUM_EXCLUSIVE);

			/* But root can become non-leaf during relock */
			if (!RumPageIsLeaf(page))
			{
				/* restore old lock type (very rare) */
				LockBuffer(buffer, RUM_UNLOCK);
				LockBuffer(buffer, RUM_SHARE);
			}
			else
			{
				access = RUM_EXCLUSIVE;
			}
		}
	}

	return access;
}


RumBtreeStack *
rumPrepareFindLeafPage(RumBtree btree, BlockNumber blkno)
{
	RumBtreeStack *stack = (RumBtreeStack *) palloc(sizeof(RumBtreeStack));

	stack->blkno = blkno;
	stack->buffer = ReadBuffer(btree->index, stack->blkno);
	stack->parent = NULL;
	stack->predictNumber = 1;

	rumTraverseLock(stack->buffer, btree->searchMode);

	return stack;
}


/*
 * Locates leaf page contained tuple
 */
RumBtreeStack *
rumReFindLeafPage(RumBtree btree, RumBtreeStack *stack)
{
	/*
	 * Traverse the tree upwards until we sure that requested leaf page is in
	 * this subtree. Or we can stop at root page.
	 */
	while (stack->parent)
	{
		RumBtreeStack *ptr;
		Page page;
		OffsetNumber maxoff;

		LockBuffer(stack->buffer, RUM_UNLOCK);
		stack->parent->buffer =
			ReleaseAndReadBuffer(stack->buffer, btree->index, stack->parent->blkno);
		LockBuffer(stack->parent->buffer, RUM_SHARE);

		ptr = stack;
		stack = stack->parent;
		pfree(ptr);

		page = BufferGetPage(stack->buffer);
		maxoff = RumPageGetOpaque(page)->maxoff;

		/*
		 * We don't know right bound of rightmost pointer. So, we can be sure
		 * that requested leaf page is in this subtree only when requested
		 * item pointer is less than item pointer previous to rightmost.
		 */
		if (compareRumItem(btree->rumstate, btree->entryAttnum,
						   &(((PostingItem *) RumDataPageGetItem(page, maxoff -
																 1))->item),
						   &btree->items[btree->curitem]) >= 0)
		{
			break;
		}
	}

	/* Traverse tree downwards. */
	stack = rumFindLeafPage(btree, stack);
	return stack;
}


/*
 * Locates leaf page contained tuple
 */
RumBtreeStack *
rumFindLeafPage(RumBtree btree, RumBtreeStack *stack)
{
	bool isfirst = true;
	BlockNumber rootBlkno;

	if (!stack)
	{
		stack = rumPrepareFindLeafPage(btree, RUM_ROOT_BLKNO);
	}
	rootBlkno = stack->blkno;

	for (;;)
	{
		Page page;
		BlockNumber child;
		int access = RUM_SHARE;

		stack->off = InvalidOffsetNumber;

		page = BufferGetPage(stack->buffer);

		if (isfirst)
		{
			if (RumPageIsLeaf(page) && !btree->searchMode)
			{
				access = RUM_EXCLUSIVE;
			}
			isfirst = false;
		}
		else
		{
			access = rumTraverseLock(stack->buffer, btree->searchMode);
		}

		/*
		 * ok, page is correctly locked, we should check to move right ..,
		 * root never has a right link, so small optimization
		 */
		while (btree->fullScan == false && stack->blkno != rootBlkno &&
			   btree->isMoveRight(btree, page))
		{
			BlockNumber rightlink = RumPageGetOpaque(page)->rightlink;

			if (rightlink == InvalidBlockNumber)
			{
				/* rightmost page */
				break;
			}

			stack->buffer = rumStep(stack->buffer, btree->index, access,
									ForwardScanDirection);
			stack->blkno = rightlink;
			page = BufferGetPage(stack->buffer);
		}

		if (RumPageIsLeaf(page))    /* we found, return locked page */
		{
			return stack;
		}

		/* now we have correct buffer, try to find child */
		child = btree->findChildPage(btree, stack);

		LockBuffer(stack->buffer, RUM_UNLOCK);
		Assert(child != InvalidBlockNumber);
		Assert(stack->blkno != child);

		if (btree->searchMode)
		{
			/* in search mode we may forget path to leaf */
			RumBtreeStack *ptr = (RumBtreeStack *) palloc(sizeof(RumBtreeStack));
			Buffer buffer = ReleaseAndReadBuffer(stack->buffer, btree->index, child);

			ptr->parent = stack;
			ptr->predictNumber = stack->predictNumber;
			stack->buffer = InvalidBuffer;

			stack = ptr;
			stack->blkno = child;
			stack->buffer = buffer;
		}
		else
		{
			RumBtreeStack *ptr = (RumBtreeStack *) palloc(sizeof(RumBtreeStack));

			ptr->parent = stack;
			stack = ptr;
			stack->blkno = child;
			stack->buffer = ReadBuffer(btree->index, stack->blkno);
			stack->predictNumber = 1;
		}
	}
}


/*
 * Step from current page.
 */
Buffer
rumStep(Buffer buffer, Relation index, int lockmode,
		ScanDirection scanDirection)
{
	Buffer nextbuffer;
	Page page = BufferGetPage(buffer);
	bool isLeaf = RumPageIsLeaf(page);
	bool isData = RumPageIsData(page);
	BlockNumber blkno;

	blkno = (ScanDirectionIsForward(scanDirection)) ?
			RumPageGetOpaque(page)->rightlink :
			RumPageGetOpaque(page)->leftlink;

	if (blkno == InvalidBlockNumber)
	{
		UnlockReleaseBuffer(buffer);
		return InvalidBuffer;
	}

	nextbuffer = ReadBuffer(index, blkno);
	UnlockReleaseBuffer(buffer);
	LockBuffer(nextbuffer, lockmode);

	/* Sanity check that the page we stepped to is of similar kind. */
	page = BufferGetPage(nextbuffer);
	if (isLeaf != RumPageIsLeaf(page) || isData != RumPageIsData(page))
	{
		elog(ERROR, "right sibling of RUM page is of different type");
	}

	/*
	 * Given the proper lock sequence above, we should never land on a deleted
	 * page.
	 */
	if (RumPageIsDeleted(page))
	{
		elog(ERROR, "%s sibling of RUM page was deleted",
			 ScanDirectionIsForward(scanDirection) ? "right" : "left");
	}

	return nextbuffer;
}


void
freeRumBtreeStack(RumBtreeStack *stack)
{
	while (stack)
	{
		RumBtreeStack *tmp = stack->parent;

		if (stack->buffer != InvalidBuffer)
		{
			ReleaseBuffer(stack->buffer);
		}

		pfree(stack);
		stack = tmp;
	}
}


/*
 * Try to find parent for current stack position, returns correct
 * parent and child's offset in  stack->parent.
 * Function should never release root page to prevent conflicts
 * with vacuum process
 */
void
rumFindParents(RumBtree btree, RumBtreeStack *stack,
			   BlockNumber rootBlkno)
{
	Page page;
	Buffer buffer;
	BlockNumber blkno,
				leftmostBlkno;
	OffsetNumber offset;
	RumBtreeStack *root = stack->parent;
	RumBtreeStack *ptr;

	if (!root)
	{
		/* XLog mode... */
		root = (RumBtreeStack *) palloc(sizeof(RumBtreeStack));
		root->blkno = rootBlkno;
		root->buffer = ReadBuffer(btree->index, rootBlkno);
		LockBuffer(root->buffer, RUM_EXCLUSIVE);
		root->parent = NULL;
	}
	else
	{
		/*
		 * find root, we should not release root page until update is
		 * finished!!
		 */
		while (root->parent)
		{
			ReleaseBuffer(root->buffer);
			root = root->parent;
		}

		Assert(root->blkno == rootBlkno);
		Assert(BufferGetBlockNumber(root->buffer) == rootBlkno);
		LockBuffer(root->buffer, RUM_EXCLUSIVE);
	}
	root->off = InvalidOffsetNumber;

	page = BufferGetPage(root->buffer);
	Assert(!RumPageIsLeaf(page));

	/* check trivial case */
	if ((root->off = btree->findChildPtr(btree, page, stack->blkno,
										 InvalidOffsetNumber)) != InvalidOffsetNumber)
	{
		stack->parent = root;
		return;
	}

	blkno = btree->getLeftMostPage(btree, page);
	LockBuffer(root->buffer, RUM_UNLOCK);
	Assert(blkno != InvalidBlockNumber);

	for (;;)
	{
		buffer = ReadBuffer(btree->index, blkno);
		LockBuffer(buffer, RUM_EXCLUSIVE);
		page = BufferGetPage(buffer);
		if (RumPageIsLeaf(page))
		{
			elog(ERROR, "Lost path");
		}

		leftmostBlkno = btree->getLeftMostPage(btree, page);

		while ((offset = btree->findChildPtr(btree, page, stack->blkno,
											 InvalidOffsetNumber)) == InvalidOffsetNumber)
		{
			blkno = RumPageGetOpaque(page)->rightlink;
			if (blkno == InvalidBlockNumber)
			{
				UnlockReleaseBuffer(buffer);
				break;
			}
			buffer = rumStep(buffer, btree->index, RUM_EXCLUSIVE,
							 ForwardScanDirection);
			page = BufferGetPage(buffer);
		}

		if (blkno != InvalidBlockNumber)
		{
			ptr = (RumBtreeStack *) palloc(sizeof(RumBtreeStack));
			ptr->blkno = blkno;
			ptr->buffer = buffer;
			ptr->parent = root; /* it's may be wrong, but in next call we will
			                     * correct */
			ptr->off = offset;
			stack->parent = ptr;
			return;
		}

		blkno = leftmostBlkno;
	}
}


/*
 * Insert value (stored in RumBtree) to tree described by stack
 *
 * During an index build, buildStats is non-null and the counters
 * it contains should be incremented as needed.
 *
 * NB: the passed-in stack is freed, as though by freeRumBtreeStack.
 */
void
rumInsertValue(Relation index, RumBtree btree, RumBtreeStack *stack,
			   GinStatsData *buildStats)
{
	RumBtreeStack *parent;
	BlockNumber rootBlkno;
	Page page,
		 rpage,
		 lpage;
	GenericXLogState *state = NULL;

	/* extract root BlockNumber from stack */
	Assert(stack != NULL);
	parent = stack;
	while (parent->parent)
	{
		parent = parent->parent;
	}
	rootBlkno = parent->blkno;
	Assert(BlockNumberIsValid(rootBlkno));

	/* this loop crawls up the stack until the insertion is complete */
	for (;;)
	{
		BlockNumber savedLeftLink,
					savedRightLink;

		page = BufferGetPage(stack->buffer);
		savedLeftLink = RumPageGetOpaque(page)->leftlink;
		savedRightLink = RumPageGetOpaque(page)->rightlink;

		if (btree->isEnoughSpace(btree, stack->buffer, stack->off))
		{
			if (btree->rumstate->isBuild)
			{
				page = BufferGetPage(stack->buffer);
				START_CRIT_SECTION();
			}
			else
			{
				state = GenericXLogStart(index);
				page = GenericXLogRegisterBuffer(state, stack->buffer, 0);
			}

			btree->placeToPage(btree, page, stack->off);

			if (btree->rumstate->isBuild)
			{
				MarkBufferDirty(stack->buffer);
				END_CRIT_SECTION();
			}
			else
			{
				GenericXLogFinish(state);
			}

			LockBuffer(stack->buffer, RUM_UNLOCK);
			freeRumBtreeStack(stack);

			return;
		}
		else
		{
			Buffer rbuffer = RumNewBuffer(btree->index);
			Page newlpage;

			/* During index build, count the newly-split page */
			if (buildStats)
			{
				if (btree->isData)
				{
					buildStats->nDataPages++;
				}
				else
				{
					buildStats->nEntryPages++;
				}
			}

			parent = stack->parent;

			if (parent == NULL)
			{
				Buffer lbuffer;

				if (btree->rumstate->isBuild)
				{
					page = BufferGetPage(stack->buffer);
					rpage = BufferGetPage(rbuffer);
				}
				else
				{
					state = GenericXLogStart(index);

					page = GenericXLogRegisterBuffer(state, stack->buffer, 0);
					rpage = GenericXLogRegisterBuffer(state, rbuffer,
													  GENERIC_XLOG_FULL_IMAGE);
				}

				/*
				 * newlpage is a pointer to memory page, it doesn't associate
				 * with buffer, stack->buffer should be untouched
				 */
				newlpage = btree->splitPage(btree, stack->buffer, rbuffer,
											page, rpage, stack->off);

				/*
				 * split root, so we need to allocate new left page and place
				 * pointer on root to left and right page
				 */
				lbuffer = RumNewBuffer(btree->index);
				if (btree->rumstate->isBuild)
				{
					lpage = BufferGetPage(lbuffer);
				}
				else
				{
					lpage = GenericXLogRegisterBuffer(state, lbuffer,
													  GENERIC_XLOG_FULL_IMAGE);
				}

				RumPageGetOpaque(rpage)->rightlink = InvalidBlockNumber;
				RumPageGetOpaque(newlpage)->leftlink = InvalidBlockNumber;
				RumPageGetOpaque(rpage)->leftlink = BufferGetBlockNumber(lbuffer);
				RumPageGetOpaque(newlpage)->rightlink = BufferGetBlockNumber(rbuffer);

				RumInitPage(page, RumPageGetOpaque(newlpage)->flags & ~RUM_LEAF,
							BufferGetPageSize(stack->buffer));
				PageRestoreTempPage(newlpage, lpage);
				btree->fillRoot(btree, stack->buffer, lbuffer, rbuffer,
								page, lpage, rpage);

				PredicateLockPageSplit(btree->index,
									   BufferGetBlockNumber(stack->buffer),
									   BufferGetBlockNumber(lbuffer));

				PredicateLockPageSplit(btree->index,
									   BufferGetBlockNumber(stack->buffer),
									   BufferGetBlockNumber(rbuffer));

				if (btree->rumstate->isBuild)
				{
					START_CRIT_SECTION();
					MarkBufferDirty(rbuffer);
					MarkBufferDirty(lbuffer);
					MarkBufferDirty(stack->buffer);
				}
				else
				{
					GenericXLogFinish(state);
				}

				UnlockReleaseBuffer(rbuffer);
				UnlockReleaseBuffer(lbuffer);
				LockBuffer(stack->buffer, RUM_UNLOCK);

				if (btree->rumstate->isBuild)
				{
					END_CRIT_SECTION();
				}

				freeRumBtreeStack(stack);

				/* During index build, count the newly-added root page */
				if (buildStats)
				{
					if (btree->isData)
					{
						buildStats->nDataPages++;
					}
					else
					{
						buildStats->nEntryPages++;
					}
				}

				return;
			}
			else
			{
				BlockNumber rightrightBlkno = InvalidBlockNumber;
				Buffer rightrightBuffer = InvalidBuffer;

				/* split non-root page */
				if (btree->rumstate->isBuild)
				{
					lpage = BufferGetPage(stack->buffer);
					rpage = BufferGetPage(rbuffer);
				}
				else
				{
					state = GenericXLogStart(index);

					lpage = GenericXLogRegisterBuffer(state, stack->buffer, 0);
					rpage = GenericXLogRegisterBuffer(state, rbuffer, 0);
				}

				rightrightBlkno = RumPageGetOpaque(lpage)->rightlink;

				/*
				 * newlpage is a pointer to memory page, it doesn't associate
				 * with buffer, stack->buffer should be untouched
				 */
				newlpage = btree->splitPage(btree, stack->buffer, rbuffer,
											lpage, rpage, stack->off);

				RumPageGetOpaque(rpage)->rightlink = savedRightLink;
				RumPageGetOpaque(newlpage)->leftlink = savedLeftLink;
				RumPageGetOpaque(rpage)->leftlink = BufferGetBlockNumber(stack->buffer);
				RumPageGetOpaque(newlpage)->rightlink = BufferGetBlockNumber(rbuffer);

				PredicateLockPageSplit(btree->index,
									   BufferGetBlockNumber(stack->buffer),
									   BufferGetBlockNumber(rbuffer));

				/*
				 * it's safe because we don't have right-to-left walking
				 * with locking bth pages except vacuum. But vacuum will
				 * try to lock all pages with conditional lock
				 */
				if (rightrightBlkno != InvalidBlockNumber)
				{
					Page rightrightPage;

					rightrightBuffer = ReadBuffer(btree->index,
												  rightrightBlkno);

					LockBuffer(rightrightBuffer, RUM_EXCLUSIVE);
					if (btree->rumstate->isBuild)
					{
						rightrightPage = BufferGetPage(rightrightBuffer);
					}
					else
					{
						rightrightPage =
							GenericXLogRegisterBuffer(state, rightrightBuffer, 0);
					}

					RumPageGetOpaque(rightrightPage)->leftlink =
						BufferGetBlockNumber(rbuffer);
				}

				if (btree->rumstate->isBuild)
				{
					START_CRIT_SECTION();
				}
				PageRestoreTempPage(newlpage, lpage);

				if (btree->rumstate->isBuild)
				{
					MarkBufferDirty(rbuffer);
					MarkBufferDirty(stack->buffer);
					if (rightrightBlkno != InvalidBlockNumber)
					{
						MarkBufferDirty(rightrightBuffer);
					}
					END_CRIT_SECTION();
				}
				else
				{
					GenericXLogFinish(state);
				}

				UnlockReleaseBuffer(rbuffer);
				if (rightrightBlkno != InvalidBlockNumber)
				{
					UnlockReleaseBuffer(rightrightBuffer);
				}
			}
		}

		btree->isDelete = false;

		/* search parent to lock */
		LockBuffer(parent->buffer, RUM_EXCLUSIVE);

		/* move right if it's needed */
		page = BufferGetPage(parent->buffer);
		while ((parent->off = btree->findChildPtr(btree, page, stack->blkno,
												  parent->off)) == InvalidOffsetNumber)
		{
			BlockNumber rightlink = RumPageGetOpaque(page)->rightlink;

			if (rightlink == InvalidBlockNumber)
			{
				/*
				 * rightmost page, but we don't find parent, we should use
				 * plain search...
				 */
				LockBuffer(parent->buffer, RUM_UNLOCK);
				rumFindParents(btree, stack, rootBlkno);
				parent = stack->parent;
				Assert(parent != NULL);
				break;
			}

			parent->buffer = rumStep(parent->buffer, btree->index,
									 RUM_EXCLUSIVE, ForwardScanDirection);
			parent->blkno = rightlink;
			page = BufferGetPage(parent->buffer);
		}

		UnlockReleaseBuffer(stack->buffer);
		pfree(stack);
		stack = parent;
	}
}
