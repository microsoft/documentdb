/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/opclass/bson_gin_composite_entrypoint.c
 *
 *
 * Gin operator implementations of BSON for a composite index
 * See also: https://www.postgresql.org/docs/current/gin-extensibility.html
 *
 *-------------------------------------------------------------------------
 */


 #include <postgres.h>
 #include <fmgr.h>
 #include <miscadmin.h>
 #include <access/reloptions.h>
 #include <executor/executor.h>
 #include <utils/builtins.h>
 #include <utils/typcache.h>
 #include <utils/lsyscache.h>
 #include <utils/syscache.h>
 #include <utils/timestamp.h>
 #include <utils/array.h>
 #include <parser/parse_coerce.h>
 #include <catalog/pg_type.h>
 #include <funcapi.h>
 #include <lib/stringinfo.h>

 #include "io/bson_core.h"
 #include "query/query_operator.h"
 #include "aggregation/bson_query_common.h"
 #include "opclass/bson_gin_common.h"
 #include "opclass/bson_gin_private.h"
 #include "opclass/bson_gin_index_mgmt.h"
 #include "opclass/bson_gin_index_term.h"
 #include "opclass/bson_gin_index_types_core.h"
 #include "query/bson_compare.h"
 #include "utils/documentdb_errors.h"
 #include "metadata/metadata_cache.h"
 #include "collation/collation.h"
 #include "query/bson_dollar_operators.h"
 #include "opclass/bson_gin_composite_private.h"

/* --------------------------------------------------------- */
/* Data-types */
/* --------------------------------------------------------- */

/* Wrapper struct around regexData to track if it is a negation operator regex or not. */
typedef struct CompositeRegexData
{
	RegexData *regexData;

	bool isNegationOperator;
} CompositeRegexData;


/* --------------------------------------------------------- */
/* Forward declaration */
/* --------------------------------------------------------- */

static void ProcessBoundForQuery(CompositeSingleBound *bound, const
								 IndexTermCreateMetadata *metadata);
static void SetEqualityBound(const bson_value_t *queryValue,
							 CompositeIndexBounds *queryBounds);
static void SetArrayEqualityBound(const bson_value_t *queryValue,
								  CompositeIndexBounds *bounds);
static void SetGreaterThanBounds(const bson_value_t *queryValue,
								 BsonIndexStrategy queryStrategy,
								 CompositeIndexBounds *queryBounds);
static void SetLessThanBounds(const bson_value_t *queryValue,
							  BsonIndexStrategy queryStrategy,
							  CompositeIndexBounds *queryBounds);
static void SetBoundsExistsTrue(CompositeIndexBounds *queryBounds);
static void SetSingleBoundsDollarType(const bson_value_t *queryValue,
									  CompositeIndexBounds *queryBounds);
static void SetSingleBoundsDollarRegex(const bson_value_t *queryValue,
									   CompositeIndexBounds *queryBounds,
									   bool isNegationOp);
static void SetBoundsForNotEqual(const bson_value_t *queryValue,
								 CompositeIndexBounds *queryBounds);


static void SetUpperBound(CompositeSingleBound *currentBoundValue, const
						  CompositeSingleBound *upperBound);
static void SetLowerBound(CompositeSingleBound *currentBoundValue, const
						  CompositeSingleBound *lowerBound);

static void AddMultiBoundaryForDollarIn(int32_t indexAttribute,
										pgbsonelement *queryElement,
										VariableIndexBounds *indexBounds);
static void AddMultiBoundaryForDollarType(int32_t indexAttribute,
										  pgbsonelement *queryElement,
										  VariableIndexBounds *indexBounds);
static void AddMultiBoundaryForDollarNotIn(int32_t indexAttribute,
										   pgbsonelement *queryElement,
										   VariableIndexBounds *indexBounds);
static void AddMultiBoundaryForBitwiseOperator(BsonIndexStrategy strategy,
											   int32_t indexAttribute,
											   pgbsonelement *queryElement,
											   VariableIndexBounds *indexBounds);
static void AddMultiBoundaryForNotGreater(int32_t indexAttribute,
										  pgbsonelement *queryElement,
										  VariableIndexBounds *indexBounds, bool
										  isEquals);
static void AddMultiBoundaryForNotLess(int32_t indexAttribute,
									   pgbsonelement *queryElement,
									   VariableIndexBounds *indexBounds, bool isEquals);
static void AddMultiBoundaryForDollarRange(int32_t indexAttribute,
										   pgbsonelement *queryElement,
										   VariableIndexBounds *indexBounds);
static CompositeIndexBoundsSet * AddMultiBoundaryForDollarRegex(int32_t indexAttribute,
																pgbsonelement *
																queryElement,
																VariableIndexBounds *
																indexBounds,
																bool isNegationOp);

inline static CompositeSingleBound
GetTypeLowerBound(bson_type_t type)
{
	CompositeSingleBound bound = { 0 };
	bound.bound = GetLowerBound(type);
	bound.isBoundInclusive = true; /* Default to inclusive for lower bounds */
	return bound;
}


inline static CompositeSingleBound
GetTypeUpperBound(bson_type_t type)
{
	CompositeSingleBound bound = { 0 };
	bound.bound = GetUpperBound(type, &bound.isBoundInclusive);
	return bound;
}


bytea *
BuildLowerBoundTermFromIndexBounds(CompositeQueryRunData *runData,
								   IndexTermCreateMetadata *metadata,
								   bool *hasInequalityMatch)
{
	bytea *lowerBoundDatums[INDEX_MAX_KEYS] = { 0 };

	bool hasTruncation = false;
	for (int i = 0; i < runData->numIndexPaths; i++)
	{
		runData->metaInfo->requiresRuntimeRecheck =
			runData->metaInfo->requiresRuntimeRecheck ||
			runData->indexBounds[i].
			requiresRuntimeRecheck;
		hasTruncation = hasTruncation ||
						runData->indexBounds[i].lowerBound.isProcessedValueTruncated;

		/* If both lower and upper bound match it's equality */
		if (runData->indexBounds[i].lowerBound.bound.value_type != BSON_TYPE_EOD &&
			runData->indexBounds[i].upperBound.bound.value_type != BSON_TYPE_EOD &&
			runData->indexBounds[i].lowerBound.isBoundInclusive &&
			runData->indexBounds[i].upperBound.isBoundInclusive &&
			BsonValueEquals(&runData->indexBounds[i].lowerBound.bound,
							&runData->indexBounds[i].upperBound.bound))
		{
			runData->indexBounds[i].isEqualityBound = true;
			lowerBoundDatums[i] = runData->indexBounds[i].lowerBound.serializedTerm;
			continue;
		}

		*hasInequalityMatch = true;
		if (runData->indexBounds[i].lowerBound.bound.value_type != BSON_TYPE_EOD)
		{
			/* There exists a lower bound for this key */
			lowerBoundDatums[i] = runData->indexBounds[i].lowerBound.serializedTerm;
		}
		else
		{
			/* All possible values are valid for this key */
			pgbsonelement termElement = { 0 };
			termElement.path = "$";
			termElement.pathLength = 1;
			termElement.bsonValue.value_type = BSON_TYPE_MINKEY;
			BsonIndexTermSerialized serialized = SerializeBsonIndexTerm(&termElement,
																		metadata);
			lowerBoundDatums[i] = serialized.indexTermVal;
		}
	}

	BsonIndexTermSerialized ser = SerializeCompositeBsonIndexTerm(lowerBoundDatums,
																  runData->numIndexPaths);
	return ser.indexTermVal;
}


