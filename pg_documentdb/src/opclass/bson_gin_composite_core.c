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
 #include "opclass/bson_gin_composite_private.h"

static void ProcessBoundForQuery(CompositeSingleBound *bound, const
								 IndexTermCreateMetadata *metadata);
static void SetSingleRangeBoundsFromStrategy(pgbsonelement *queryElement,
											 BsonIndexStrategy queryStrategy,
											 CompositeIndexBounds *queryBounds);


static void SetUpperBound(CompositeSingleBound *currentBoundValue, const
						  CompositeSingleBound *upperBound);
static void SetLowerBound(CompositeSingleBound *currentBoundValue, const
						  CompositeSingleBound *lowerBound);

static void AddMultiBoundaryForDollarAll(int32_t indexAttribute,
										 pgbsonelement *queryElement,
										 VariableIndexBounds *indexBounds);
static void AddMultiBoundaryForDollarIn(int32_t indexAttribute,
										pgbsonelement *queryElement,
										VariableIndexBounds *indexBounds);
static void AddMultiBoundaryForDollarType(int32_t indexAttribute,
										  pgbsonelement *queryElement,
										  VariableIndexBounds *indexBounds);
static void AddMultiBoundaryForDollarNotIn(int32_t indexAttribute,
										   pgbsonelement *queryElement,
										   VariableIndexBounds *indexBounds);
