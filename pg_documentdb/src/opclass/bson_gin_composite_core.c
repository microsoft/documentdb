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

bytea *
BuildLowerBoundTermFromIndexBounds(CompositeQueryRunData *runData, int32_t numPaths,
								   IndexTermCreateMetadata *metadata,
								   bool *hasInequalityMatch)
{
	pgbson_writer lower_bound_writer;
	pgbson_array_writer lower_bound_array_writer;
	PgbsonWriterInit(&lower_bound_writer);

	PgbsonWriterStartArray(&lower_bound_writer, "$", 1, &lower_bound_array_writer);
	for (int i = 0; i < numPaths; i++)
	{
		runData->requiresRuntimeRecheck = runData->requiresRuntimeRecheck ||
										  runData->indexBounds[i].requiresRuntimeRecheck;

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
SetBoundsExistsTrue(CompositeIndexBounds *queryBounds, int32_t attribute)
{
	/* This is similar to $exists: true */
	CompositeSingleBound bounds = { 0 };
	bounds.bound.value_type = BSON_TYPE_MINKEY;
	bounds.isBoundInclusive = true;
	SetLowerBound(&queryBounds[attribute].lowerBound, &bounds);

	bounds.bound.value_type = BSON_TYPE_MAXKEY;
	bounds.isBoundInclusive = true;
	SetUpperBound(&queryBounds[attribute].upperBound, &bounds);

	/* TODO: Replace this with index eval function */
	queryBounds[attribute].requiresRuntimeRecheck = true;
}


void
ParseBoundsFromStrategy(const char **indexPaths, int32_t numPaths,
						pgbsonelement *queryElement,
						BsonIndexStrategy queryStrategy,
						CompositeIndexBounds *queryBounds)
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
		case BSON_INDEX_STRATEGY_DOLLAR_EQUAL:
		{
			CompositeSingleBound equalsBounds = { 0 };
			equalsBounds.bound = queryElement->bsonValue;
			equalsBounds.isBoundInclusive = true;
			SetLowerBound(&queryBounds[i].lowerBound, &equalsBounds);
			SetUpperBound(&queryBounds[i].upperBound, &equalsBounds);
			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_GREATER_EQUAL:
		{
			if (queryElement->bsonValue.value_type == BSON_TYPE_MINKEY)
			{
				/* This is similar to $exists: true */
				SetBoundsExistsTrue(queryBounds, i);
			}
			else
			{
				CompositeSingleBound bounds = { 0 };
				bounds.bound = queryElement->bsonValue;
				bounds.isBoundInclusive = true;
				SetLowerBound(&queryBounds[i].lowerBound, &bounds);

				/* Apply type bracketing */
				bounds.bound = GetUpperBound(
					queryElement->bsonValue.value_type,
					&bounds.isBoundInclusive);
				SetUpperBound(&queryBounds[i].upperBound, &bounds);
			}

			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_GREATER:
		{
			CompositeSingleBound bounds = { 0 };
			bounds.bound = queryElement->bsonValue;
			bounds.isBoundInclusive = false;
			SetLowerBound(&queryBounds[i].lowerBound, &bounds);

			/* Apply type bracketing */
			bounds.bound = GetUpperBound(
				queryElement->bsonValue.value_type,
				&bounds.isBoundInclusive);
			SetUpperBound(&queryBounds[i].upperBound, &bounds);
			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_LESS:
		case BSON_INDEX_STRATEGY_DOLLAR_LESS_EQUAL:
		{
			CompositeSingleBound bounds = { 0 };
			bounds.bound = queryElement->bsonValue;
			bounds.isBoundInclusive = queryStrategy ==
									  BSON_INDEX_STRATEGY_DOLLAR_LESS_EQUAL;
			SetUpperBound(&queryBounds[i].upperBound, &bounds);

			/* Apply type bracketing */
			bounds.bound = GetLowerBound(
				queryElement->bsonValue.value_type,
				&bounds.isBoundInclusive);
			SetLowerBound(&queryBounds[i].lowerBound, &bounds);
			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_REGEX:
		{
			CompositeSingleBound bounds = { 0 };
			bounds.bound = GetLowerBound(BSON_TYPE_UTF8, &bounds.isBoundInclusive);
			SetLowerBound(&queryBounds[i].lowerBound, &bounds);

			bounds.bound = GetUpperBound(BSON_TYPE_UTF8, &bounds.isBoundInclusive);
			SetUpperBound(&queryBounds[i].upperBound, &bounds);

			/* TODO: Replace this with index eval function */
			queryBounds[i].requiresRuntimeRecheck = true;
			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_EXISTS:
		{
			int existsValue = BsonValueAsInt32(&queryElement->bsonValue);
			if (existsValue == 1)
			{
				/* { exists: true } */
				SetBoundsExistsTrue(queryBounds, i);
			}
			else
			{
				CompositeSingleBound equalsBounds = { 0 };
				equalsBounds.bound.value_type = BSON_TYPE_NULL;
				equalsBounds.isBoundInclusive = true;
				SetLowerBound(&queryBounds[i].lowerBound, &equalsBounds);
				SetUpperBound(&queryBounds[i].upperBound, &equalsBounds);

				/* TODO: Replace this with index eval function */
				queryBounds[i].requiresRuntimeRecheck = true;
			}

			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_ELEMMATCH:
		{
			CompositeSingleBound bounds = { 0 };
			bounds.bound = GetLowerBound(BSON_TYPE_ARRAY, &bounds.isBoundInclusive);
			SetLowerBound(&queryBounds[i].lowerBound, &bounds);

			bounds.bound = GetUpperBound(BSON_TYPE_ARRAY, &bounds.isBoundInclusive);
			SetUpperBound(&queryBounds[i].upperBound, &bounds);

			/* TODO: Replace this with index eval function */
			queryBounds[i].requiresRuntimeRecheck = true;
			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_ALL:
		{
			CompositeSingleBound bounds = { 0 };
			bounds.bound = GetLowerBound(BSON_TYPE_ARRAY, &bounds.isBoundInclusive);
			SetLowerBound(&queryBounds[i].lowerBound, &bounds);

			bounds.bound = GetUpperBound(BSON_TYPE_ARRAY, &bounds.isBoundInclusive);
			SetUpperBound(&queryBounds[i].upperBound, &bounds);

			/* TODO: Replace this with index eval function */
			queryBounds[i].requiresRuntimeRecheck = true;
			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_SIZE:
		{
			CompositeSingleBound bounds = { 0 };
			bounds.bound = GetLowerBound(BSON_TYPE_ARRAY, &bounds.isBoundInclusive);
			SetLowerBound(&queryBounds[i].lowerBound, &bounds);

			bounds.bound = GetUpperBound(BSON_TYPE_ARRAY, &bounds.isBoundInclusive);
			SetUpperBound(&queryBounds[i].upperBound, &bounds);

			/* TODO: Replace this with index eval function */
			queryBounds[i].requiresRuntimeRecheck = true;
			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_MOD:
		{
			CompositeSingleBound bounds = { 0 };
			bounds.bound = GetLowerBound(BSON_TYPE_DOUBLE, &bounds.isBoundInclusive);
			SetLowerBound(&queryBounds[i].lowerBound, &bounds);

			bounds.bound = GetUpperBound(BSON_TYPE_DOUBLE, &bounds.isBoundInclusive);
			SetUpperBound(&queryBounds[i].upperBound, &bounds);

			/* TODO: Replace this with index eval function */
			queryBounds[i].requiresRuntimeRecheck = true;
			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_TYPE:
		{
			if (queryElement->bsonValue.value_type == BSON_TYPE_UTF8)
			{
				/* Single $type */
				bson_type_t typeValue =
					GetBsonTypeNameFromStringForDollarType(
						queryElement->bsonValue.value.v_utf8.str);

				CompositeSingleBound bounds = { 0 };
				bounds.bound = GetLowerBound(typeValue, &bounds.isBoundInclusive);
				SetLowerBound(&queryBounds[i].lowerBound, &bounds);

				bounds.bound = GetUpperBound(typeValue, &bounds.isBoundInclusive);
				SetUpperBound(&queryBounds[i].upperBound, &bounds);
			}
			else if (BsonValueIsNumberOrBool(&queryElement->bsonValue))
			{
				int64_t typeCode = BsonValueAsInt64(&queryElement->bsonValue);

				/* TryGetTypeFromInt64 should be successful as this was already validated in the planner when walking the query. */
				bson_type_t resolvedType;
				if (!TryGetTypeFromInt64(typeCode, &resolvedType))
				{
					ereport(ERROR,
							(errcode(ERRCODE_DOCUMENTDB_BADVALUE),
							 errmsg("Invalid $type value: %ld", typeCode)));
				}

				CompositeSingleBound bounds = { 0 };
				bounds.bound = GetLowerBound(resolvedType, &bounds.isBoundInclusive);
				SetLowerBound(&queryBounds[i].lowerBound, &bounds);

				bounds.bound = GetUpperBound(resolvedType, &bounds.isBoundInclusive);
				SetUpperBound(&queryBounds[i].upperBound, &bounds);
			}
			else if (queryElement->bsonValue.value_type == BSON_TYPE_ARRAY)
			{
				/* Array of types */
				/* TODO: Support multiple boundaries */
				CompositeSingleBound bounds = { 0 };
				bounds.bound.value_type = BSON_TYPE_MINKEY;
				bounds.isBoundInclusive = true;
				SetLowerBound(&queryBounds[i].lowerBound, &bounds);

				bounds.bound.value_type = BSON_TYPE_MAXKEY;
				bounds.isBoundInclusive = true;
				SetUpperBound(&queryBounds[i].lowerBound, &bounds);

				/* TODO: Replace this with index eval function */
				queryBounds[i].requiresRuntimeRecheck = true;
			}
			else
			{
				ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE), errmsg(
									"Invalid $type value for composite index: %s",
									BsonValueToJsonForLogging(
										&queryElement->bsonValue))));
			}

			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_RANGE:
		{
			DollarRangeParams *params = ParseQueryDollarRange(queryElement);
			CompositeSingleBound bounds = { 0 };
			if (params->minValue.value_type == BSON_TYPE_MINKEY &&
				params->isMinInclusive)
			{
				SetBoundsExistsTrue(queryBounds, i);
			}
			else if (params->minValue.value_type != BSON_TYPE_EOD)
			{
				bounds.bound = params->minValue;
				bounds.isBoundInclusive = params->isMinInclusive;
				SetLowerBound(&queryBounds[i].lowerBound, &bounds);
			}

			if (params->maxValue.value_type != BSON_TYPE_EOD)
			{
				bounds.bound = params->maxValue;
				bounds.isBoundInclusive = params->isMaxInclusive;
				SetUpperBound(&queryBounds[i].upperBound, &bounds);
			}

			break;
		}

		case BSON_INDEX_STRATEGY_DOLLAR_IN:
		case BSON_INDEX_STRATEGY_DOLLAR_NOT_EQUAL:
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
			/* TODO: Support multiple boundaries */
			CompositeSingleBound bounds = { 0 };
			bounds.bound.value_type = BSON_TYPE_MINKEY;
			bounds.isBoundInclusive = true;
			SetLowerBound(&queryBounds[i].lowerBound, &bounds);

			bounds.bound.value_type = BSON_TYPE_MAXKEY;
			bounds.isBoundInclusive = true;
			SetUpperBound(&queryBounds[i].lowerBound, &bounds);

			/* TODO: Replace this with index eval function */
			queryBounds[i].requiresRuntimeRecheck = true;
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
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg(
								"Unsupported strategy for composite index: %d",
								queryStrategy)));
			break;
		}
	}
}