void
UpdateRunDataForVariableBounds(CompositeQueryRunData *runData,
							   PathScanTermMap *termMap,
							   VariableIndexBounds *variableBounds,
							   int32_t permutation)
{
	ListCell *cell;
	int32_t originalPermutation = permutation;

	/* Take one term per path */
	for (int i = 0; i < runData->numIndexPaths; i++)
	{
		/* This is the index'th term for the current path */
		if (termMap[i].numTermsPerPath == 0)
		{
			continue;
		}

		int index = permutation % termMap[i].numTermsPerPath;
		permutation /= termMap[i].numTermsPerPath;

		/* Now fetch the set based on the index */
		int32_t scanKeyIndex = -1;
		CompositeIndexBoundsSet *set = NULL;
		foreach(cell, termMap[i].scanKeyIndexList)
		{
			int scanKeyCandidate = lfirst_int(cell);
			set = list_nth(variableBounds->variableBoundsList, scanKeyCandidate);
			if (set->numBounds > index)
			{
				scanKeyIndex = scanKeyCandidate;
				break;
			}

			index -= set->numBounds;
		}

		if (scanKeyIndex == -1 || set == NULL)
		{
			ereport(ERROR, (errmsg("Could not find scan key for term")));
		}

		/* Track the current term in the scan key */
		runData->metaInfo->scanKeyMap[scanKeyIndex].scanIndices =
			lappend_int(runData->metaInfo->scanKeyMap[scanKeyIndex].scanIndices,
						originalPermutation);

		/* Update the runData with the selected bounds for this index attribute */
		CompositeIndexBounds *bound = &set->bounds[index];
		if (bound->lowerBound.bound.value_type != BSON_TYPE_EOD)
		{
			SetLowerBound(&runData->indexBounds[set->indexAttribute].lowerBound,
						  &bound->lowerBound);
		}

		if (bound->upperBound.bound.value_type != BSON_TYPE_EOD)
		{
			SetUpperBound(&runData->indexBounds[set->indexAttribute].upperBound,
						  &bound->upperBound);
		}

		if (bound->indexRecheckFunctions != NIL)
		{
			runData->indexBounds[set->indexAttribute].indexRecheckFunctions =
				list_concat(
					runData->indexBounds[set->indexAttribute].indexRecheckFunctions,
					bound->indexRecheckFunctions);
		}

		runData->metaInfo->requiresRuntimeRecheck =
			runData->metaInfo->requiresRuntimeRecheck ||
			bound->requiresRuntimeRecheck;
	}
}


void
MergeSingleVariableBounds(VariableIndexBounds *variableBounds,
						  CompositeQueryRunData *runData)
{
	ListCell *cell;
	foreach(cell, variableBounds->variableBoundsList)
	{
		CompositeIndexBoundsSet *set =
			(CompositeIndexBoundsSet *) lfirst(cell);
		if (set->numBounds != 1)
		{
			continue;
		}

		CompositeIndexBounds *bound = &set->bounds[0];
		if (bound->lowerBound.bound.value_type != BSON_TYPE_EOD)
		{
			SetLowerBound(&runData->indexBounds[set->indexAttribute].lowerBound,
						  &bound->lowerBound);
		}

		if (bound->upperBound.bound.value_type != BSON_TYPE_EOD)
		{
			SetUpperBound(&runData->indexBounds[set->indexAttribute].upperBound,
						  &bound->upperBound);
		}

		runData->indexBounds[set->indexAttribute].requiresRuntimeRecheck =
			runData->indexBounds[set->indexAttribute].requiresRuntimeRecheck ||
			set->bounds->requiresRuntimeRecheck;

		if (set->bounds->indexRecheckFunctions != NIL)
		{
			runData->indexBounds[set->indexAttribute].indexRecheckFunctions =
				list_concat(
					runData->indexBounds[set->indexAttribute].indexRecheckFunctions,
					set->bounds->indexRecheckFunctions);
		}


		/* Postgres requires that we don't use cell or anything in foreach after
		 * calling delete. explicity add a continue to match that contract.
		 */
		variableBounds->variableBoundsList =
			foreach_delete_current(variableBounds->variableBoundsList, cell);
		continue;
	}
}


bool
UpdateBoundsForTruncation(CompositeIndexBounds *queryBounds, int32_t numPaths,
						  IndexTermCreateMetadata *metadata)
{
	bool hasTruncation = false;
	for (int i = 0; i < numPaths; i++)
	{
		if (queryBounds[i].lowerBound.bound.value_type != BSON_TYPE_EOD)
		{
			ProcessBoundForQuery(&queryBounds[i].lowerBound,
								 metadata);
			hasTruncation = hasTruncation ||
							queryBounds[i].lowerBound.
							isProcessedValueTruncated;
		}

		if (queryBounds[i].upperBound.bound.value_type != BSON_TYPE_EOD)
		{
			ProcessBoundForQuery(&queryBounds[i].upperBound,
								 metadata);
			hasTruncation = hasTruncation ||
							queryBounds[i].upperBound.
							isProcessedValueTruncated;
		}
	}

	return hasTruncation;
}


inline static CompositeIndexBoundsSet *
CreateAndRegisterSingleIndexBoundsSet(VariableIndexBounds *indexBounds, int
									  indexAttribute)
{
	const int numTerms = 1;
	CompositeIndexBoundsSet *set = palloc0(offsetof(CompositeIndexBoundsSet,
													bounds) +
										   numTerms * sizeof(CompositeIndexBounds));
	set->indexAttribute = indexAttribute;
	set->numBounds = numTerms;
	indexBounds->variableBoundsList = lappend(indexBounds->variableBoundsList,
											  set);
	return set;
}


static bool
GetFirstElementFromQueryArray(const bson_value_t *arrayValue, bson_value_t *firstElement)
{
	/* Empty array defaults to NULL */
	firstElement->value_type = BSON_TYPE_NULL;
	bson_iter_t arrayIter;
	BsonValueInitIterator(arrayValue, &arrayIter);

	if (bson_iter_next(&arrayIter))
	{
		*firstElement = *bson_iter_value(&arrayIter);
		return true;
	}

	return false;
}


