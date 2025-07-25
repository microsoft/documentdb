/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * include/commands/delete.h
 *
 * Exports related to implementation of a single-document delete.
 *
 *-------------------------------------------------------------------------
 */
#ifndef DELETE_H
#define DELETE_H

#include <postgres.h>

#include "collation/collation.h"
#include "metadata/collection.h"


/*
 * DeleteOneParams describes delete operation for a single document.
 */
typedef struct
{
	/* list of Deletions */
	const bson_value_t *query;

	/* sort order to use when selecting 1 row */
	const bson_value_t *sort;

	/* whether to return deleted document */
	bool returnDeletedDocument;

	/* fields to return if returning a document */
	const bson_value_t *returnFields;

	/* parsed variable spec*/
	const bson_value_t *variableSpec;

	/* collation string */
	const char collationString[MAX_ICU_COLLATION_LENGTH];
} DeleteOneParams;


/*
 * DeleteOneRow reflects the result of a single-row delete
 * on a single shard.
 */
typedef struct
{
	/* whether one row matched the query and was deleted */
	bool isRowDeleted;

	/* object_id of the deleted document (only used within delete_one) */
	pgbson *objectId;

	/* value of the deleted (and maybe projected) document, if requested and matched any */
	pgbson *resultDeletedDocument;
} DeleteOneResult;


void CallDeleteOne(MongoCollection *collection, DeleteOneParams *deleteOneParams,
				   int64 shardKeyHash, text *transactionId, bool forceInlineWrites,
				   DeleteOneResult *result);

#endif
