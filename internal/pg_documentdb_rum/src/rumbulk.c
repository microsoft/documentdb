/*-------------------------------------------------------------------------
 *
 * rumbulk.c
 *	  routines for fast build of inverted index
 *
 *
 * Portions Copyright (c) 2015-2022, Postgres Professional
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/datum.h"

#include "pg_documentdb_rum.h"

#define DEF_NENTRY 2048         /* RumEntryAccumulator allocation quantum */
#define DEF_NPTR 5              /* ItemPointer initial allocation quantum */

/* PostgreSQL pre 10 has different names for this functions */
#if PG_VERSION_NUM <= 100006 || PG_VERSION_NUM == 110000
#define rbt_create(node_size, comparator, combiner, allocfunc, freefunc, arg) \
	(rb_create(node_size, comparator, combiner, allocfunc, freefunc, arg))
#define rbt_insert(rbt, data, isNew) \
	(rb_insert(rbt, data, isNew))
#endif


/* Combiner function for rbtree.c */
static void
rumCombineData(RBTNode *existing, const RBTNode *newdata, void *arg)
{
	RumEntryAccumulator *eo = (RumEntryAccumulator *) existing;
	const RumEntryAccumulator *en = (const RumEntryAccumulator *) newdata;
	BuildAccumulator *accum = (BuildAccumulator *) arg;

	/*
	 * Note this code assumes that newdata contains only one itempointer.
	 */
	if (eo->count >= eo->maxcount)
	{
		accum->allocatedMemory -= GetMemoryChunkSpace(eo->list);
		eo->maxcount *= 2;
		eo->list = (RumItem *) repalloc(eo->list, sizeof(RumItem) * eo->maxcount);
		accum->allocatedMemory += GetMemoryChunkSpace(eo->list);
	}

	/*
	 * If item pointers are not ordered, they will need to be sorted later
	 * Note: if useAlternativeOrder == true then shouldSort should be true
	 * because anyway list isn't right ordered and code below could not check
	 * it correctly
	 */
	if (accum->rumstate->useAlternativeOrder == false && eo->shouldSort == false)
	{
		int res;

		res = rumCompareItemPointers(&eo->list[eo->count - 1].iptr,
									 &en->list->iptr);
		Assert(res != 0);

		if (res > 0)
		{
			eo->shouldSort = true;
		}
	}

	eo->list[eo->count] = en->list[0];
	eo->count++;
}


/* Comparator function for rbtree.c */
static int
cmpEntryAccumulator(const RBTNode *a, const RBTNode *b, void *arg)
{
	const RumEntryAccumulator *ea = (const RumEntryAccumulator *) a;
	const RumEntryAccumulator *eb = (const RumEntryAccumulator *) b;
	BuildAccumulator *accum = (BuildAccumulator *) arg;

	return rumCompareAttEntries(accum->rumstate,
								ea->attnum, ea->key, ea->category,
								eb->attnum, eb->key, eb->category);
}


/* Allocator function for rbtree.c */
static RBTNode *
rumAllocEntryAccumulator(void *arg)
{
	BuildAccumulator *accum = (BuildAccumulator *) arg;
	RumEntryAccumulator *ea;

	/*
	 * Allocate memory by rather big chunks to decrease overhead.  We have no
	 * need to reclaim RBTNodes individually, so this costs nothing.
	 */
	if (accum->entryallocator == NULL || accum->eas_used >= DEF_NENTRY)
	{
		accum->entryallocator = palloc(sizeof(RumEntryAccumulator) * DEF_NENTRY);
		accum->allocatedMemory += GetMemoryChunkSpace(accum->entryallocator);
		accum->eas_used = 0;
	}

	/* Allocate new RBTNode from current chunk */
	ea = accum->entryallocator + accum->eas_used;
	accum->eas_used++;

	return (RBTNode *) ea;
}


void
rumInitBA(BuildAccumulator *accum)
{
	/* accum->rumstate is intentionally not set here */
	accum->allocatedMemory = 0;
	accum->entryallocator = NULL;
	accum->eas_used = 0;
	accum->tree = rbt_create(sizeof(RumEntryAccumulator),
							 cmpEntryAccumulator,
							 rumCombineData,
							 rumAllocEntryAccumulator,
							 NULL,      /* no freefunc needed */
							 (void *) accum);
}


/*
 * This is basically the same as datumCopy(), but extended to count
 * palloc'd space in accum->allocatedMemory.
 */
static Datum
getDatumCopy(BuildAccumulator *accum, OffsetNumber attnum, Datum value)
{
	Form_pg_attribute att = RumTupleDescAttr(accum->rumstate->origTupdesc, attnum - 1);
	Datum res;

	if (att->attbyval)
	{
		res = value;
	}
	else
	{
		res = datumCopy(value, false, att->attlen);
		accum->allocatedMemory += GetMemoryChunkSpace(DatumGetPointer(res));
	}
	return res;
}


/*
 * Find/store one entry from indexed value.
 */