void
ParseOperatorStrategy(const char **indexPaths, int32_t numPaths,
					  pgbsonelement *queryElement,
					  BsonIndexStrategy queryStrategy,
					  VariableIndexBounds *indexBounds)
{
	/* First figure out which query path matches */
	int32_t i = 0;
	bool found = false;
	for (; i < numPaths; i++)
	{
		if (strcmp(indexPaths[i], queryElement->path) == 0)
		{
			found = true;
			break;
		}
	}

	if (!found)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_INTERNALERROR), errmsg(
							"Query path '%s' does not match any index paths",
							queryElement->path)));
	}

	bool isNegationOp = false;

	/* Now that we have the index path, add or update the bounds */
	switch (queryStrategy)
	{
		/* Single bound operators */
		case BSON_INDEX_STRATEGY_DOLLAR_EQUAL:
		{
			if (queryElement->bsonValue.value_type == BSON_TYPE_ARRAY)
			{
				int numterms = 2;
				CompositeIndexBoundsSet *set = CreateCompositeIndexBoundsSet(numterms, i);
				SetArrayEqualityBound(&queryElement->bsonValue, &set->bounds[0]);
				indexBounds->variableBoundsList = lappend(indexBounds->variableBoundsList,
														  set);
			}
			else
			{
				CompositeIndexBoundsSet *set = CreateAndRegisterSingleIndexBoundsSet(
					indexBounds, i);
				SetEqualityBound(&queryElement->bsonValue, &set->bounds[0]);
			}

			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_GREATER_EQUAL:
		case BSON_INDEX_STRATEGY_DOLLAR_GREATER:
		{
			CompositeIndexBoundsSet *set = CreateAndRegisterSingleIndexBoundsSet(
				indexBounds, i);
			SetGreaterThanBounds(&queryElement->bsonValue, queryStrategy,
								 &set->bounds[0]);
			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_LESS:
		case BSON_INDEX_STRATEGY_DOLLAR_LESS_EQUAL:
		{
			CompositeIndexBoundsSet *set = CreateAndRegisterSingleIndexBoundsSet(
				indexBounds, i);
			SetLessThanBounds(&queryElement->bsonValue, queryStrategy, &set->bounds[0]);
			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_EXISTS:
		{
			CompositeIndexBoundsSet *set = CreateAndRegisterSingleIndexBoundsSet(
				indexBounds, i);
			int existsValue = BsonValueAsInt32(&queryElement->bsonValue);
			if (existsValue == 1)
			{
				/* { exists: true } */
				SetBoundsExistsTrue(&set->bounds[0]);
			}
			else
			{
				/* Exists false is counted as > MinKey && <= NULL */
				CompositeSingleBound minKeyBounds = { 0 };
				minKeyBounds.bound.value_type = BSON_TYPE_MINKEY;
				minKeyBounds.isBoundInclusive = false;
				SetLowerBound(&set->bounds[0].lowerBound, &minKeyBounds);

				CompositeSingleBound nullBounds = { 0 };
				nullBounds.bound.value_type = BSON_TYPE_NULL;
				nullBounds.isBoundInclusive = true;
				SetUpperBound(&set->bounds[0].upperBound, &nullBounds);

				bool *existsValue = (bool *) palloc(sizeof(bool));
				*existsValue = false;
				IndexRecheckArgs *args = palloc0(sizeof(IndexRecheckArgs));
				args->queryDatum = (Pointer) existsValue;
				args->queryStrategy = BSON_INDEX_STRATEGY_DOLLAR_EXISTS;
				set->bounds[0].indexRecheckFunctions =
					lappend(set->bounds[0].indexRecheckFunctions, args);
			}

			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_ELEMMATCH:
		{
			/* TODO(Composite): Push this to actually filter on the current index */
			CompositeIndexBoundsSet *set = CreateAndRegisterSingleIndexBoundsSet(
				indexBounds, i);
			SetBoundsExistsTrue(&set->bounds[0]);
			set->bounds[0].requiresRuntimeRecheck = true;
			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_SIZE:
		{
			/*
			 * $size can't be pushed to the index, so we convert
			 * to an exists query with runtime recheck.
			 */
			CompositeIndexBoundsSet *set = CreateAndRegisterSingleIndexBoundsSet(
				indexBounds, i);
			int sizeValue = BsonValueAsInt32(&queryElement->bsonValue);
			if (sizeValue == 0)
			{
				/* This is an empty scan with a runtime recheck */
				bson_value_t undefinedValue = { 0 };
				undefinedValue.value_type = BSON_TYPE_NULL;
				SetEqualityBound(&undefinedValue, &set->bounds[0]);
			}
			else
			{
				SetBoundsExistsTrue(&set->bounds[0]);
			}

			/* Needs a runtime recheck since we don't know about arrays */
			set->bounds[0].requiresRuntimeRecheck = true;
			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_MOD:
		{
			CompositeIndexBoundsSet *set = CreateAndRegisterSingleIndexBoundsSet(
				indexBounds, i);
			CompositeSingleBound bounds = GetTypeLowerBound(BSON_TYPE_DOUBLE);
			SetLowerBound(&set->bounds[0].lowerBound, &bounds);

			bounds = GetTypeUpperBound(BSON_TYPE_DOUBLE);
			SetUpperBound(&set->bounds[0].upperBound, &bounds);

			bson_value_t *modFilter = palloc(sizeof(bson_value_t));
			*modFilter = queryElement->bsonValue;
			IndexRecheckArgs *args = palloc0(sizeof(IndexRecheckArgs));
			args->queryDatum = (Pointer) modFilter;
			args->queryStrategy = BSON_INDEX_STRATEGY_DOLLAR_MOD;
			set->bounds[0].indexRecheckFunctions =
				lappend(set->bounds[0].indexRecheckFunctions, args);
			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_NOT_EQUAL:
		{
			CompositeIndexBoundsSet *set = CreateAndRegisterSingleIndexBoundsSet(
				indexBounds, i);
			SetBoundsForNotEqual(&queryElement->bsonValue, &set->bounds[0]);
			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_REGEX:
		{
			if (queryElement->bsonValue.value_type == BSON_TYPE_REGEX)
			{
				AddMultiBoundaryForDollarRegex(i, queryElement, indexBounds,
											   isNegationOp);
			}
			else
			{
				/* Regex with a string - single strategy */
				CompositeIndexBoundsSet *set = CreateAndRegisterSingleIndexBoundsSet(
					indexBounds, i);
				SetSingleBoundsDollarRegex(&queryElement->bsonValue,
										   set->bounds,
										   isNegationOp);
			}

			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_RANGE:
		{
			AddMultiBoundaryForDollarRange(i, queryElement, indexBounds);
			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_TYPE:
		{
			if (queryElement->bsonValue.value_type == BSON_TYPE_ARRAY)
			{
				AddMultiBoundaryForDollarType(i, queryElement, indexBounds);
			}
			else
			{
				CompositeIndexBoundsSet *set = CreateAndRegisterSingleIndexBoundsSet(
					indexBounds, i);
				SetSingleBoundsDollarType(&queryElement->bsonValue, set->bounds);
			}

			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_IN:
		{
			AddMultiBoundaryForDollarIn(i, queryElement, indexBounds);
			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_NOT_IN:
		{
			AddMultiBoundaryForDollarNotIn(i, queryElement, indexBounds);
			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_BITS_ALL_CLEAR:
		case BSON_INDEX_STRATEGY_DOLLAR_BITS_ANY_CLEAR:
		case BSON_INDEX_STRATEGY_DOLLAR_BITS_ALL_SET:
		case BSON_INDEX_STRATEGY_DOLLAR_BITS_ANY_SET:
		{
			AddMultiBoundaryForBitwiseOperator(queryStrategy, i, queryElement,
											   indexBounds);
			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_NOT_GT:
		{
			bool isEquals = false;
			AddMultiBoundaryForNotGreater(i, queryElement, indexBounds, isEquals);
			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_NOT_GTE:
		{
			bool isEquals = true;
			AddMultiBoundaryForNotGreater(i, queryElement, indexBounds, isEquals);
			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_NOT_LT:
		{
			bool isEquals = false;
			AddMultiBoundaryForNotLess(i, queryElement, indexBounds, isEquals);
			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_NOT_LTE:
		{
			bool isEquals = true;
			AddMultiBoundaryForNotLess(i, queryElement, indexBounds, isEquals);
			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_ORDERBY:
		{
			/* It's a full scan */
			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_ALL:
		case BSON_INDEX_STRATEGY_DOLLAR_TEXT:
		case BSON_INDEX_STRATEGY_DOLLAR_GEOINTERSECTS:
		case BSON_INDEX_STRATEGY_DOLLAR_GEOWITHIN:
		case BSON_INDEX_STRATEGY_GEONEAR:
		case BSON_INDEX_STRATEGY_GEONEAR_RANGE:
		case BSON_INDEX_STRATEGY_COMPOSITE_QUERY:
		case BSON_INDEX_STRATEGY_UNIQUE_EQUAL:
		default:
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_INTERNALERROR), errmsg(
								"Unsupported strategy for composite index: %d",
								queryStrategy)));
			break;
		}
	}
}


bool
IsValidRecheckForIndexValue(const BsonIndexTerm *compareTerm,
							IndexRecheckArgs *recheckArgs)
{
	switch (recheckArgs->queryStrategy)
	{
		case BSON_INDEX_STRATEGY_DOLLAR_REGEX:
		{
			if (compareTerm->isIndexTermTruncated)
			{
				/* don't bother, let the runtime check on this */
				return true;
			}

			CompositeRegexData *compositeRegexData =
				(CompositeRegexData *) recheckArgs->queryDatum;
			bool result = CompareRegexTextMatch(&compareTerm->element.bsonValue,
												compositeRegexData->regexData);
			return compositeRegexData->isNegationOperator ? !result : result;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_EXISTS:
		{
			bool *exists = (bool *) recheckArgs->queryDatum;
			if (!*exists)
			{
				/* exists: false, matches all values except that are defined */
				return compareTerm->isValueUndefined;
			}
			else
			{
				/* exists: true, check that it's not undefined */
				return !compareTerm->isValueUndefined;
			}
		}

		case BSON_INDEX_STRATEGY_DOLLAR_MOD:
		{
			bson_value_t *modQuery = (bson_value_t *) recheckArgs->queryDatum;
			return CompareModOperator(&compareTerm->element.bsonValue, modQuery);
		}

		case BSON_INDEX_STRATEGY_DOLLAR_NOT_EQUAL:
		{
			bson_value_t *notEqualQuery = (bson_value_t *) recheckArgs->queryDatum;

			if (compareTerm->isIndexTermTruncated)
			{
				/* don't bother, let the runtime check on this */
				return true;
			}

			/* In the case of NULL and if we see undefined, we don't know if it's literal
			 * undefined or an empty array - thunk to runtime
			 * TODO(Composite): Can we differentiate between empty array and literal null?
			 */
			if (notEqualQuery->value_type == BSON_TYPE_NULL)
			{
				/* if the value is *maybe* undefined then there's another value that's defined
				 * let the other value determine matched-ness
				 */
				return !compareTerm->isValueMaybeUndefined;
			}

			return !BsonValueEquals(&compareTerm->element.bsonValue, notEqualQuery);
		}

		case BSON_INDEX_STRATEGY_DOLLAR_BITS_ALL_CLEAR:
		{
			bson_value_t *bitsQuery = (bson_value_t *) recheckArgs->queryDatum;
			if (compareTerm->isIndexTermTruncated)
			{
				/* don't bother, let the runtime check on this */
				return true;
			}

			return CompareBitwiseOperator(&compareTerm->element.bsonValue,
										  bitsQuery, CompareArrayForBitsAllClear);
		}

		case BSON_INDEX_STRATEGY_DOLLAR_BITS_ANY_CLEAR:
		{
			bson_value_t *bitsQuery = (bson_value_t *) recheckArgs->queryDatum;
			if (compareTerm->isIndexTermTruncated)
			{
				/* don't bother, let the runtime check on this */
				return true;
			}

			return CompareBitwiseOperator(&compareTerm->element.bsonValue,
										  bitsQuery, CompareArrayForBitsAnyClear);
		}

		case BSON_INDEX_STRATEGY_DOLLAR_BITS_ALL_SET:
		{
			bson_value_t *bitsQuery = (bson_value_t *) recheckArgs->queryDatum;
			if (compareTerm->isIndexTermTruncated)
			{
				/* don't bother, let the runtime check on this */
				return true;
			}

			return CompareBitwiseOperator(&compareTerm->element.bsonValue,
										  bitsQuery, CompareArrayForBitsAllSet);
		}

		case BSON_INDEX_STRATEGY_DOLLAR_BITS_ANY_SET:
		{
			bson_value_t *bitsQuery = (bson_value_t *) recheckArgs->queryDatum;
			if (compareTerm->isIndexTermTruncated)
			{
				/* don't bother, let the runtime check on this */
				return true;
			}

			return CompareBitwiseOperator(&compareTerm->element.bsonValue,
										  bitsQuery, CompareArrayForBitsAnySet);
		}

		case BSON_INDEX_STRATEGY_DOLLAR_EQUAL:
		case BSON_INDEX_STRATEGY_DOLLAR_GREATER_EQUAL:
		case BSON_INDEX_STRATEGY_DOLLAR_GREATER:
		case BSON_INDEX_STRATEGY_DOLLAR_LESS:
		case BSON_INDEX_STRATEGY_DOLLAR_LESS_EQUAL:
		case BSON_INDEX_STRATEGY_DOLLAR_RANGE:
		case BSON_INDEX_STRATEGY_DOLLAR_TYPE:
		case BSON_INDEX_STRATEGY_DOLLAR_ALL:
		case BSON_INDEX_STRATEGY_DOLLAR_IN:
		case BSON_INDEX_STRATEGY_DOLLAR_NOT_IN:
		case BSON_INDEX_STRATEGY_DOLLAR_NOT_GT:
		case BSON_INDEX_STRATEGY_DOLLAR_NOT_GTE:
		case BSON_INDEX_STRATEGY_DOLLAR_NOT_LT:
		case BSON_INDEX_STRATEGY_DOLLAR_NOT_LTE:
		case BSON_INDEX_STRATEGY_DOLLAR_ORDERBY:
		{
			/* No recheck */
			ereport(ERROR, (errmsg(
								"Unexpected - should not have Index Recheck function for %d",
								recheckArgs->queryStrategy)));
			return false;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_TEXT:
		case BSON_INDEX_STRATEGY_DOLLAR_GEOINTERSECTS:
		case BSON_INDEX_STRATEGY_DOLLAR_GEOWITHIN:
		case BSON_INDEX_STRATEGY_GEONEAR:
		case BSON_INDEX_STRATEGY_GEONEAR_RANGE:
		case BSON_INDEX_STRATEGY_COMPOSITE_QUERY:
		case BSON_INDEX_STRATEGY_UNIQUE_EQUAL:
		default:
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_INTERNALERROR), errmsg(
								"Unsupported strategy for composite index: %d",
								recheckArgs->queryStrategy)));
			return false;
		}
	}
}


static void
ProcessBoundForQuery(CompositeSingleBound *bound, const IndexTermCreateMetadata *metadata)
{
	pgbson_writer writer;
	PgbsonWriterInit(&writer);

	pgbsonelement termElement = { 0 };
	termElement.path = "$";
	termElement.pathLength = 1;
	termElement.bsonValue = bound->bound;

	BsonIndexTermSerialized serialized = SerializeBsonIndexTerm(&termElement, metadata);
	bound->serializedTerm = serialized.indexTermVal;
	if (serialized.isIndexTermTruncated)
	{
		/* preserve and store the value */
		BsonIndexTerm term;
		InitializeBsonIndexTerm(serialized.indexTermVal, &term);
		bound->processedBoundValue = term.element.bsonValue;
		bound->isProcessedValueTruncated = term.isIndexTermTruncated;
	}
	else
	{
		/* Just keep the original */
		bound->processedBoundValue = bound->bound;
		bound->isProcessedValueTruncated = false;
	}
}


static void
SetLowerBound(CompositeSingleBound *currentBoundValue, const
			  CompositeSingleBound *lowerBound)
{
	if (currentBoundValue->bound.value_type == BSON_TYPE_EOD)
	{
		*currentBoundValue = *lowerBound;
	}
	else
	{
		bool isComparisonValid = false;
		int32_t comparison = CompareBsonValueAndType(&currentBoundValue->bound,
													 &lowerBound->bound,
													 &isComparisonValid);

		if (comparison == 0)
		{
			/* scenario of $ > val with $ > value: ensure the inclusive bits are correct */
			currentBoundValue->isBoundInclusive =
				currentBoundValue->isBoundInclusive && lowerBound->isBoundInclusive;
		}
		else if (comparison < 0)
		{
			/* Current bound is less than incoming bound
			 * We have current: $ > a, new is $ > b where a < b
			 * Pick the new bound.
			 */
			*currentBoundValue = *lowerBound;
		}
	}
}


static void
SetUpperBound(CompositeSingleBound *currentBoundValue, const
			  CompositeSingleBound *upperBound)
{
	if (currentBoundValue->bound.value_type == BSON_TYPE_EOD)
	{
		*currentBoundValue = *upperBound;
	}
	else
	{
		bool isComparisonValid = false;
		int32_t comparison = CompareBsonValueAndType(&currentBoundValue->bound,
													 &upperBound->bound,
													 &isComparisonValid);

		if (comparison == 0)
		{
			/* scenario of $ > val with $ > value: ensure the inclusive bits are correct */
			currentBoundValue->isBoundInclusive =
				currentBoundValue->isBoundInclusive && upperBound->isBoundInclusive;
		}
		else if (comparison > 0)
		{
			/* Current bound is greater than incoming bound
			 * We have current: $ > a, new is $ > b where a > b
			 * Pick the new bound.
			 */
			*currentBoundValue = *upperBound;
		}
	}
}


static void
SetBoundsExistsTrue(CompositeIndexBounds *queryBounds)
{
	/* This is similar to $exists: true */
	CompositeSingleBound bounds = { 0 };
	bounds.bound.value_type = BSON_TYPE_MINKEY;
	bounds.isBoundInclusive = true;
	SetLowerBound(&queryBounds->lowerBound, &bounds);

	bounds.bound.value_type = BSON_TYPE_MAXKEY;
	bounds.isBoundInclusive = true;
	SetUpperBound(&queryBounds->upperBound, &bounds);

	bool *existsValue = (bool *) palloc(sizeof(bool));
	*existsValue = true;
	IndexRecheckArgs *args = palloc0(sizeof(IndexRecheckArgs));
	args->queryDatum = (Pointer) existsValue;
	args->queryStrategy = BSON_INDEX_STRATEGY_DOLLAR_EXISTS;
	queryBounds->indexRecheckFunctions =
		lappend(queryBounds->indexRecheckFunctions, args);
}


static void
SetArrayEqualityBound(const bson_value_t *queryValue,
					  CompositeIndexBounds *bounds)
{
	/* This is a special case for $eq on an array, since we don't index top level arrays
	 * We need to push this as 2 bounds:
	 * - One that is equality on the first entry of the top level array
	 * - One that is equality on the array as a whole (to match nested arrays).
	 */

	/* Equality on the array itself */
	SetEqualityBound(queryValue, &bounds[0]);

	/* Equality on the first element of the array */
	bson_value_t firstElement = { 0 };
	GetFirstElementFromQueryArray(queryValue, &firstElement);

	SetEqualityBound(&firstElement, &bounds[1]);

	/* Add a runtime recheck */
	bounds[1].requiresRuntimeRecheck = true;
}


static void
SetEqualityBound(const bson_value_t *queryValue, CompositeIndexBounds *queryBounds)
{
	CompositeSingleBound equalsBounds = { 0 };
	equalsBounds.bound = *queryValue;
	equalsBounds.isBoundInclusive = true;

	if (queryValue->value_type == BSON_TYPE_NULL)
	{
		/* For null we set the lower bound to be > MinKey so we capture undefined values as well. */
		CompositeSingleBound minNullBound = { 0 };
		minNullBound.bound.value_type = BSON_TYPE_MINKEY;
		minNullBound.isBoundInclusive = false;
		SetLowerBound(&queryBounds->lowerBound, &minNullBound);
	}
	else
	{
		SetLowerBound(&queryBounds->lowerBound, &equalsBounds);
	}

	SetUpperBound(&queryBounds->upperBound, &equalsBounds);

	if (queryValue->value_type == BSON_TYPE_NULL)
	{
		/* TODO(Composite): See if this is needed
		 * Special case, requires runtime recheck always
		 */
		queryBounds->requiresRuntimeRecheck = true;
	}
}


static void
SetGreaterThanBounds(const bson_value_t *queryValue,
					 BsonIndexStrategy queryStrategy,
					 CompositeIndexBounds *queryBounds)
{
	bool isMinBoundInclusive = queryStrategy == BSON_INDEX_STRATEGY_DOLLAR_GREATER_EQUAL;
	if (queryValue->value_type == BSON_TYPE_MINKEY)
	{
		/* Special case, minKey is always inclusive */
		SetBoundsExistsTrue(queryBounds);

		/*
		 * If it's the case of $gt: MinKey, we need to include MinKey and add a recheck
		 * since an array with MinKey as the first element is a valid match.
		 * TODO(Composite): This can be improved if we had the recheck flag capable of being set at
		 * a term level or split it into two bounds - equality on MinKey with recheck and
		 * > Minkey without recheck.
		 */
		queryBounds->requiresRuntimeRecheck = !isMinBoundInclusive;
		return;
	}

	bool skipTypeBracketing = false;
	bson_value_t compareValue = *queryValue;
	if (compareValue.value_type == BSON_TYPE_ARRAY)
	{
		/* Arrays require runtime recheck on the greater than value */
		queryBounds->requiresRuntimeRecheck = true;

		/* Arrays need to skip typebracketing - it'll be all values until maxKey */
		skipTypeBracketing = true;

		/* Now the value to use for the $gt bounds is the MIN(Array, firstElement) */
		bson_value_t firstElement = { 0 };
		if (!GetFirstElementFromQueryArray(&compareValue, &firstElement))
		{
			/* If the array is empty, we need to compare against all elements >= []
			 * In this case, that includes arrays whose first value can be MinKey.
			 * The lower bound becomes minkey
			 */
			firstElement.value_type = BSON_TYPE_MINKEY;
		}

		bool isComparisonValidIgnore = false;
		int compareResult = CompareBsonValueAndType(&compareValue, &firstElement,
													&isComparisonValidIgnore);
		if (compareResult > 0)
		{
			/* Use the first element as the compare value
			 * In this case, the lower bound is always inclusive
			 */
			compareValue = firstElement;
			isMinBoundInclusive = true;
		}
	}

	/* Special case for $gte null */
	CompositeSingleBound bounds = { 0 };
	if (compareValue.value_type == BSON_TYPE_NULL &&
		isMinBoundInclusive && !skipTypeBracketing)
	{
		bounds.bound.value_type = BSON_TYPE_MINKEY;
		bounds.isBoundInclusive = false;
	}
	else
	{
		bounds.bound = compareValue;
		bounds.isBoundInclusive = isMinBoundInclusive;
	}

	SetLowerBound(&queryBounds->lowerBound, &bounds);

	/* Apply type bracketing unless requested to skip */
	if (compareValue.value_type == BSON_TYPE_MINKEY || skipTypeBracketing)
	{
		bounds = GetTypeUpperBound(BSON_TYPE_MAXKEY);
		SetUpperBound(&queryBounds->upperBound, &bounds);
	}
	else if (IsBsonValueNaN(queryValue))
	{
		/* Range should just be [ > NaN, < NaN ] */
		SetUpperBound(&queryBounds->upperBound, &bounds);
	}
	else
	{
		bounds = GetTypeUpperBound(queryValue->value_type);
		SetUpperBound(&queryBounds->upperBound, &bounds);
	}

	if (queryValue->value_type == BSON_TYPE_NULL)
	{
		/* Special case, requires runtime recheck always */
		queryBounds->requiresRuntimeRecheck = true;
	}
}


static void
SetLessThanBounds(const bson_value_t *queryValue,
				  BsonIndexStrategy queryStrategy,
				  CompositeIndexBounds *queryBounds)
{
	bson_value_t compareValue = *queryValue;
	bool skipTypeBracketing = false;
	bool isUpperBoundInclusive =
		queryStrategy == BSON_INDEX_STRATEGY_DOLLAR_LESS_EQUAL;
	if (compareValue.value_type == BSON_TYPE_ARRAY)
	{
		/* Arrays require runtime recheck on the greater than value */
		queryBounds->requiresRuntimeRecheck = true;

		/* Arrays need to skip typebracketing - it'll be all values until maxKey */
		skipTypeBracketing = true;

		/* Now the value to use for the $gt bounds is the MAX(Array, firstElement) */
		bson_value_t firstElement = { 0 };
		GetFirstElementFromQueryArray(&compareValue, &firstElement);

		bool isComparisonValidIgnore = false;
		int compareResult = CompareBsonValueAndType(&compareValue, &firstElement,
													&isComparisonValidIgnore);
		if (compareResult < 0)
		{
			/* Use the first element as the compare value
			 * In this case, the upper bound is always inclusive.
			 */
			compareValue = firstElement;
			isUpperBoundInclusive = true;
		}
	}

	/* In the case where we have $lt: MaxKey, the Value MaxKey is valid since
	 * an array with the first element MaxKey is a valid match.
	 * Consider this an exists true with runtime recheck - since this ONLY excludes
	 * literal MaxKey.
	 * TODO(Composite): This could be improved by saying that the recheck is per compare partial
	 * or breaking this down into 2 bounds - one for MaxKey with recheck and the rest is exact.
	 */
	if (compareValue.value_type == BSON_TYPE_MAXKEY && !isUpperBoundInclusive)
	{
		/* Special case, maxKey is always inclusive */
		SetBoundsExistsTrue(queryBounds);
		queryBounds->requiresRuntimeRecheck = true;
		return;
	}

	CompositeSingleBound bounds = { 0 };
	bounds.bound = compareValue;
	bounds.isBoundInclusive = isUpperBoundInclusive;
	SetUpperBound(&queryBounds->upperBound, &bounds);

	/* Apply type bracketing */
	if (compareValue.value_type == BSON_TYPE_MAXKEY || skipTypeBracketing)
	{
		bounds = GetTypeLowerBound(BSON_TYPE_MINKEY);
		SetLowerBound(&queryBounds->lowerBound, &bounds);
	}
	else if (IsBsonValueNaN(&compareValue))
	{
		/* Range should just be [NaN, NaN]. */
		SetLowerBound(&queryBounds->lowerBound, &bounds);
	}
	else if (compareValue.value_type == BSON_TYPE_NULL &&
			 queryStrategy == BSON_INDEX_STRATEGY_DOLLAR_LESS_EQUAL)
	{
		/* Special case, null is always inclusive */
		bounds.bound.value_type = BSON_TYPE_MINKEY;
		bounds.isBoundInclusive = false;
		SetLowerBound(&queryBounds->lowerBound, &bounds);
	}
	else
	{
		bounds = GetTypeLowerBound(compareValue.value_type);
		SetLowerBound(&queryBounds->lowerBound, &bounds);
	}

	if (compareValue.value_type == BSON_TYPE_NULL)
	{
		/* Special case, requires runtime recheck always */
		queryBounds->requiresRuntimeRecheck = true;
	}
}


static void
SetBoundsForNotEqual(const bson_value_t *queryValue,
					 CompositeIndexBounds *queryBounds)
{
	CompositeSingleBound bounds = GetTypeLowerBound(BSON_TYPE_MINKEY);
	SetLowerBound(&queryBounds->lowerBound, &bounds);

	bounds = GetTypeUpperBound(BSON_TYPE_MAXKEY);
	SetUpperBound(&queryBounds->upperBound, &bounds);

	bson_value_t *equalsValue = palloc0(sizeof(bson_value_t));
	*equalsValue = *queryValue;
	IndexRecheckArgs *args = palloc0(sizeof(IndexRecheckArgs));
	args->queryDatum = (Pointer) equalsValue;
	args->queryStrategy = BSON_INDEX_STRATEGY_DOLLAR_NOT_EQUAL;
	queryBounds->indexRecheckFunctions =
		lappend(queryBounds->indexRecheckFunctions, args);

	/*
	 * For $ne (and other negation scenarios), we need to revalidate
	 * in the runtime since you could have a: [ 1, 2, 3 ]
	 * a != 2 will match for the 3rd term.
	 */
	queryBounds->requiresRuntimeRecheck = true;
}


static void
SetSingleBoundsDollarRegex(const bson_value_t *queryValue,
						   CompositeIndexBounds *queryBounds,
						   bool isNegationOp)
{
	CompositeSingleBound bounds = GetTypeLowerBound(isNegationOp ?
													BSON_TYPE_MINKEY :
													BSON_TYPE_UTF8);
	SetLowerBound(&queryBounds->lowerBound, &bounds);

	bounds = GetTypeUpperBound(isNegationOp ? BSON_TYPE_MAXKEY : BSON_TYPE_UTF8);
	SetUpperBound(&queryBounds->upperBound, &bounds);

	RegexData *regexData = (RegexData *) palloc0(sizeof(RegexData));
	CompositeRegexData *compositeRegexData = (CompositeRegexData *) palloc0(
		sizeof(CompositeRegexData));
	if (queryValue->value_type == BSON_TYPE_REGEX)
	{
		regexData->regex = queryValue->value.v_regex.regex;
		regexData->options = queryValue->value.v_regex.options;
	}
	else
	{
		regexData->regex = queryValue->value.v_utf8.str;
		regexData->options = NULL;
	}

	regexData->pcreData = RegexCompile(regexData->regex,
									   regexData->options);

	compositeRegexData->regexData = regexData;
	compositeRegexData->isNegationOperator = isNegationOp;

	IndexRecheckArgs *args = palloc0(sizeof(IndexRecheckArgs));
	args->queryDatum = (Pointer) compositeRegexData;
	args->queryStrategy = BSON_INDEX_STRATEGY_DOLLAR_REGEX;
	queryBounds->indexRecheckFunctions =
		lappend(queryBounds->indexRecheckFunctions, args);
}


static void
SetSingleBoundsDollarType(const bson_value_t *queryValue,
						  CompositeIndexBounds *queryBounds)
{
	bson_type_t typeValue = BSON_TYPE_EOD;
	if (queryValue->value_type == BSON_TYPE_UTF8)
	{
		/* Single $type */
		typeValue = GetBsonTypeNameFromStringForDollarType(
			queryValue->value.v_utf8.str);
	}
	else if (BsonValueIsNumberOrBool(queryValue))
	{
		int64_t typeCode = BsonValueAsInt64(queryValue);

		/* TryGetTypeFromInt64 should be successful as this was already validated in the planner when walking the query. */
		if (!TryGetTypeFromInt64(typeCode, &typeValue))
		{
			ereport(ERROR,
					(errcode(ERRCODE_DOCUMENTDB_BADVALUE),
					 errmsg("Invalid $type value: %ld", typeCode)));
		}
	}
	else
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE), errmsg(
							"Invalid $type value for composite index: %s",
							BsonValueToJsonForLogging(queryValue))));
	}

	if (typeValue == BSON_TYPE_ARRAY)
	{
		SetBoundsExistsTrue(queryBounds);
		queryBounds->requiresRuntimeRecheck = true;
		return;
	}

	CompositeSingleBound bounds = GetTypeLowerBound(typeValue);
	SetLowerBound(&queryBounds->lowerBound, &bounds);

	bounds = GetTypeUpperBound(typeValue);
	SetUpperBound(&queryBounds->upperBound, &bounds);

	/* TODO(Composite): Why does this need a runtime recheck */
	queryBounds->requiresRuntimeRecheck = true;
}


static void
AddMultiBoundaryForDollarIn(int32_t indexAttribute,
							pgbsonelement *queryElement, VariableIndexBounds *indexBounds)
{
	if (queryElement->bsonValue.value_type != BSON_TYPE_ARRAY)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE), errmsg(
							"$in should have an array of values")));
	}

	bson_iter_t arrayIter;
	bson_iter_init_from_data(&arrayIter, queryElement->bsonValue.value.v_doc.data,
							 queryElement->bsonValue.value.v_doc.data_len);

	bool arrayHasNull = false;
	int32_t inArraySize = 0;
	while (bson_iter_next(&arrayIter))
	{
		const bson_value_t *arrayValue = bson_iter_value(&arrayIter);

		/* if it is bson document and valid one for $in/$nin array. It fails with exact same error for both $in/$nin. */
		if (!IsValidBsonDocumentForDollarInOrNinOp(arrayValue))
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE), errmsg(
								"cannot nest $ under $in")));
		}

		inArraySize++;
		if (arrayValue->value_type == BSON_TYPE_REGEX)
		{
			/* Regex has 2 boundaries */
			inArraySize++;
		}

		if (arrayValue->value_type == BSON_TYPE_ARRAY)
		{
			/* array equals has 2 boundaries */
			inArraySize++;
		}

		arrayHasNull = arrayHasNull || arrayValue->value_type == BSON_TYPE_NULL;
	}

	bson_iter_init_from_data(&arrayIter, queryElement->bsonValue.value.v_doc.data,
							 queryElement->bsonValue.value.v_doc.data_len);

	CompositeIndexBoundsSet *set = CreateCompositeIndexBoundsSet(inArraySize,
																 indexAttribute);

	int index = 0;
	bool isNegationOp = false;
	while (bson_iter_next(&arrayIter))
	{
		if (index >= inArraySize)
		{
			ereport(ERROR, (errmsg(
								"Index is not expected to be greater than size - code defect")));
		}

		pgbsonelement element;
		element.path = queryElement->path;
		element.pathLength = queryElement->pathLength;
		element.bsonValue = *bson_iter_value(&arrayIter);

		if (element.bsonValue.value_type == BSON_TYPE_REGEX)
		{
			CompositeIndexBoundsSet *regexSet = AddMultiBoundaryForDollarRegex(index,
																			   &element,
																			   NULL,
																			   isNegationOp);
			set->bounds[index] = regexSet->bounds[0];
			set->bounds[index + 1] = regexSet->bounds[1];
			index += 2;
		}
		else if (element.bsonValue.value_type == BSON_TYPE_ARRAY)
		{
			/* Array equality has 2 boundaries */
			SetArrayEqualityBound(&element.bsonValue, &set->bounds[index]);
			index += 2;
		}
		else
		{
			SetEqualityBound(&element.bsonValue, &set->bounds[index]);
			index++;
		}
	}
	indexBounds->variableBoundsList = lappend(indexBounds->variableBoundsList, set);
}


