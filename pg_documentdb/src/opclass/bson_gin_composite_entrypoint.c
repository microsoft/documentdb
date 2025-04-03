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
 #include "opclass/bson_gin_composite_scan.h"


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
	bool requiresRuntimeRecheck;
} CompositeQueryRunData;

/* --------------------------------------------------------- */
/* Top level exports */
/* --------------------------------------------------------- */
PG_FUNCTION_INFO_V1(gin_bson_composite_path_extract_value);
PG_FUNCTION_INFO_V1(gin_bson_composite_path_extract_query);
PG_FUNCTION_INFO_V1(gin_bson_composite_path_compare_partial);
PG_FUNCTION_INFO_V1(gin_bson_composite_path_consistent);
PG_FUNCTION_INFO_V1(gin_bson_composite_path_options);
PG_FUNCTION_INFO_V1(gin_bson_get_composite_path_generated_terms);

extern bool EnableCollation;
extern bool EnableNewCompositeIndexOpclass;

static void ValidateCompositePathSpec(const char *prefix);
static Size FillCompositePathSpec(const char *prefix, void *buffer);
static Datum * GenerateCompositeTermsCore(pgbson *doc,
										  BsonGinCompositePathOptions *options,
										  int32_t *nentries);
static int32_t GetIndexPathsFromOptions(BsonGinCompositePathOptions *options,
										const char **indexPaths);
static void ParseBoundsFromValue(const char **indexPaths, int32_t numPaths,
								 pgbsonelement *queryElement,
								 BsonIndexStrategy queryStrategy,
								 CompositeIndexBounds *queryBounds);
static void ParseBoundsForCompositeOperator(pgbsonelement *singleElement, const
											char **indexPaths, int32_t numPaths,
											CompositeIndexBounds *queryBounds);
static void ProcessBoundForQuery(CompositeSingleBound *bound, const
								 IndexTermCreateMetadata *metadata);

inline static int32_t
GetSinglePathTruncationLimit(int32_t compositeTruncationLimit, int32_t numPaths)
{
	/* allocate 4 bytes for each path\0 + 1 typecode within the truncated array */
	return (compositeTruncationLimit / numPaths) - 4;
}


/*
 * gin_bson_composite_path_extract_value is run on the insert/update path and collects the terms
 * that will be indexed for indexes for a single path definition. the method provides the bson document as an input, and
 * can return as many terms as is necessary (1:N).
 * For more details see documentation on the 'extractValue' method in the GIN extensibility.
 */
Datum
gin_bson_composite_path_extract_value(PG_FUNCTION_ARGS)
{
	pgbson *bson = PG_GETARG_PGBSON_PACKED(0);
	int32_t *nentries = (int32_t *) PG_GETARG_POINTER(1);
	if (!PG_HAS_OPCLASS_OPTIONS())
	{
		ereport(ERROR, (errmsg("Index does not have options")));
	}

	BsonGinCompositePathOptions *options =
		(BsonGinCompositePathOptions *) PG_GET_OPCLASS_OPTIONS();

	Datum *indexEntries = GenerateCompositeTermsCore(bson, options, nentries);
	PG_RETURN_POINTER(indexEntries);
}


/*
 * gin_bson_composite_path_extract_query is run on the query path when a predicate could be pushed
 * to the index. The predicate and the "strategy" based on the operator is passed down.
 * In the operator class, the OPERATOR index maps to the strategy index presented here.
 * The method then returns a set of terms that are valid for that predicate and strategy.
 * For more details see documentation on the 'extractQuery' method in the GIN extensibility.
 * TODO: Today this recurses through the given document fully. We would need to implement
 * something that recurses down 1 level of objects & arrays for a given path unless it's a wildcard
 * index.
 */
