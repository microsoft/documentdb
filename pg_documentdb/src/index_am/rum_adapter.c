/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/index_am/rum_adapter.c
 *
 * Rum specific implementation overrides for the documentdb_api.
 *
 *-------------------------------------------------------------------------
 */


#include <postgres.h>
#include <fmgr.h>
#include <miscadmin.h>
#include <utils/index_selfuncs.h>
#include <utils/selfuncs.h>
#include <utils/lsyscache.h>
#include <access/relscan.h>
#include <utils/rel.h>
#include <access/generic_xlog.h>
#include <storage/bufmgr.h>
#include <access/gin.h>
#include "math.h"

#include "api_hooks.h"
#include "planner/mongo_query_operator.h"
#include "opclass/bson_gin_index_mgmt.h"
#include "index_am/documentdb_rum.h"
#include "metadata/metadata_cache.h"
#include "opclass/bson_gin_composite_scan.h"


/* Fixed offset pages (Copied from rum.h) */
#define RUM_METAPAGE_BLKNO (0)
#define RUM_EXCLUSIVE BUFFER_LOCK_EXCLUSIVE
#define RUM_SHARE BUFFER_LOCK_SHARE

/*  This is a copy of the RUM metadata page struct
 * CODESYNC: Keep this in sync with the rum version being used.
 */
typedef struct RumMetaPageData
{
	/*
	 * RUM version number
	 */
	uint32 rumVersion;

	/*
	 * Pointers to head and tail of pending list, which consists of RUM_LIST
	 * pages.  These store fast-inserted entries that haven't yet been moved
	 * into the regular RUM structure.
	 * XXX unused - pending list is removed.
	 */
	BlockNumber head;
	BlockNumber tail;

	/*
	 * Free space in bytes in the pending list's tail page.
	 */
	uint32 tailFreeSize;

	/*
	 * We store both number of pages and number of heap tuples that are in the
	 * pending list.
	 */
	BlockNumber nPendingPages;
	int64 nPendingHeapTuples;

	/*
	 * Statistics for planner use (accurate as of last VACUUM)
	 */
	BlockNumber nTotalPages;
	BlockNumber nEntryPages;
	BlockNumber nDataPages;
	int64 nEntries;
}   RumMetaPageData;

#define RumPageGetMeta(p) \
	((RumMetaPageData *) PageGetContents(p))

/*
 * SECTION: New Methods / data
 */

/*
 * This is similar to rumUpdateStats but it reuses the
 * nPendingTuples field which is unused in RUM to set
 * the multi-key status.
 */
void
RumUpdateMultiKeyStatus(Relation index)
{
	/* First do a get to see if we even need to update */
	bool isMultiKey = RumGetMultikeyStatus(index);
	if (isMultiKey)
	{
		return;
	}

	Buffer metaBuffer;
	Page metapage;
	RumMetaPageData *metadata;
	GenericXLogState *state;

	metaBuffer = ReadBuffer(index, RUM_METAPAGE_BLKNO);
	LockBuffer(metaBuffer, RUM_EXCLUSIVE);

	state = GenericXLogStart(index);
	metapage = GenericXLogRegisterBuffer(state, metaBuffer, 0);
	metadata = RumPageGetMeta(metapage);

	/* Set pending heap tuples to 1 to indicate this is a multi-key index */
	metadata->nPendingHeapTuples = 1;

	GenericXLogFinish(state);
	UnlockReleaseBuffer(metaBuffer);
}


bool
RumGetMultikeyStatus(Relation indexRelation)
{
	Buffer metabuffer;
	Page metapage;
	RumMetaPageData *metadata;
	bool hasMultiKeyPaths = false;

	metabuffer = ReadBuffer(indexRelation, RUM_METAPAGE_BLKNO);
	LockBuffer(metabuffer, RUM_SHARE);
	metapage = BufferGetPage(metabuffer);
	metadata = RumPageGetMeta(metapage);
	hasMultiKeyPaths = metadata->nPendingHeapTuples > 0;
	UnlockReleaseBuffer(metabuffer);

	return hasMultiKeyPaths;
}