static void
AddMultiBoundaryForDollarNotIn(int32_t indexAttribute, pgbsonelement *queryElement,
							   VariableIndexBounds *indexBounds)
{
	if (queryElement->bsonValue.value_type != BSON_TYPE_ARRAY)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE), errmsg(
							"$nin should have an array of values")));
	}

	bson_iter_t arrayIter;
	bson_iter_init_from_data(&arrayIter, queryElement->bsonValue.value.v_doc.data,
							 queryElement->bsonValue.value.v_doc.data_len);

	bool arrayHasNull = false;
	int32_t inArraySize = 0;
	bool isNegationOp = true;
	while (bson_iter_next(&arrayIter))
	{
		const bson_value_t *arrayValue = bson_iter_value(&arrayIter);

		/* if it is bson document and valid one for $in/$nin array. It fails with exact same error for both $in/$nin. */
		if (!IsValidBsonDocumentForDollarInOrNinOp(arrayValue))
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE), errmsg(
								"cannot nest $ under $nin")));
		}

		inArraySize++;
		if (arrayValue->value_type == BSON_TYPE_REGEX)
		{
			/* Regex has 2 boundaries */
			inArraySize++;
		}

		arrayHasNull = arrayHasNull || arrayValue->value_type == BSON_TYPE_NULL;
	}

	if (inArraySize == 0)
	{
		/* $nin nothing is all documents */
		CompositeIndexBoundsSet *set = CreateCompositeIndexBoundsSet(1,
																	 indexAttribute);
		SetBoundsExistsTrue(&set->bounds[0]);
		indexBounds->variableBoundsList = lappend(indexBounds->variableBoundsList, set);
		return;
	}

	bson_iter_init_from_data(&arrayIter, queryElement->bsonValue.value.v_doc.data,
							 queryElement->bsonValue.value.v_doc.data_len);

	CompositeIndexBoundsSet *set = CreateCompositeIndexBoundsSet(inArraySize,
																 indexAttribute);

	int index = 0;
	while (bson_iter_next(&arrayIter))
	{
		if (index >= inArraySize)
		{
			ereport(ERROR, (errmsg(
								"Index is not expected to be greater than size - code defect")));
		}

		pgbsonelement element;
		element.path = queryElement->path;
		element.pathLength = queryElement->pathLength;
		element.bsonValue = *bson_iter_value(&arrayIter);

		if (element.bsonValue.value_type == BSON_TYPE_REGEX)
		{
			CompositeIndexBoundsSet *regexSet = AddMultiBoundaryForDollarRegex(index,
																			   &element,
																			   NULL,
																			   isNegationOp);

			set->bounds[index] = regexSet->bounds[0];
			set->bounds[index + 1] = regexSet->bounds[1];
			index += 2;
		}
		else
		{
			SetBoundsForNotEqual(&element.bsonValue,
								 &set->bounds[index]);
			index++;
		}
	}

	indexBounds->variableBoundsList = lappend(indexBounds->variableBoundsList, set);
}


