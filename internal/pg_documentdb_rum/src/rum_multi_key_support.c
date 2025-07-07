/* -------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/rum_multi_key_support.c
 *
 * This is currently a naive implementation based off of tidbitmap.
 * This supports sorts until the point of being lossy, at which point
 * it fails.
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"
#include "miscadmin.h"
#include "pg_documentdb_rum.h"


/* CODESYNC: keep this up to date with documentdb_rum.h */
typedef void *(*CreateIndexArrayTrackerState)(void);
typedef bool (*IndexArrayTrackerAdd)(void *state, ItemPointer item);
typedef void (*FreeIndexArrayTrackerState)(void *);

typedef struct RumIndexArrayStateFuncs
{
	/* Create opaque state to manage entries in this specific index scan */
	CreateIndexArrayTrackerState createState;

	/* Add an item to the index scan and return whether or not it is new or existing */
	IndexArrayTrackerAdd addItem;

	/* Frees the temporary state used for the adding of items */
	FreeIndexArrayTrackerState freeState;
} RumIndexArrayStateFuncs;


/* Custom bitmap multikey implementation */
typedef struct DocumentDBRumBitmapState
{
	TIDBitmap *bitmap;
} DocumentDBRumBitmapState;

static void * documentdb_rum_create_multi_key_state(void);
static bool documentdb_rum_multi_key_add_item(void *state, ItemPointer item);
static void documentdb_rum_multi_key_free_state(void *state);


static RumIndexArrayStateFuncs documentdb_array_state_funcs = {
	.createState = documentdb_rum_create_multi_key_state,
	.addItem = documentdb_rum_multi_key_add_item,
	.freeState = documentdb_rum_multi_key_free_state,
};

/* Exports */
extern PGDLLEXPORT const RumIndexArrayStateFuncs * get_rum_index_array_state_funcs(void);


PGDLLEXPORT const RumIndexArrayStateFuncs *
get_rum_index_array_state_funcs(void)
{
	return &documentdb_array_state_funcs;
}


static void *
documentdb_rum_create_multi_key_state(void)
{
	DocumentDBRumBitmapState *state = palloc(sizeof(DocumentDBRumBitmapState));
	state->bitmap = tbm_create(work_mem, NULL);
	return state;
}


static bool
documentdb_rum_multi_key_add_item(void *state, ItemPointer item)
{
	bool hasTuple = false;
	DocumentDBRumBitmapState *bitmap_state = (DocumentDBRumBitmapState *) state;

	/* First add it as the only item on to the recheck bitmap */
	TIDBitmap *perTupleBitmap = tbm_create(work_mem, NULL);
	tbm_add_tuples(perTupleBitmap, item, 1, false);

	/* next intersect it with the main tbm */
	tbm_intersect(perTupleBitmap, bitmap_state->bitmap);
	if (tbm_is_empty(perTupleBitmap))
	{
		tbm_add_tuples(bitmap_state->bitmap, item, 1, false);
	}
	else
	{
		/* The TID exists in the main bitmap - two cases
		 * it exists or it's lossy - check which one.
		 */
		TBMIterator *iterator = tbm_begin_iterate(perTupleBitmap);
		TBMIterateResult *result = tbm_iterate(iterator);

		hasTuple = true;
		if (result->ntuples < 0 || result->recheck)
		{
			ereport(ERROR, (errmsg("Cannot iterate over a lossy check on order by")));
		}

		tbm_end_iterate(iterator);
	}

	tbm_free(perTupleBitmap);
	return !hasTuple;
}


static void
documentdb_rum_multi_key_free_state(void *state)
{
	DocumentDBRumBitmapState *bitmap_state = (DocumentDBRumBitmapState *) state;
	if (bitmap_state->bitmap != NULL)
	{
		tbm_free(bitmap_state->bitmap);
	}

	pfree(bitmap_state);
}