static void AddMultiBoundaryForBitwiseOperator(int32_t indexAttribute,
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
	pgbson_writer lower_bound_writer;
	pgbson_array_writer lower_bound_array_writer;
	PgbsonWriterInit(&lower_bound_writer);

	PgbsonWriterStartArray(&lower_bound_writer, "$", 1, &lower_bound_array_writer);
	for (int i = 0; i < runData->numIndexPaths; i++)
	{
		runData->metaInfo->requiresRuntimeRecheck =
			runData->metaInfo->requiresRuntimeRecheck ||
			runData->indexBounds[i].
			requiresRuntimeRecheck;

		/* If both lower and upper bound match it's equality */
		if (runData->indexBounds[i].lowerBound.bound.value_type != BSON_TYPE_EOD &&
			runData->indexBounds[i].upperBound.bound.value_type != BSON_TYPE_EOD &&
			runData->indexBounds[i].lowerBound.isBoundInclusive &&
			runData->indexBounds[i].upperBound.isBoundInclusive &&
			BsonValueEquals(&runData->indexBounds[i].lowerBound.bound,
							&runData->indexBounds[i].upperBound.bound))
		{
			runData->indexBounds[i].isEqualityBound = true;
			PgbsonArrayWriterWriteValue(&lower_bound_array_writer,
										&runData->indexBounds[i].lowerBound.
										processedBoundValue);
			continue;
		}

		*hasInequalityMatch = true;
		if (runData->indexBounds[i].lowerBound.bound.value_type != BSON_TYPE_EOD)
		{
			/* There exists a lower bound for this key */
			PgbsonArrayWriterWriteValue(&lower_bound_array_writer,
										&runData->indexBounds[i].lowerBound.
										processedBoundValue);
		}
		else
		{
			/* All possible values are valid for this key */
			bson_value_t minValue = { 0 };
			minValue.value_type = BSON_TYPE_MINKEY;
			PgbsonArrayWriterWriteValue(&lower_bound_array_writer,
										&minValue);
		}
	}

	PgbsonWriterEndArray(&lower_bound_writer, &lower_bound_array_writer);

	pgbsonelement lower_bound_element = { 0 };
	lower_bound_element.path = "$";
	lower_bound_element.pathLength = 1;
	lower_bound_element.bsonValue = PgbsonArrayWriterGetValue(&lower_bound_array_writer);

	BsonIndexTermSerialized ser = SerializeBsonIndexTerm(&lower_bound_element, metadata);
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

	/* Now that we have the index path, add or update the bounds */
	switch (queryStrategy)
	{
		/* Single bound operators */
		case BSON_INDEX_STRATEGY_DOLLAR_EQUAL:
		case BSON_INDEX_STRATEGY_DOLLAR_GREATER_EQUAL:
		case BSON_INDEX_STRATEGY_DOLLAR_GREATER:
		case BSON_INDEX_STRATEGY_DOLLAR_LESS:
		case BSON_INDEX_STRATEGY_DOLLAR_LESS_EQUAL:
		case BSON_INDEX_STRATEGY_DOLLAR_REGEX:
		case BSON_INDEX_STRATEGY_DOLLAR_EXISTS:
		case BSON_INDEX_STRATEGY_DOLLAR_ELEMMATCH:
		case BSON_INDEX_STRATEGY_DOLLAR_SIZE:
		case BSON_INDEX_STRATEGY_DOLLAR_MOD:
		case BSON_INDEX_STRATEGY_DOLLAR_NOT_EQUAL:
		{
			int numterms = 1;
			CompositeIndexBoundsSet *set = CreateCompositeIndexBoundsSet(numterms, i);
			SetSingleRangeBoundsFromStrategy(queryElement,
											 queryStrategy, set->bounds);
			indexBounds->variableBoundsList = lappend(indexBounds->variableBoundsList,
													  set);
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
				int numterms = 1;
				CompositeIndexBoundsSet *set = CreateCompositeIndexBoundsSet(numterms, i);
				SetSingleRangeBoundsFromStrategy(queryElement,
												 queryStrategy, set->bounds);
				indexBounds->variableBoundsList = lappend(indexBounds->variableBoundsList,
														  set);
			}

			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_ALL:
		{
			AddMultiBoundaryForDollarAll(i, queryElement, indexBounds);
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
			AddMultiBoundaryForBitwiseOperator(i, queryElement, indexBounds);
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
		pfree(serialized.indexTermVal);
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

	/* TODO: Replace this with index eval function */
	queryBounds->requiresRuntimeRecheck = true;
}


static void
SetSingleRangeBoundsFromStrategy(pgbsonelement *queryElement,
								 BsonIndexStrategy queryStrategy,
								 CompositeIndexBounds *queryBounds)
{
	/* Now that we have the index path, add or update the bounds */
	switch (queryStrategy)
	{
		case BSON_INDEX_STRATEGY_DOLLAR_EQUAL:
		{
			CompositeSingleBound equalsBounds = { 0 };
			equalsBounds.bound = queryElement->bsonValue;
			equalsBounds.isBoundInclusive = true;
			SetLowerBound(&queryBounds->lowerBound, &equalsBounds);
			SetUpperBound(&queryBounds->upperBound, &equalsBounds);


			if (queryElement->bsonValue.value_type == BSON_TYPE_NULL)
			{
				/* Special case, requires runtime recheck always */
				queryBounds->requiresRuntimeRecheck = true;
			}
			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_GREATER_EQUAL:
		{
			if (queryElement->bsonValue.value_type == BSON_TYPE_MINKEY)
			{
				/* This is similar to $exists: true */
				SetBoundsExistsTrue(queryBounds);
			}
			else
			{
				CompositeSingleBound bounds = { 0 };
				bounds.bound = queryElement->bsonValue;
				bounds.isBoundInclusive = true;
				SetLowerBound(&queryBounds->lowerBound, &bounds);

				/* Apply type bracketing */
				bounds = GetTypeUpperBound(queryElement->bsonValue.value_type);
				SetUpperBound(&queryBounds->upperBound, &bounds);
			}

			if (queryElement->bsonValue.value_type == BSON_TYPE_NULL)
			{
				/* Special case, requires runtime recheck always */
				queryBounds->requiresRuntimeRecheck = true;
			}

			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_GREATER:
		{
			CompositeSingleBound bounds = { 0 };
			bounds.bound = queryElement->bsonValue;
			bounds.isBoundInclusive = false;
			SetLowerBound(&queryBounds->lowerBound, &bounds);

			/* Apply type bracketing */
			bounds = GetTypeUpperBound(queryElement->bsonValue.value_type);
			SetUpperBound(&queryBounds->upperBound, &bounds);
			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_LESS:
		case BSON_INDEX_STRATEGY_DOLLAR_LESS_EQUAL:
		{
			CompositeSingleBound bounds = { 0 };
			bounds.bound = queryElement->bsonValue;
			bounds.isBoundInclusive = queryStrategy ==
									  BSON_INDEX_STRATEGY_DOLLAR_LESS_EQUAL;
			SetUpperBound(&queryBounds->upperBound, &bounds);

			/* Apply type bracketing */
			bounds = GetTypeLowerBound(queryElement->bsonValue.value_type);
			SetLowerBound(&queryBounds->lowerBound, &bounds);

			if (queryElement->bsonValue.value_type == BSON_TYPE_NULL)
			{
				/* Special case, requires runtime recheck always */
				queryBounds->requiresRuntimeRecheck = true;
			}
			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_REGEX:
		{
			CompositeSingleBound bounds = GetTypeLowerBound(BSON_TYPE_UTF8);
			SetLowerBound(&queryBounds->lowerBound, &bounds);

			bounds = GetTypeUpperBound(BSON_TYPE_UTF8);
			SetUpperBound(&queryBounds->upperBound, &bounds);

			/* TODO: Replace this with index eval function */
			queryBounds->requiresRuntimeRecheck = true;
			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_EXISTS:
		{
			int existsValue = BsonValueAsInt32(&queryElement->bsonValue);
			if (existsValue == 1)
			{
				/* { exists: true } */
				SetBoundsExistsTrue(queryBounds);
			}
			else
			{
				CompositeSingleBound equalsBounds = { 0 };
				equalsBounds.bound.value_type = BSON_TYPE_NULL;
				equalsBounds.isBoundInclusive = true;
				SetLowerBound(&queryBounds->lowerBound, &equalsBounds);
				SetUpperBound(&queryBounds->upperBound, &equalsBounds);

				/* TODO: Replace this with index eval function */
				queryBounds->requiresRuntimeRecheck = true;
			}

			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_ELEMMATCH:
		{
			CompositeSingleBound bounds = GetTypeLowerBound(BSON_TYPE_ARRAY);
			SetLowerBound(&queryBounds->lowerBound, &bounds);

			bounds = GetTypeUpperBound(BSON_TYPE_ARRAY);
			SetUpperBound(&queryBounds->upperBound, &bounds);

			/* TODO: Replace this with index eval function */
			queryBounds->requiresRuntimeRecheck = true;
			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_SIZE:
		{
			CompositeSingleBound bounds = GetTypeLowerBound(BSON_TYPE_ARRAY);
			SetLowerBound(&queryBounds->lowerBound, &bounds);

			bounds = GetTypeUpperBound(BSON_TYPE_ARRAY);
			SetUpperBound(&queryBounds->upperBound, &bounds);

			/* TODO: Replace this with index eval function */
			queryBounds->requiresRuntimeRecheck = true;
			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_MOD:
		{
			CompositeSingleBound bounds = GetTypeLowerBound(BSON_TYPE_DOUBLE);
			SetLowerBound(&queryBounds->lowerBound, &bounds);

			bounds = GetTypeUpperBound(BSON_TYPE_DOUBLE);
			SetUpperBound(&queryBounds->upperBound, &bounds);

			/* TODO: Replace this with index eval function */
			queryBounds->requiresRuntimeRecheck = true;
			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_TYPE:
		{
			bson_type_t typeValue = BSON_TYPE_EOD;
			if (queryElement->bsonValue.value_type == BSON_TYPE_UTF8)
			{
				/* Single $type */
				typeValue = GetBsonTypeNameFromStringForDollarType(
					queryElement->bsonValue.value.v_utf8.str);
			}
			else if (BsonValueIsNumberOrBool(&queryElement->bsonValue))
			{
				int64_t typeCode = BsonValueAsInt64(&queryElement->bsonValue);

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
									BsonValueToJsonForLogging(
										&queryElement->bsonValue))));
			}

			CompositeSingleBound bounds = GetTypeLowerBound(typeValue);
			SetLowerBound(&queryBounds->lowerBound, &bounds);

			bounds = GetTypeUpperBound(typeValue);
			SetUpperBound(&queryBounds->upperBound, &bounds);

			queryBounds->requiresRuntimeRecheck = true;

			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_RANGE:
		{
			DollarRangeParams *params = ParseQueryDollarRange(queryElement);
			CompositeSingleBound bounds = { 0 };
			if (params->minValue.value_type == BSON_TYPE_MINKEY &&
				params->isMinInclusive)
			{
				SetBoundsExistsTrue(queryBounds);
			}
			else if (params->minValue.value_type != BSON_TYPE_EOD)
			{
				bounds.bound = params->minValue;
				bounds.isBoundInclusive = params->isMinInclusive;
				SetLowerBound(&queryBounds->lowerBound, &bounds);
			}

			if (params->maxValue.value_type != BSON_TYPE_EOD)
			{
				bounds.bound = params->maxValue;
				bounds.isBoundInclusive = params->isMaxInclusive;
				SetUpperBound(&queryBounds->upperBound, &bounds);
			}

			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_NOT_EQUAL:
		{
			CompositeSingleBound bounds = GetTypeLowerBound(BSON_TYPE_MINKEY);
			SetLowerBound(&queryBounds->lowerBound, &bounds);

			bounds = GetTypeUpperBound(BSON_TYPE_MAXKEY);
			SetUpperBound(&queryBounds->upperBound, &bounds);

			/* TODO: Replace this with index eval function */
			queryBounds->requiresRuntimeRecheck = true;
			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_ALL:
		case BSON_INDEX_STRATEGY_DOLLAR_IN:
		case BSON_INDEX_STRATEGY_DOLLAR_NOT_IN:
		case BSON_INDEX_STRATEGY_DOLLAR_BITS_ALL_CLEAR:
		case BSON_INDEX_STRATEGY_DOLLAR_BITS_ANY_CLEAR:
		case BSON_INDEX_STRATEGY_DOLLAR_BITS_ALL_SET:
		case BSON_INDEX_STRATEGY_DOLLAR_BITS_ANY_SET:
		case BSON_INDEX_STRATEGY_DOLLAR_NOT_GT:
		case BSON_INDEX_STRATEGY_DOLLAR_NOT_GTE:
		case BSON_INDEX_STRATEGY_DOLLAR_NOT_LT:
		case BSON_INDEX_STRATEGY_DOLLAR_NOT_LTE:
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_INTERNALERROR), errmsg(
								"Unsupported strategy for single range strategy composite index: %d",
								queryStrategy)));
			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_TEXT:
		case BSON_INDEX_STRATEGY_DOLLAR_GEOINTERSECTS:
		case BSON_INDEX_STRATEGY_DOLLAR_GEOWITHIN:
		case BSON_INDEX_STRATEGY_GEONEAR:
		case BSON_INDEX_STRATEGY_GEONEAR_RANGE:
		case BSON_INDEX_STRATEGY_COMPOSITE_QUERY:
		case BSON_INDEX_STRATEGY_UNIQUE_EQUAL:
		case BSON_INDEX_STRATEGY_DOLLAR_ORDERBY:
		default:
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_INTERNALERROR), errmsg(
								"Unsupported strategy for composite index: %d",
								queryStrategy)));
			break;
		}
	}
}


static void
AddMultiBoundaryForDollarAll(int32_t indexAttribute,
							 pgbsonelement *queryElement,
							 VariableIndexBounds *indexBounds)
{
	if (queryElement->bsonValue.value_type != BSON_TYPE_ARRAY)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE), errmsg(
							"$all should have an array of values")));
	}

	bson_iter_t arrayIter;
	bson_iter_init_from_data(&arrayIter, queryElement->bsonValue.value.v_doc.data,
							 queryElement->bsonValue.value.v_doc.data_len);

	int32_t allArraySize = 0;
	while (bson_iter_next(&arrayIter))
	{
		allArraySize++;
	}

	bson_iter_init_from_data(&arrayIter, queryElement->bsonValue.value.v_doc.data,
							 queryElement->bsonValue.value.v_doc.data_len);

	CompositeIndexBoundsSet *set = CreateCompositeIndexBoundsSet(allArraySize,
																 indexAttribute);

	int index = 0;
	while (bson_iter_next(&arrayIter))
	{
		if (index >= allArraySize)
		{
			ereport(ERROR, (errmsg(
								"Index is not expected to be greater than size - code defect")));
		}

		pgbsonelement innerDocumentElement;
		pgbsonelement element;
		element.path = queryElement->path;
		element.pathLength = queryElement->pathLength;
		element.bsonValue = *bson_iter_value(&arrayIter);

		if (element.bsonValue.value_type == BSON_TYPE_REGEX)
		{
			SetSingleRangeBoundsFromStrategy(&element, BSON_INDEX_STRATEGY_DOLLAR_REGEX,
											 &set->bounds[index]);
		}
		else if (element.bsonValue.value_type == BSON_TYPE_DOCUMENT &&
				 TryGetBsonValueToPgbsonElement(&element.bsonValue,
												&innerDocumentElement) &&
				 strcmp(innerDocumentElement.path, "$elemMatch") == 0)
		{
			SetSingleRangeBoundsFromStrategy(&element,
											 BSON_INDEX_STRATEGY_DOLLAR_ELEMMATCH,
											 &set->bounds[index]);
		}
		else
		{
			SetSingleRangeBoundsFromStrategy(&element, BSON_INDEX_STRATEGY_DOLLAR_EQUAL,
											 &set->bounds[index]);
		}

		/* TODO: Since we represet $all as a $in at the index layer, we need runtime recheck */
		set->bounds[index].requiresRuntimeRecheck = true;
		index++;
	}
	indexBounds->variableBoundsList = lappend(indexBounds->variableBoundsList, set);
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
		arrayHasNull = arrayHasNull || arrayValue->value_type == BSON_TYPE_NULL;
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
			SetSingleRangeBoundsFromStrategy(&element, BSON_INDEX_STRATEGY_DOLLAR_REGEX,
											 &set->bounds[index]);
		}
		else
		{
			SetSingleRangeBoundsFromStrategy(&element, BSON_INDEX_STRATEGY_DOLLAR_EQUAL,
											 &set->bounds[index]);
		}

		index++;
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
			/* TODO: Handle this */
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_COMMANDNOTSUPPORTED),
							errmsg(
								"The $nin operator does not support regex patterns yet.")));
		}
		else
		{
			SetSingleRangeBoundsFromStrategy(&element,
											 BSON_INDEX_STRATEGY_DOLLAR_NOT_EQUAL,
											 &set->bounds[index]);
		}

		index++;
	}
	indexBounds->variableBoundsList = lappend(indexBounds->variableBoundsList, set);
}