static CompositeIndexBoundsSet *
AddMultiBoundaryForDollarRegex(int32_t indexAttribute, pgbsonelement *queryElement,
							   VariableIndexBounds *indexBounds, bool isNegationOp)
{
	CompositeIndexBoundsSet *set = CreateCompositeIndexBoundsSet(2,
																 indexAttribute);

	SetSingleBoundsDollarRegex(&queryElement->bsonValue, &set->bounds[0], isNegationOp);

	/* For not operator we need to recheck because of array terms. ["ab", "ca"] we would match a regex like "c*.*" for the second term however for the first we wouldn't, so we need to go to the runtime. */
	set->bounds[0].requiresRuntimeRecheck = isNegationOp;
	set->bounds[1].requiresRuntimeRecheck = isNegationOp;

	/* The second bound is an exact match on the $regex itself */
	CompositeSingleBound equalsBounds = { 0 };
	equalsBounds.bound = queryElement->bsonValue;
	equalsBounds.isBoundInclusive = true;
	SetLowerBound(&set->bounds[1].lowerBound, &equalsBounds);
	SetUpperBound(&set->bounds[1].upperBound, &equalsBounds);

	if (indexBounds != NULL)
	{
		indexBounds->variableBoundsList = lappend(indexBounds->variableBoundsList, set);
	}

	return set;
}