Datum
gin_bson_composite_path_extract_query(PG_FUNCTION_ARGS)
{
	StrategyNumber strategy = PG_GETARG_UINT16(2);
	pgbson *query = PG_GETARG_PGBSON(0);
	int32 *nentries = (int32 *) PG_GETARG_POINTER(1);
	bool **partialmatch = (bool **) PG_GETARG_POINTER(3);
	Pointer **extra_data = (Pointer **) PG_GETARG_POINTER(4);

	if (!PG_HAS_OPCLASS_OPTIONS())
	{
		ereport(ERROR, (errmsg("Index does not have options")));
	}

	BsonGinCompositePathOptions *options =
		(BsonGinCompositePathOptions *) PG_GET_OPCLASS_OPTIONS();

	/* We need to handle this case for amcostestimate - let
	 * compare partial and consistent handle failures.
	 */
	const char *indexPaths[INDEX_MAX_KEYS] = { 0 };

	int numPaths = GetIndexPathsFromOptions(
		options,
		indexPaths);


	CompositeQueryRunData *runData = (CompositeQueryRunData *) palloc0(
		sizeof(CompositeQueryRunData));
	runData->numIndexPaths = numPaths;

	pgbsonelement singleElement;
	PgbsonToSinglePgbsonElement(query, &singleElement);

	if (strategy != BSON_INDEX_STRATEGY_COMPOSITE_QUERY)
	{
		/* Could be for cost estimate or regular index
		 * in this path, just treat it as valid. let
		 * compare partial and consistent handle errors.
		 */

		ParseBoundsFromValue(indexPaths, numPaths, &singleElement, strategy,
							 runData->indexBounds);
	}
	else
	{
		ParseBoundsForCompositeOperator(&singleElement, indexPaths, numPaths,
										runData->indexBounds);
	}

	IndexTermCreateMetadata singlePathMetadata = GetIndexTermMetadata(options);
	singlePathMetadata.indexTermSizeLimit =
		GetSinglePathTruncationLimit(singlePathMetadata.indexTermSizeLimit,
									 numPaths);

	/* For the next phase, process each term and handle truncation */
	for (int i = 0; i < numPaths; i++)
	{
		if (runData->indexBounds[i].lowerBound.bound.value_type != BSON_TYPE_EOD)
		{
			ProcessBoundForQuery(&runData->indexBounds[i].lowerBound,
								 &singlePathMetadata);
			runData->hasTruncation = runData->hasTruncation ||
									 runData->indexBounds[i].lowerBound.
									 isProcessedValueTruncated;
		}

		if (runData->indexBounds[i].upperBound.bound.value_type != BSON_TYPE_EOD)
		{
			ProcessBoundForQuery(&runData->indexBounds[i].upperBound,
								 &singlePathMetadata);
			runData->hasTruncation = runData->hasTruncation ||
									 runData->indexBounds[i].upperBound.
									 isProcessedValueTruncated;
		}
	}

	/*
	 * Now that we have the bounds from the operators, generate index terms - we will either have
	 * one or two terms.
	 */
	pgbson_writer lower_bound_writer;
	pgbson_array_writer lower_bound_array_writer;
	PgbsonWriterInit(&lower_bound_writer);

	PgbsonWriterStartArray(&lower_bound_writer, "$", 1, &lower_bound_array_writer);

	bool hasInequalityMatch = false;
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

		hasInequalityMatch = true;
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

	IndexTermCreateMetadata metadata = GetIndexTermMetadata(options);

	*nentries = 1;

	*partialmatch = (bool *) palloc0(sizeof(bool) * 2);
	if (hasInequalityMatch)
	{
		/* indicate that we have partial matches */
		(*partialmatch)[0] = true;
	}

	*extra_data = palloc0(sizeof(Pointer) * 2);
	(*extra_data)[0] = (Pointer) runData;

	pgbsonelement lower_bound_element = { 0 };
	lower_bound_element.path = "$";
	lower_bound_element.pathLength = 1;
	lower_bound_element.bsonValue = PgbsonArrayWriterGetValue(&lower_bound_array_writer);

	Datum *entries = (Datum *) palloc(sizeof(Datum) * 2);
	BsonIndexTermSerialized ser = SerializeBsonIndexTerm(&lower_bound_element, &metadata);
	entries[0] = PointerGetDatum(ser.indexTermVal);

	if (runData->hasTruncation)
	{
		*nentries = 2;
		entries[1] = GenerateRootTruncatedTerm(&metadata);
		(*partialmatch)[1] = false;
		(*extra_data)[1] = NULL;  /* no extra data for the truncated term */
	}

	PG_RETURN_POINTER(entries);
}