static void
AddMultiBoundaryForBitwiseOperator(int32_t indexAttribute, pgbsonelement *queryElement,
								   VariableIndexBounds *indexBounds)
{
	CompositeIndexBoundsSet *set = CreateCompositeIndexBoundsSet(2, indexAttribute);

	/* First bound is all numbers */
	CompositeSingleBound bound = GetTypeLowerBound(BSON_TYPE_DOUBLE);
	SetLowerBound(&set->bounds[0].lowerBound, &bound);
	bound = GetTypeUpperBound(BSON_TYPE_DOUBLE);
	SetUpperBound(&set->bounds[0].upperBound, &bound);

	/* Second bound is all binary */
	bound = GetTypeLowerBound(BSON_TYPE_BINARY);
	SetLowerBound(&set->bounds[1].lowerBound, &bound);
	bound = GetTypeUpperBound(BSON_TYPE_BINARY);
	SetUpperBound(&set->bounds[1].upperBound, &bound);

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

	/* Second bound is (TypeMax -> MaxKey ] */
	bound = GetTypeUpperBound(queryElement->bsonValue.value_type);

	/* If the bound includes the largest value of the current type, forcibly exclude it */
	bound.isBoundInclusive = false;
	SetLowerBound(&set->bounds[1].lowerBound, &bound);

	bound = GetTypeUpperBound(BSON_TYPE_MAXKEY);
	SetUpperBound(&set->bounds[1].upperBound, &bound);

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

	/* Second bound is (maxBound -> MaxKey ] */
	bound.bound = queryElement->bsonValue;
	bound.isBoundInclusive = !isEquals;
	SetLowerBound(&set->bounds[1].lowerBound, &bound);

	bound = GetTypeUpperBound(BSON_TYPE_MAXKEY);
	SetUpperBound(&set->bounds[1].upperBound, &bound);

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

		SetSingleRangeBoundsFromStrategy(&element, BSON_INDEX_STRATEGY_DOLLAR_TYPE,
										 &set->bounds[index]);
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

	CompositeSingleBound bounds = { 0 };
	if (params->minValue.value_type == BSON_TYPE_MINKEY &&
		params->isMinInclusive)
	{
		CompositeIndexBoundsSet *set = CreateCompositeIndexBoundsSet(1,
																	 indexAttribute);
		SetBoundsExistsTrue(&set->bounds[0]);
		indexBounds->variableBoundsList = lappend(indexBounds->variableBoundsList, set);
	}
	else if (params->minValue.value_type != BSON_TYPE_EOD)
	{
		bounds.bound = params->minValue;
		bounds.isBoundInclusive = params->isMinInclusive;
		CompositeIndexBoundsSet *set = CreateCompositeIndexBoundsSet(1,
																	 indexAttribute);
		SetLowerBound(&set->bounds[0].lowerBound, &bounds);

		/* Apply type bracketing */
		bounds = GetTypeUpperBound(params->minValue.value_type);
		SetUpperBound(&set->bounds[0].upperBound, &bounds);
		indexBounds->variableBoundsList = lappend(indexBounds->variableBoundsList, set);
	}

	if (params->maxValue.value_type != BSON_TYPE_EOD)
	{
		CompositeIndexBoundsSet *set = CreateCompositeIndexBoundsSet(1,
																	 indexAttribute);
		bounds.bound = params->maxValue;
		bounds.isBoundInclusive = params->isMaxInclusive;
		SetUpperBound(&set->bounds[0].upperBound, &bounds);

		/* Apply type bracketing */
		bounds = GetTypeLowerBound(params->maxValue.value_type);
		SetLowerBound(&set->bounds[0].upperBound, &bounds);
		indexBounds->variableBoundsList = lappend(indexBounds->variableBoundsList, set);
	}
}