static void
AddMultiBoundaryForBitwiseOperator(BsonIndexStrategy strategy,
								   int32_t indexAttribute, pgbsonelement *queryElement,
								   VariableIndexBounds *indexBounds)
{
	CompositeIndexBoundsSet *set = CreateCompositeIndexBoundsSet(2, indexAttribute);

	bson_value_t *modFilter = palloc(sizeof(bson_value_t));
	*modFilter = queryElement->bsonValue;
	IndexRecheckArgs *args = palloc0(sizeof(IndexRecheckArgs));
	args->queryDatum = (Pointer) modFilter;
	args->queryStrategy = strategy;

	/* First bound is all numbers */
	CompositeSingleBound bound = GetTypeLowerBound(BSON_TYPE_DOUBLE);
	SetLowerBound(&set->bounds[0].lowerBound, &bound);
	bound = GetTypeUpperBound(BSON_TYPE_DOUBLE);
	SetUpperBound(&set->bounds[0].upperBound, &bound);

	set->bounds[0].indexRecheckFunctions =
		lappend(set->bounds[0].indexRecheckFunctions, args);

	/* Second bound is all binary */
	bound = GetTypeLowerBound(BSON_TYPE_BINARY);
	SetLowerBound(&set->bounds[1].lowerBound, &bound);
	bound = GetTypeUpperBound(BSON_TYPE_BINARY);
	SetUpperBound(&set->bounds[1].upperBound, &bound);
	set->bounds[1].indexRecheckFunctions =
		lappend(set->bounds[1].indexRecheckFunctions, args);

	indexBounds->variableBoundsList = lappend(indexBounds->variableBoundsList, set);
}