static void
rumInsertBAEntry(BuildAccumulator *accum,
				 ItemPointer heapptr, OffsetNumber attnum,
				 Datum key, Datum addInfo, bool addInfoIsNull,
				 RumNullCategory category)
{
	RumEntryAccumulator eatmp;
	RumEntryAccumulator *ea;
	bool isNew;
	RumItem item;

	/*
	 * For the moment, fill only the fields of eatmp that will be looked at by
	 * cmpEntryAccumulator or rumCombineData.
	 */
	eatmp.attnum = attnum;
	eatmp.key = key;
	eatmp.category = category;

	/* temporarily set up single-entry itempointer list */
	eatmp.list = &item;
	memset(&item, 0, sizeof(item));
	item.iptr = *heapptr;
	item.addInfo = addInfo;
	item.addInfoIsNull = addInfoIsNull;

	ea = (RumEntryAccumulator *) rbt_insert(accum->tree, (RBTNode *) &eatmp,
											&isNew);

	if (isNew)
	{
		/*
		 * Finish initializing new tree entry, including making permanent
		 * copies of the datum (if it's not null) and itempointer.
		 */
		if (category == RUM_CAT_NORM_KEY)
		{
			ea->key = getDatumCopy(accum, attnum, key);
		}
		ea->maxcount = DEF_NPTR;
		ea->count = 1;

		/*
		 * if useAlternativeOrder = true then anyway we need to sort list, but
		 * by setting shouldSort we prevent incorrect comparison in
		 * rumCombineData()
		 */
		ea->shouldSort = (accum->rumstate->useAlternativeOrder &&
						  attnum == accum->rumstate->attrnAddToColumn);
		ea->list = (RumItem *) palloc(sizeof(RumItem) * DEF_NPTR);
		ea->list[0].iptr = *heapptr;
		ea->list[0].addInfo = addInfo;
		ea->list[0].addInfoIsNull = addInfoIsNull;
		accum->allocatedMemory += GetMemoryChunkSpace(ea->list);
	}
	else
	{
		/*
		 * rumCombineData did everything needed.
		 */
	}
}


/*
 * Insert the entries for one heap pointer.
 *
 * Since the entries are being inserted into a balanced binary tree, you
 * might think that the order of insertion wouldn't be critical, but it turns
 * out that inserting the entries in sorted order results in a lot of
 * rebalancing operations and is slow.  To prevent this, we attempt to insert
 * the nodes in an order that will produce a nearly-balanced tree if the input
 * is in fact sorted.
 *
 * We do this as follows.  First, we imagine that we have an array whose size
 * is the smallest power of two greater than or equal to the actual array
 * size.  Second, we insert the middle entry of our virtual array into the
 * tree; then, we insert the middles of each half of our virtual array, then
 * middles of quarters, etc.
 */
void
rumInsertBAEntries(BuildAccumulator *accum,
				   ItemPointer heapptr, OffsetNumber attnum,
				   Datum *entries, Datum *addInfo, bool *addInfoIsNull,
				   RumNullCategory *categories, int32 nentries)
{
	uint32 step = nentries;

	if (nentries <= 0)
	{
		return;
	}

	Assert(ItemPointerIsValid(heapptr) && attnum >= FirstOffsetNumber);

	/*
	 * step will contain largest power of 2 and <= nentries
	 */
	step |= (step >> 1);
	step |= (step >> 2);
	step |= (step >> 4);
	step |= (step >> 8);
	step |= (step >> 16);
	step >>= 1;
	step++;

	while (step > 0)
	{
		int i;

		for (i = step - 1; i < nentries && i >= 0; i += step << 1 /* *2 */)
		{
			rumInsertBAEntry(accum, heapptr, attnum,
							 entries[i], addInfo[i], addInfoIsNull[i], categories[i]);
		}

		step >>= 1;             /* /2 */
	}
}


static int
qsortCompareItemPointers(const void *a, const void *b)
{
	int res = rumCompareItemPointers((ItemPointer) a, (ItemPointer) b);

	/* Assert that there are no equal item pointers being sorted */
	Assert(res != 0);
	return res;
}


static AttrNumber AttrNumberQsort = 0;

static int
qsortCompareRumItem(const void *a, const void *b, void *arg)
{
	return compareRumItem(arg, AttrNumberQsort, a, b);
}


/* Prepare to read out the rbtree contents using rumGetBAEntry */
void
rumBeginBAScan(BuildAccumulator *accum)
{
#if (PG_VERSION_NUM > 100006 && PG_VERSION_NUM < 110000) || PG_VERSION_NUM >= 110001
	rbt_begin_iterate(accum->tree, LeftRightWalk, &accum->tree_walk);
#elif PG_VERSION_NUM >= 100000
	rb_begin_iterate(accum->tree, LeftRightWalk, &accum->tree_walk);
#else
	rb_begin_iterate(accum->tree, LeftRightWalk);
#endif
}


/*
 * Get the next entry in sequence from the BuildAccumulator's rbtree.
 * This consists of a single key datum and a list (array) of one or more
 * heap TIDs in which that key is found.  The list is guaranteed sorted.
 */
RumItem *
rumGetBAEntry(BuildAccumulator *accum,
			  OffsetNumber *attnum, Datum *key, RumNullCategory *category,
			  uint32 *n)
{
	RumEntryAccumulator *entry;
	RumItem *list;

#if (PG_VERSION_NUM > 100006 && PG_VERSION_NUM < 110000) || PG_VERSION_NUM >= 110001
	entry = (RumEntryAccumulator *) rbt_iterate(&accum->tree_walk);
#elif PG_VERSION_NUM >= 100000
	entry = (RumEntryAccumulator *) rb_iterate(&accum->tree_walk);
#else
	entry = (RumEntryAccumulator *) rb_iterate(accum->tree);
#endif

	if (entry == NULL)
	{
		return NULL;            /* no more entries */
	}
	*attnum = entry->attnum;
	*key = entry->key;
	*category = entry->category;
	list = entry->list;
	*n = entry->count;

	Assert(list != NULL && entry->count > 0);

	if (entry->count > 1)
	{
		AttrNumberQsort = entry->attnum;

		if (accum->rumstate->useAlternativeOrder &&
			entry->attnum == accum->rumstate->attrnAddToColumn)
		{
			qsort_arg(list, entry->count, sizeof(RumItem),
					  qsortCompareRumItem, accum->rumstate);
		}
		else if (entry->shouldSort)
		{
			qsort(list, entry->count, sizeof(RumItem), qsortCompareItemPointers);
		}
	}

	return list;
}