/*
 * gin_bson_composite_path_compare_partial is run on the query path when extract_query requests a partial
 * match on the index. Each index term that has a partial match (with the lower bound as a
 * starting point) will be an input to this method. compare_partial will return '0' if the term
 * is a match, '-1' if the term is not a match but enumeration should continue, and '1' if
 * enumeration should stop. Note that enumeration may happen multiple times - this sorted enumeration
 * happens once per GIN page so there may be several sequences of [-1, 0]* -> 1 per query.
 * The strategy passed in will map to the index of the Operator on the OPERATOR class definition
 * For more details see documentation on the 'comparePartial' method in the GIN extensibility.
 */
Datum
gin_bson_composite_path_compare_partial(PG_FUNCTION_ARGS)
{
	/* 0 will be the value we passed in for the extract query */
	/* bytea *queryValue = PG_GETARG_BYTEA_PP(0); */

	/* 1 is the value in the index we want to compare against. */
	bytea *compareValue = PG_GETARG_BYTEA_PP(1);
	StrategyNumber strategy = PG_GETARG_UINT16(2);
	Pointer extraData = PG_GETARG_POINTER(3);

	if (strategy != BSON_INDEX_STRATEGY_COMPOSITE_QUERY)
	{
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("Composite index does not support strategy %d",
							   strategy)));
	}

	CompositeQueryRunData *runData = (CompositeQueryRunData *) extraData;

	BsonIndexTerm compareTerm;
	InitializeBsonIndexTerm(compareValue, &compareTerm);

	bson_iter_t compareIter;
	BsonValueInitIterator(&compareTerm.element.bsonValue, &compareIter);

	int32_t compareIndex = -1;
	bool priorMatchesEquality = true;
	bool hasEqualityPrefix = true;
	while (bson_iter_next(&compareIter))
	{
		compareIndex++;

		if (compareIndex >= runData->numIndexPaths)
		{
			/* We have more terms than we have index paths - this is not a match */
			return -1;
		}

		hasEqualityPrefix = hasEqualityPrefix && priorMatchesEquality;
		const bson_value_t *compareValue = bson_iter_value(&compareIter);
		if (runData->indexBounds[compareIndex].isEqualityBound)
		{
			/* We have an equality on a term - if not equal - we can bail */
			if (!BsonValueEquals(compareValue,
								 &runData->indexBounds[compareIndex].lowerBound.
								 processedBoundValue))
			{
				/* Stop the search */
				return hasEqualityPrefix ? 1 : -1;
			}

			continue;
		}

		priorMatchesEquality = false;
		if (runData->indexBounds[compareIndex].lowerBound.bound.value_type !=
			BSON_TYPE_EOD)
		{
			bool isComparisonValid = false;
			int32_t compareBounds = CompareBsonValueAndType(
				compareValue,
				&runData->indexBounds[compareIndex].lowerBound.processedBoundValue,
				&isComparisonValid);
			if (!isComparisonValid)
			{
				return -1;
			}

			if (compareBounds == 0)
			{
				if (!runData->indexBounds[compareIndex].lowerBound.isBoundInclusive)
				{
					/* for truncated you can't be sure - let the runtime re-evaluate */
					return runData->indexBounds[compareIndex].lowerBound.
						   isProcessedValueTruncated ? 0 : -1;
				}
				else
				{
					continue;
				}
			}

			if (compareBounds < 0)
			{
				/* compareValue < lowerBound, not a match */
				return -1;
			}
		}

		if (runData->indexBounds[compareIndex].upperBound.bound.value_type !=
			BSON_TYPE_EOD)
		{
			bool isComparisonValid = false;
			int32_t compareBounds = CompareBsonValueAndType(
				compareValue,
				&runData->indexBounds[compareIndex].upperBound.processedBoundValue,
				&isComparisonValid);
			if (!isComparisonValid)
			{
				return -1;
			}

			if (compareBounds == 0)
			{
				if (!runData->indexBounds[compareIndex].upperBound.isBoundInclusive)
				{
					/* for truncated you can't be sure - let the runtime re-evaluate */
					return runData->indexBounds[compareIndex].upperBound.
						   isProcessedValueTruncated ? 0 : -1;
				}
				else
				{
					continue;
				}
			}

			if (compareBounds > 0)
			{
				/* Can stop searching */
				return hasEqualityPrefix ? 1 : -1;
			}
		}
	}

	return 0;
}


