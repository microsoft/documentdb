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

typedef struct IndexRecheckArgs
{
	Pointer queryDatum;

	BsonIndexStrategy queryStrategy;
} IndexRecheckArgs;

typedef struct CompositeIndexBounds
{
	CompositeSingleBound lowerBound;
	CompositeSingleBound upperBound;

	bool isEqualityBound;

	bool requiresRuntimeRecheck;

	/* A list of IndexRecheckArgs that need recheck */
	List *indexRecheckFunctions;
} CompositeIndexBounds;

typedef struct PathScanKeyMap
{
	/* integer list of term indexes - one for each scanKey */
	List *scanIndices;
} PathScanKeyMap;

typedef struct PathScanTermMap
{
	/* Integer list of key indexes - one for each index path */
	List *scanKeyIndexList;
	int32_t numTermsPerPath;
} PathScanTermMap;

typedef struct CompositeQueryMetaInfo
{
	bool hasTruncation;
	int32_t truncationTermIndex;
	bool requiresRuntimeRecheck;
	int32_t numScanKeys;
	bool hasMultipleScanKeysPerPath;
	PathScanKeyMap *scanKeyMap;
} CompositeQueryMetaInfo;

typedef struct CompositeQueryRunData
{
	CompositeIndexBounds indexBounds[INDEX_MAX_KEYS];
	int32_t numIndexPaths;
	CompositeQueryMetaInfo *metaInfo;
} CompositeQueryRunData;

typedef struct CompositeIndexBoundsSet
{
	/* The index path attribute (0 based) */
	int32_t indexAttribute;
	int32_t numBounds;
	CompositeIndexBounds bounds[FLEXIBLE_ARRAY_MEMBER];
} CompositeIndexBoundsSet;

typedef struct VariableIndexBounds
{
	/* List of CompositeIndexBoundsSet */
	List *variableBoundsList;
} VariableIndexBounds;

static inline CompositeIndexBoundsSet *
CreateCompositeIndexBoundsSet(int32_t numTerms, int32_t indexAttribute)
{
	CompositeIndexBoundsSet *set = palloc0(sizeof(CompositeIndexBoundsSet) +
										   (sizeof(CompositeIndexBounds) * numTerms));
	set->numBounds = numTerms;
	set->indexAttribute = indexAttribute;
	return set;
}


bool IsValidRecheckForIndexValue(const bson_value_t *compareValue,
								 bool indexTermHasTruncation,
								 IndexRecheckArgs *recheckArgs);

bytea * BuildLowerBoundTermFromIndexBounds(CompositeQueryRunData *runData,
										   IndexTermCreateMetadata *metadata,
										   bool *hasInequalityMatch);

bool UpdateBoundsForTruncation(CompositeIndexBounds *queryBounds, int32_t numPaths,
							   IndexTermCreateMetadata *metadata);


void ParseOperatorStrategy(const char **indexPaths, int32_t numPaths,
						   pgbsonelement *queryElement,
						   BsonIndexStrategy queryStrategy,
						   VariableIndexBounds *indexBounds);

void UpdateRunDataForVariableBounds(CompositeQueryRunData *runData,
									PathScanTermMap *termMap,
									VariableIndexBounds *variableBounds,
									int32_t permutation);

void MergeSingleVariableBounds(VariableIndexBounds *variableBounds,
							   CompositeQueryRunData *runData);
 #endif
