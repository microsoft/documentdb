/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * include/opclass/bson_gin_index_mgmt.h
 *
 * Common declarations of the bson index management methods.
 *
 *-------------------------------------------------------------------------
 */

 #ifndef BSON_GIN_COMPOSITE_PRIVATE_H
 #define BSON_GIN_COMPOSITE_PRIVATE_H

 #include "io/bson_core.h"

typedef struct CompositeSingleBound
{
	bson_value_t bound;
	bool isBoundInclusive;

	/* The processed bound (post truncation if any) */
	bson_value_t processedBoundValue;
	bool isProcessedValueTruncated;
} CompositeSingleBound;

typedef struct CompositeIndexBounds
{
	CompositeSingleBound lowerBound;
	CompositeSingleBound upperBound;

	bool isEqualityBound;

	bool requiresRuntimeRecheck;
} CompositeIndexBounds;

typedef struct CompositeQueryRunData
{
	CompositeIndexBounds indexBounds[INDEX_MAX_KEYS];
	int32_t numIndexPaths;
	bool hasTruncation;
	int32_t truncationTermIndex;
	bool requiresRuntimeRecheck;
} CompositeQueryRunData;


bytea * BuildLowerBoundTermFromIndexBounds(CompositeQueryRunData *runData, int32_t
										   numPaths,
										   IndexTermCreateMetadata *metadata,
										   bool *hasInequalityMatch);

bool UpdateBoundsForTruncation(CompositeIndexBounds *queryBounds, int32_t numPaths,
							   IndexTermCreateMetadata *metadata);


void ParseBoundsFromStrategy(const char **indexPaths, int32_t numPaths,
							 pgbsonelement *queryElement,
							 BsonIndexStrategy queryStrategy,
							 CompositeIndexBounds *queryBounds);
 #endif