/*
 * gin_bson_composite_path_consistent validates whether a given match on a key
 * can be used to satisfy a query. given an array of queryKeys and
 * an array of 'check' that indicates whether that queryKey matched
 * exactly for the check. it allows for the gin index to do a full
 * runtime check for partial matches (recheck) or to accept that the term was a
 * hit for the query.
 * For more details see documentation on the 'consistent' method in the GIN extensibility.
 */
Datum
gin_bson_composite_path_consistent(PG_FUNCTION_ARGS)
{
	bool *check = (bool *) PG_GETARG_POINTER(0);
	StrategyNumber strategy = PG_GETARG_UINT16(1);
	int32_t numKeys = (int32_t) PG_GETARG_INT32(3);

	Pointer *extra_data = (Pointer *) PG_GETARG_POINTER(4);
	bool *recheck = (bool *) PG_GETARG_POINTER(5);       /* out param. */
	/* Datum *queryKeys = (Datum *) PG_GETARG_POINTER(6); */

	if (strategy != BSON_INDEX_STRATEGY_COMPOSITE_QUERY)
	{
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("Composite index does not support strategy %d",
							   strategy)));
	}

	CompositeQueryRunData *runData = (CompositeQueryRunData *) extra_data[0];

	if (numKeys > 2)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
						errmsg("Too many keys in consistent check: %d", numKeys)));
	}

	*recheck = runData->requiresRuntimeRecheck;
	if (runData->hasTruncation)
	{
		*recheck = check[1];
	}

	return check[0];
}


/*
 * gin_bson_get_composite_path_generated_terms is an internal utility function that allows to retrieve
 * the set of terms that *would* be inserted in the index for a given document for a single
 * path index option specification.
 * The function gets a document, path, and if it's a wildcard, and sets up the index structures
 * to call 'generateTerms' and returns it as a SETOF records.
 *
 * gin_bson_get_composite_path_generated_terms(
 *      document bson,
 *      pathSpec text,
 *      termLength int)
 *
 */
Datum
gin_bson_get_composite_path_generated_terms(PG_FUNCTION_ARGS)
{
	FuncCallContext *functionContext;
	GenerateTermsContext *context;

	bool addMetadata = PG_GETARG_BOOL(3);
	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		pgbson *document = PG_GETARG_PGBSON(0);
		char *pathSpec = text_to_cstring(PG_GETARG_TEXT_P(1));
		int32_t truncationLimit = PG_GETARG_INT32(2);

		functionContext = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(functionContext->multi_call_memory_ctx);

		Size fieldSize = FillCompositePathSpec(pathSpec, NULL);
		BsonGinCompositePathOptions *options = palloc0(
			sizeof(BsonGinCompositePathOptions) + fieldSize);
		options->base.indexTermTruncateLimit = truncationLimit;
		options->base.type = IndexOptionsType_Composite;
		options->base.version = IndexOptionsVersion_V0;
		options->compositePathSpec = sizeof(BsonGinCompositePathOptions);

		FillCompositePathSpec(
			pathSpec,
			((char *) options) + sizeof(BsonGinCompositePathOptions));

		context = (GenerateTermsContext *) palloc0(sizeof(GenerateTermsContext));
		context->terms.entries = GenerateCompositeTermsCore(document, options,
															&context->totalTermCount);
		context->index = 0;
		MemoryContextSwitchTo(oldcontext);
		functionContext->user_fctx = (void *) context;
	}

	functionContext = SRF_PERCALL_SETUP();
	context = (GenerateTermsContext *) functionContext->user_fctx;

	if (context->index < context->totalTermCount)
	{
		Datum next = context->terms.entries[context->index++];
		BsonIndexTerm term = {
			false, false, { 0 }
		};
		bytea *serializedTerm = DatumGetByteaPP(next);
		InitializeBsonIndexTerm(serializedTerm, &term);

		/* By default we only print out the index term. If addMetadata is set, then we
		 * also append the bson metadata for the index term to the final output.
		 * This includes things like whether or not the term is truncated
		 */
		if (!addMetadata)
		{
			SRF_RETURN_NEXT(functionContext, PointerGetDatum(PgbsonElementToPgbson(
																 &term.element)));
		}
		else
		{
			pgbson_writer writer;
			PgbsonWriterInit(&writer);
			PgbsonWriterAppendValue(&writer, term.element.path, term.element.pathLength,
									&term.element.bsonValue);
			PgbsonWriterAppendBool(&writer, "t", 1, term.isIndexTermTruncated);
			SRF_RETURN_NEXT(functionContext, PointerGetDatum(PgbsonWriterGetPgbson(
																 &writer)));
		}
	}

	SRF_RETURN_DONE(functionContext);
}


