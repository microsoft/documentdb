/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * include/index_am/index_am_utils.h
 *
 * Common declarations for RUM specific helper functions.
 *
 *-------------------------------------------------------------------------
 */

#ifndef INDEX_AM_UTILS_H
#define INDEX_AM_UTILS_H

#include <postgres.h>
#include <utils/rel.h>
#include "metadata/metadata_cache.h"
#include "utils/version_utils.h"

struct IndexScanDescData;
struct ExplainState;

/*
 * Data structure for an alternative index acess method for indexing bosn.
 * It contains the indexing capability and various utility function.
 */
typedef struct
{
	bool is_single_path_index_supported;
	bool is_unique_index_supported;
	bool is_wild_card_supported;
	bool is_composite_index_supported;
	bool is_text_index_supported;
	bool is_hashed_index_supported;
	bool is_order_by_supported;
	Oid (*get_am_oid)(void);
	Oid (*get_single_path_op_family_oid)(void);
	Oid (*get_composite_path_op_family_oid)(void);

	/* optional func to add explain output */
	void (*add_explain_output)(struct IndexScanDescData *indexScanDesc, struct
							   ExplainState *explainState);
	const char *am_name;
} BsonIndexAmEntry;

#define MAX_ALTERNATE_INDEX_AMS 5

/*
 * Registers an bson index access method at system start time.
 */
void RegisterIndexAm(BsonIndexAmEntry indexAmEntry);

int SetDynamicIndexAmOidsAndGetCount(Datum *indexAmArray, int32_t indexAmArraySize);

/*
 * Gets an index AM entry by name.
 */
const BsonIndexAmEntry * GetBsonIndexAmByIndexAmName(const char *index_am_name);


/*
 * Is the Index Acess Method used for indexing bson (as opposed to indexing TEXT, Vector, Points etc)
 * as indicated by enum MongoIndexKind_Regular.
 */
bool IsBsonRegularIndexAm(Oid indexAm);

/*
 * Whether the index relation was created via a composite index opclass
 */
bool IsCompositeOpClass(Relation indexRelation);

/*
 * Whether the Oid of the oprator family points to a single path operator family.
 */
bool IsSinglePathOpFamilyOid(Oid opFamilyOid);

bool IsOrderBySupportedOnOpClass(Oid indexAm, Oid IndexPathOpFamilyAm);


void TryExplainByIndexAm(struct IndexScanDescData *scan, struct ExplainState *es);
#endif