static void
AddMultiBoundaryForNotGreater(int32_t indexAttribute, pgbsonelement *queryElement,
							  VariableIndexBounds *indexBounds, bool isEquals)
{
	CompositeIndexBoundsSet *set = CreateCompositeIndexBoundsSet(2, indexAttribute);

	/* Greater than is (minBound -> TypeMAX] */
	/* The inverse set to this is [MinKey -> minBound ] || (TypeMax -> MaxKey ]*/
	/* For $gte is [minBound -> TypeMAX] */
	/* The inverse set to this is [MinKey -> minBound ) || (TypeMax -> MaxKey ]*/

	/* First bound is [MinKey -> minBound ] */
	CompositeSingleBound bound = GetTypeLowerBound(BSON_TYPE_MINKEY);
	SetLowerBound(&set->bounds[0].lowerBound, &bound);
	bound.bound = queryElement->bsonValue;
	bound.isBoundInclusive = !isEquals;
	SetUpperBound(&set->bounds[0].upperBound, &bound);

	/* Upper bound here is the type value and not equals. However, for null,
	 * not equal to null also excludes undefined values which has empty arrays.
	 * Consequently, we set this to recheck for only this bound.
	 */
	if (queryElement->bsonValue.value_type == BSON_TYPE_NULL)
	{
		set->bounds[0].requiresRuntimeRecheck = true;
		set->bounds[0].upperBound.isBoundInclusive = true;
	}

	/* Second bound is (TypeMax -> MaxKey ] */
	bound = GetTypeUpperBound(queryElement->bsonValue.value_type);

	/* If the bound includes the largest value of the current type, forcibly exclude it */
	bound.isBoundInclusive = false;
	SetLowerBound(&set->bounds[1].lowerBound, &bound);

	bound = GetTypeUpperBound(BSON_TYPE_MAXKEY);
	SetUpperBound(&set->bounds[1].upperBound, &bound);

	/*
	 * Not functions need recheck for arrays ( given "a": [ 1, 2 ]:
	 * a not gt 1 will match on the first element)
	 */
	set->bounds[0].requiresRuntimeRecheck = true;
	set->bounds[1].requiresRuntimeRecheck = true;
	indexBounds->variableBoundsList = lappend(indexBounds->variableBoundsList, set);
}