/*
 * gin_bson_composite_path_options sets up the option specification for single field indexes
 * This initializes the structure that is used by the Index AM to process user specified
 * options on how to handle documents with the index.
 * For single field indexes we only need to track the path being indexed, and whether or not
 * it's a wildcard.
 * usage is as: using gin(document bson_gin_single_path_ops(path='a.b',iswildcard=true))
 * For more details see documentation on the 'options' method in the GIN extensibility.
 */
Datum
gin_bson_composite_path_options(PG_FUNCTION_ARGS)
{
	local_relopts *relopts = (local_relopts *) PG_GETARG_POINTER(0);

	init_local_reloptions(relopts, sizeof(BsonGinCompositePathOptions));

	/* add an option that has a default value of single path and accepts *one* value
	 *  This is used later to key off whether it's a single path or multi-key wildcard index options */
	add_local_int_reloption(relopts, "optionsType",
							"The type of the options struct.",
							IndexOptionsType_Composite,  /* default value */
							IndexOptionsType_Composite,  /* min */
							IndexOptionsType_Composite,  /* max */
							offsetof(BsonGinCompositePathOptions, base.type));
	add_local_string_reloption(relopts, "pathspec",
							   "Composite path array for the index",
							   NULL, &ValidateCompositePathSpec, &FillCompositePathSpec,
							   offsetof(BsonGinCompositePathOptions, compositePathSpec));
	add_local_int_reloption(relopts, "tl",
							"The index term size limit for truncation.",
							-1,  /* default value */
							-1,  /* min */
							INT32_MAX,  /* max */
							offsetof(BsonGinCompositePathOptions,
									 base.indexTermTruncateLimit));
	add_local_int_reloption(relopts, "v",
							"The version of the options struct.",
							IndexOptionsVersion_V0,          /* default value */
							IndexOptionsVersion_V0,          /* min */
							IndexOptionsVersion_V1,          /* max */
							offsetof(BsonGinCompositePathOptions, base.version));

	PG_RETURN_VOID();
}


IndexTraverseOption
GetCompositePathIndexTraverseOption(BsonIndexStrategy strategy, void *contextOptions,
									const char *currentPath,
									uint32_t currentPathLength,
									bson_type_t bsonType)
{
	if (!EnableNewCompositeIndexOpclass)
	{
		return IndexTraverse_Invalid;
	}

	BsonGinCompositePathOptions *options =
		(BsonGinCompositePathOptions *) contextOptions;
	uint32_t pathCount;
	const char *pathSpecBytes;
	Get_Index_Path_Option(options, compositePathSpec, pathSpecBytes, pathCount);

	for (uint32_t i = 0; i < pathCount; i++)
	{
		uint32_t indexPathLength = *(uint32_t *) pathSpecBytes;
		const char *indexPath = pathSpecBytes + sizeof(uint32_t);
		pathSpecBytes += indexPathLength + sizeof(uint32_t) + 1;

		if (currentPathLength == indexPathLength &&
			strncmp(currentPath, indexPath, indexPathLength) == 0)
		{
			return IndexTraverse_Match;
		}
	}

	return IndexTraverse_Invalid;
}


void
ModifyScanKeysForCompositeScan(ScanKey scankey, int nscankeys)
{
	pgbson_writer compositeWriter;
	PgbsonWriterInit(&compositeWriter);

	pgbson_array_writer queryWriter;
	PgbsonWriterStartArray(&compositeWriter, "q", 1, &queryWriter);

	for (int i = 0; i < nscankeys; i++)
	{
		Datum scanKeyArg = scankey[i].sk_argument;
		BsonIndexStrategy strategy = scankey[i].sk_strategy;
		pgbson *secondBson = DatumGetPgBson(scanKeyArg);

		pgbson_writer clauseWriter;
		PgbsonArrayWriterStartDocument(&queryWriter, &clauseWriter);
		PgbsonWriterAppendInt32(&clauseWriter, "op", 2,
								strategy);
		PgbsonWriterConcat(&clauseWriter, secondBson);
		PgbsonArrayWriterEndDocument(&queryWriter, &clauseWriter);
	}

	PgbsonWriterEndArray(&compositeWriter, &queryWriter);

	Datum finalDatum = PointerGetDatum(
		PgbsonWriterGetPgbson(&compositeWriter));

	/* Now update all the scan keys */
	for (int i = 0; i < nscankeys; i++)
	{
		scankey[i].sk_argument = finalDatum;
		scankey[i].sk_strategy = BSON_INDEX_STRATEGY_COMPOSITE_QUERY;
	}
}


/* --------------------------------------------------------- */
/* Private helper methods */
/* --------------------------------------------------------- */


/*
 * Callback that validates a user provided wildcard projection prefix
 * This is called on CREATE INDEX when a specific wildcard projection is provided.
 * We do minimal sanity validation here and instead use the Fill method to do final validation.
 */
static void
ValidateCompositePathSpec(const char *prefix)
{
	if (prefix == NULL)
	{
		/* validate can be called with the default value NULL. */
		return;
	}

	int32_t stringLength = strlen(prefix);
	if (stringLength < 3)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE), errmsg(
							"at least one filter path must be specified")));
	}
}


/*
 * Callback that updates the single path data into the serialized,
 * post-processed options structure - this is used later in term generation
 * through PG_GET_OPCLASS_OPTIONS().
 * This is called on CREATE INDEX to set up the serialized structure.
 * This function is called twice
 * - once with buffer being NULL (to get alloc size)
 * - once again with the buffer that should be serialized.
 * Here we parse the jsonified path options to build a serialized path
 * structure that is more efficiently parsed during term generation.
 */
static Size
FillCompositePathSpec(const char *prefix, void *buffer)
{
	if (prefix == NULL)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE), errmsg(
							"at least one filter path must be specified")));
	}

	pgbson *bson = PgbsonInitFromJson(prefix);
	uint32_t pathCount = 0;
	bson_iter_t bsonIterator;

	/* serialized length - start with the total term count. */
	uint32_t totalSize = sizeof(uint32_t);
	PgbsonInitIterator(bson, &bsonIterator);
	while (bson_iter_next(&bsonIterator))
	{
		if (!BSON_ITER_HOLDS_UTF8(&bsonIterator))
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE), errmsg(
								"filter must have a valid string path")));
		}

		uint32_t pathLength;
		bson_iter_utf8(&bsonIterator, &pathLength);
		if (pathLength == 0)
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE), errmsg(
								"filter must have a valid path")));
		}

		pathCount++;

		/* add the prefixed path length */
		totalSize += sizeof(uint32_t);

		/* add the path size */
		totalSize += pathLength;

		/* Add the null terminator */
		totalSize += 1;
	}

	if (buffer != NULL)
	{
		PgbsonInitIterator(bson, &bsonIterator);
		char *bufferPtr = (char *) buffer;
		*((uint32_t *) bufferPtr) = pathCount;
		bufferPtr += sizeof(uint32_t);

		while (bson_iter_next(&bsonIterator))
		{
			uint32_t pathLength;
			const char *path = bson_iter_utf8(&bsonIterator, &pathLength);

			/* add the prefixed path length */
			*((uint32_t *) bufferPtr) = pathLength;
			bufferPtr += sizeof(uint32_t);

			/* add the serialized string */
			memcpy(bufferPtr, path, pathLength);
			bufferPtr += pathLength;

			*bufferPtr = 0;
			bufferPtr++;
		}
	}

	return totalSize;
}