static void
AddMultiBoundaryForNotLess(int32_t indexAttribute, pgbsonelement *queryElement,
						   VariableIndexBounds *indexBounds, bool isEquals)
{
	CompositeIndexBoundsSet *set = CreateCompositeIndexBoundsSet(2, indexAttribute);

	/* Less than is [TypeMin -> maxBound) */
	/* The inverse set to this is [MinKey -> TypeMin ) || [maxBound -> MaxKey ]*/
	/* For $lte is [TypeMin -> maxBound] */
	/* The inverse set to this is [MinKey -> TypeMin ) || (maxBound -> MaxKey ]*/

	/* First bound is [MinKey -> TypeMin ] */
	CompositeSingleBound bound = GetTypeLowerBound(BSON_TYPE_MINKEY);
	SetLowerBound(&set->bounds[0].lowerBound, &bound);

	/* Upper bound is type min: We never include the min type value */
	bound = GetTypeLowerBound(queryElement->bsonValue.value_type);
	bound.isBoundInclusive = false;
	SetUpperBound(&set->bounds[0].upperBound, &bound);

	/* If this first bound is NULL, we need to recheck due to empty arrays */
	if (queryElement->bsonValue.value_type == BSON_TYPE_NULL)
	{
		set->bounds[0].upperBound.isBoundInclusive = true;
		set->bounds[0].requiresRuntimeRecheck = true;
	}

	/* Second bound is (maxBound -> MaxKey ] */
	bound.bound = queryElement->bsonValue;
	bound.isBoundInclusive = !isEquals;
	SetLowerBound(&set->bounds[1].lowerBound, &bound);

	bound = GetTypeUpperBound(BSON_TYPE_MAXKEY);
	SetUpperBound(&set->bounds[1].upperBound, &bound);

	/*
	 * Not functions need recheck for arrays ( given "a": [ 1, 2 ]:
	 * a not lt 2 will match on the first element)
	 */
	set->bounds[0].requiresRuntimeRecheck = true;
	set->bounds[1].requiresRuntimeRecheck = true;
	indexBounds->variableBoundsList = lappend(indexBounds->variableBoundsList, set);
}


static void
AddMultiBoundaryForDollarType(int32_t indexAttribute, pgbsonelement *queryElement,
							  VariableIndexBounds *indexBounds)
{
	if (queryElement->bsonValue.value_type != BSON_TYPE_ARRAY)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE), errmsg(
							"$type should have an array of values")));
	}

	bson_iter_t arrayIter;
	bson_iter_init_from_data(&arrayIter, queryElement->bsonValue.value.v_doc.data,
							 queryElement->bsonValue.value.v_doc.data_len);

	int32_t typeArraySize = 0;
	while (bson_iter_next(&arrayIter))
	{
		typeArraySize++;
	}

	bson_iter_init_from_data(&arrayIter, queryElement->bsonValue.value.v_doc.data,
							 queryElement->bsonValue.value.v_doc.data_len);

	CompositeIndexBoundsSet *set = CreateCompositeIndexBoundsSet(typeArraySize,
																 indexAttribute);

	int index = 0;
	while (bson_iter_next(&arrayIter))
	{
		if (index >= typeArraySize)
		{
			ereport(ERROR, (errmsg(
								"Index is not expected to be greater than size - code defect")));
		}

		pgbsonelement element;
		element.path = queryElement->path;
		element.pathLength = queryElement->pathLength;
		element.bsonValue = *bson_iter_value(&arrayIter);

		SetSingleBoundsDollarType(&element.bsonValue, &set->bounds[index]);
		index++;
	}
	indexBounds->variableBoundsList = lappend(indexBounds->variableBoundsList, set);
}


static void
AddMultiBoundaryForDollarRange(int32_t indexAttribute,
							   pgbsonelement *queryElement,
							   VariableIndexBounds *indexBounds)
{
	DollarRangeParams *params = ParseQueryDollarRange(queryElement);

	if (params->isFullScan)
	{
		/* Don't update any bounds */
		return;
	}

	pgbsonelement boundElement = { 0 };
	if (params->minValue.value_type != BSON_TYPE_EOD)
	{
		CompositeIndexBoundsSet *set = CreateCompositeIndexBoundsSet(1,
																	 indexAttribute);

		BsonIndexStrategy queryStrategy = params->isMinInclusive ?
										  BSON_INDEX_STRATEGY_DOLLAR_GREATER_EQUAL :
										  BSON_INDEX_STRATEGY_DOLLAR_GREATER;
		boundElement.bsonValue = params->minValue;
		boundElement.path = queryElement->path;
		boundElement.pathLength = queryElement->pathLength;
		SetGreaterThanBounds(&boundElement.bsonValue,
							 queryStrategy, &set->bounds[0]);
		indexBounds->variableBoundsList = lappend(indexBounds->variableBoundsList, set);
	}

	if (params->maxValue.value_type != BSON_TYPE_EOD)
	{
		CompositeIndexBoundsSet *set = CreateCompositeIndexBoundsSet(1,
																	 indexAttribute);
		BsonIndexStrategy queryStrategy = params->isMaxInclusive ?
										  BSON_INDEX_STRATEGY_DOLLAR_LESS_EQUAL :
										  BSON_INDEX_STRATEGY_DOLLAR_LESS;
		boundElement.bsonValue = params->maxValue;
		boundElement.path = queryElement->path;
		boundElement.pathLength = queryElement->pathLength;
		SetLessThanBounds(&boundElement.bsonValue, queryStrategy, &set->bounds[0]);
		indexBounds->variableBoundsList = lappend(indexBounds->variableBoundsList, set);
	}
}