static Datum *
GenerateCompositeTermsCore(pgbson *bson, BsonGinCompositePathOptions *options,
						   int32_t *nentries)
{
	uint32_t pathCount;
	const char *pathSpecBytes;
	Get_Index_Path_Option(options, compositePathSpec, pathSpecBytes, pathCount);

	Datum **entries = palloc(sizeof(Datum *) * pathCount);
	int32_t *entryCounts = palloc0(sizeof(int32_t) * pathCount);

	uint32_t totalTermCount = 1;
	for (uint32_t i = 0; i < pathCount; i++)
	{
		uint32_t indexPathLength = *(uint32_t *) pathSpecBytes;
		const char *indexPath = pathSpecBytes + sizeof(uint32_t);
		pathSpecBytes += indexPathLength + sizeof(uint32_t) + 1;

		Size requiredSize = FillSinglePathSpec(indexPath, NULL);

		GenerateTermsContext context = { 0 };
		BsonGinSinglePathOptions *singlePathOptions = palloc(
			sizeof(BsonGinSinglePathOptions) + requiredSize + 1);
		singlePathOptions->base.type = IndexOptionsType_SinglePath;
		singlePathOptions->base.version = IndexOptionsVersion_V0;

		/* The truncation limit will be divided by the numPaths */
		singlePathOptions->base.indexTermTruncateLimit =
			GetSinglePathTruncationLimit(options->base.indexTermTruncateLimit, pathCount);
		singlePathOptions->isWildcard = false;
		singlePathOptions->generateNotFoundTerm = true;
		singlePathOptions->path = sizeof(BsonGinSinglePathOptions);

		FillSinglePathSpec(indexPath, ((char *) singlePathOptions) +
						   sizeof(BsonGinSinglePathOptions));

		context.options = (void *) singlePathOptions;
		context.traverseOptionsFunc = &GetSinglePathIndexTraverseOption;
		context.generateNotFoundTerm = true;
		context.termMetadata = GetIndexTermMetadata(singlePathOptions);

		bool addRootTerm = false;
		GenerateTerms(bson, &context, addRootTerm);

		entries[i] = context.terms.entries;
		entryCounts[i] = context.totalTermCount;

		/* We will have at least 1 term */
		totalTermCount = totalTermCount * context.totalTermCount;
		pfree(singlePathOptions);
	}

	/* Now that we have the per term counts, generate the overall terms */
	/* Add an additional one in case we need a truncated term */
	Datum *indexEntries = palloc0(sizeof(Datum) * (totalTermCount + 2));

	bool hasTruncation = false;
	bool hasUndefined = false;
	IndexTermCreateMetadata overallMetadata = GetIndexTermMetadata(options);
	for (uint32_t i = 0; i < totalTermCount; i++)
	{
		pgbson_writer singleWriter;
		PgbsonWriterInit(&singleWriter);
		pgbson_array_writer termWriter;
		PgbsonWriterStartArray(&singleWriter, "$", 1, &termWriter);

		int termIndex = i;
		for (uint32_t j = 0; j < pathCount; j++)
		{
			int32_t currentIndex = termIndex % entryCounts[j];
			termIndex = termIndex / entryCounts[j];
			Datum term = entries[j][currentIndex];

			BsonIndexTerm indexTerm;
			InitializeBsonIndexTerm(DatumGetByteaPP(term), &indexTerm);

			if (indexTerm.isIndexTermTruncated)
			{
				hasTruncation = true;
			}
			if (indexTerm.element.pathLength == 0 &&
				indexTerm.element.bsonValue.value_type == BSON_TYPE_NULL)
			{
				/* This is the "path does not exist" term */
				indexTerm.element.bsonValue.value_type = BSON_TYPE_UNDEFINED;
				hasUndefined = true;
			}

			PgbsonArrayWriterWriteValue(&termWriter, &indexTerm.element.bsonValue);
		}
		PgbsonWriterEndArray(&singleWriter, &termWriter);

		pgbsonelement element = { 0 };
		element.path = "$";
		element.pathLength = 1;
		element.bsonValue = PgbsonArrayWriterGetValue(&termWriter);
		BsonIndexTermSerialized serializedTerm = SerializeBsonIndexTerm(
			&element, &overallMetadata);
		if (serializedTerm.isIndexTermTruncated)
		{
			hasTruncation = true;
		}

		indexEntries[i] = PointerGetDatum(serializedTerm.indexTermVal);
	}

	if (hasTruncation)
	{
		indexEntries[totalTermCount] = GenerateRootTruncatedTerm(&overallMetadata);
		totalTermCount++;
	}

	if (hasUndefined)
	{
		indexEntries[totalTermCount] = GenerateRootNonExistsTerm(&overallMetadata);
		totalTermCount++;
	}

	*nentries = totalTermCount;
	return indexEntries;
}


static int32_t
GetIndexPathsFromOptions(BsonGinCompositePathOptions *options,
						 const char **indexPaths)
{
	uint32_t pathCount;
	const char *pathSpecBytes;
	Get_Index_Path_Option(options, compositePathSpec, pathSpecBytes, pathCount);

	for (uint32_t i = 0; i < pathCount; i++)
	{
		uint32_t indexPathLength = *(uint32_t *) pathSpecBytes;
		const char *indexPath = pathSpecBytes + sizeof(uint32_t);
		pathSpecBytes += indexPathLength + sizeof(uint32_t) + 1;

		indexPaths[i] = indexPath;
	}

	return (int32_t) pathCount;
}


static void
ParseBoundsForCompositeOperator(pgbsonelement *singleElement, const char **indexPaths,
								int32_t numPaths, CompositeIndexBounds *queryBounds)
{
	if (singleElement->bsonValue.value_type != BSON_TYPE_ARRAY)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_INTERNALERROR), errmsg(
							"extract query for composite expecting a single array value: not %s",
							BsonTypeName(singleElement->bsonValue.value_type))));
	}

	bson_iter_t arrayIter;
	BsonValueInitIterator(&singleElement->bsonValue, &arrayIter);
	while (bson_iter_next(&arrayIter))
	{
		const bson_value_t *value = bson_iter_value(&arrayIter);
		if (value->value_type != BSON_TYPE_DOCUMENT)
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_INTERNALERROR), errmsg(
								"extract query composite expecting a single document value: %s",
								BsonValueToJsonForLogging(&singleElement->bsonValue))));
		}

		bson_iter_t queryOpIter;

		BsonValueInitIterator(value, &queryOpIter);
		BsonIndexStrategy queryStrategy = BSON_INDEX_STRATEGY_INVALID;
		pgbsonelement queryElement = { 0 };
		while (bson_iter_next(&queryOpIter))
		{
			const char *key = bson_iter_key(&queryOpIter);
			if (strcmp(key, "op") == 0)
			{
				queryStrategy = (BsonIndexStrategy) bson_iter_int32(&queryOpIter);
			}
			else
			{
				queryElement.path = key;
				queryElement.pathLength = strlen(key);
				queryElement.bsonValue = *bson_iter_value(&queryOpIter);
			}
		}

		if (queryStrategy == BSON_INDEX_STRATEGY_INVALID ||
			queryElement.pathLength == 0 ||
			queryElement.bsonValue.value_type == BSON_TYPE_EOD)
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_INTERNALERROR), errmsg(
								"extract query composite expecting a valid operator and value: op=%d, value=%s",
								queryStrategy, BsonValueToJsonForLogging(value))));
		}

		ParseBoundsFromValue(indexPaths, numPaths, &queryElement, queryStrategy,
							 queryBounds);
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


static void
ParseBoundsFromValue(const char **indexPaths, int32_t numPaths,
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
