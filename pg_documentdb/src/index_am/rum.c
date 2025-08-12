/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/index_am/rum.c
 *
 * Rum access method implementations for documentdb_api.
 * See also: https://www.postgresql.org/docs/current/gin-extensibility.html
 * See also: https://github.com/postgrespro/rum
 *
 *-------------------------------------------------------------------------
 */


#include <postgres.h>
#include <fmgr.h>
#include <utils/index_selfuncs.h>
#include <utils/selfuncs.h>
#include <utils/lsyscache.h>
#include <access/relscan.h>
#include <utils/rel.h>
#include "math.h"
#include <commands/explain.h>
#include <access/gin.h>

#include "api_hooks.h"
#include "planner/mongo_query_operator.h"
#include "opclass/bson_gin_index_mgmt.h"
#include "index_am/documentdb_rum.h"
#include "metadata/metadata_cache.h"
#include "opclass/bson_gin_composite_scan.h"
#include "index_am/index_am_utils.h"
#include "opclass/bson_gin_index_term.h"
#include "opclass/bson_gin_private.h"
#include "utils/documentdb_errors.h"

extern bool ForceUseIndexIfAvailable;
extern bool EnableNewCompositeIndexOpclass;
extern bool EnableIndexOrderbyPushdown;
extern bool EnableDescendingCompositeIndex;
extern bool EnableIndexOnlyScan;
extern const RumIndexArrayStateFuncs RoaringStateFuncs;

bool RumHasMultiKeyPaths = false;


/* --------------------------------------------------------- */
/* Forward declaration */
/* --------------------------------------------------------- */
extern BsonIndexAmEntry RumIndexAmEntry;
static bool loaded_rum_routine = false;
static IndexAmRoutine rum_index_routine = { 0 };

const RumIndexArrayStateFuncs *IndexArrayStateFuncs = &RoaringStateFuncs;

typedef enum IndexMultiKeyStatus
{
	IndexMultiKeyStatus_Unknown = 0,

	IndexMultiKeyStatus_HasArrays = 1,

	IndexMultiKeyStatus_HasNoArrays = 2
} IndexMultiKeyStatus;

typedef struct DocumentDBRumIndexState
{
	IndexScanDesc innerScan;

	ScanKeyData compositeKey;

	IndexMultiKeyStatus multiKeyStatus;

	void *indexArrayState;

	int32_t numDuplicates;

	ScanDirection scanDirection;
} DocumentDBRumIndexState;


const char *DocumentdbRumPath = "$libdir/pg_documentdb_extended_rum";
typedef const RumIndexArrayStateFuncs *(*GetIndexArrayStateFuncsFunc)(void);

extern Datum gin_bson_composite_path_extract_query(PG_FUNCTION_ARGS);

static bool IsIndexIsValidForQuery(IndexPath *path);
static bool MatchClauseWithIndexForFuncExpr(IndexPath *path, int32_t indexcol,
											Oid funcId, List *args);
static bool ValidateMatchForOrderbyQuals(IndexPath *path);

static bool IsTextIndexMatch(IndexPath *path);

static IndexMultiKeyStatus CheckIndexHasArrays(Relation indexRelation,
											   IndexAmRoutine *coreRoutine);

static IndexScanDesc extension_rumbeginscan(Relation rel, int nkeys, int norderbys);
static void extension_rumendscan(IndexScanDesc scan);
static void extension_rumrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
								ScanKey orderbys, int norderbys);
static int64 extension_amgetbitmap(IndexScanDesc scan,
								   TIDBitmap *tbm);
static bool extension_amgettuple(IndexScanDesc scan,
								 ScanDirection direction);
static IndexBuildResult * extension_rumbuild(Relation heapRelation,
											 Relation indexRelation,
											 struct IndexInfo *indexInfo);
static bool extension_ruminsert(Relation indexRelation,
								Datum *values,
								bool *isnull,
								ItemPointer heap_tid,
								Relation heapRelation,
								IndexUniqueCheck checkUnique,
								bool indexUnchanged,
								struct IndexInfo *indexInfo);

static bool RumScanOrderedFalse(IndexScanDesc scan);

static CanOrderInIndexScan rum_index_scan_ordered = RumScanOrderedFalse;

inline static void
EnsureRumLibLoaded(void)
{
	if (!loaded_rum_routine)
	{
		ereport(ERROR, (errmsg(
							"The rum library should be loaded as part of shared_preload_libraries - this is a bug")));
	}
}


/* --------------------------------------------------------- */
/* Top level exports */
/* --------------------------------------------------------- */
PG_FUNCTION_INFO_V1(extensionrumhandler);

/*
 * Register the access method for RUM as a custom index handler.
 * This allows us to create a 'custom' RUM index in the extension.
 * Today, this is temporary: This is needed until the RUM index supports
 * a custom configuration function proc for index operator classes.
 * By registering it here we maintain compatibility with existing GIN implementations.
 * Once we merge the RUM config changes into the mainline repo, this can be removed.
 */
Datum
extensionrumhandler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *indexRoutine = GetRumIndexHandler(fcinfo);
	PG_RETURN_POINTER(indexRoutine);
}


void
RegisterIndexArrayStateFuncs(const RumIndexArrayStateFuncs *funcs)
{
	if (IndexArrayStateFuncs != NULL && IndexArrayStateFuncs != &RoaringStateFuncs)
	{
		/* This should not happen, as we should only register once */
		ereport(ERROR, (errmsg("Index array state functions already registered")));
	}

	if (funcs == NULL)
	{
		ereport(ERROR, (errmsg("Index array state functions cannot be null")));
	}

	if (funcs->createState == NULL || funcs->addItem == NULL ||
		funcs->freeState == NULL)
	{
		ereport(ERROR, (errmsg("Index array state functions cannot be null")));
	}

	IndexArrayStateFuncs = funcs;
}


IndexAmRoutine *
GetRumIndexHandler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *indexRoutine = palloc0(sizeof(IndexAmRoutine));

	EnsureRumLibLoaded();
	*indexRoutine = rum_index_routine;

	/* add a new proc as a config prog. */
	/* Based on https://github.com/postgrespro/rum/blob/master/src/rumutil.c#L117 */
	/* AMsupport is the index of the largest support function. We point to the options proc */
	uint16 RUMNProcs = indexRoutine->amsupport;
	if (RUMNProcs < 11)
	{
		indexRoutine->amsupport = RUMNProcs + 1;

		/* register the user config proc number. */
		/* based on https://github.com/postgrespro/rum/blob/master/src/rum.h#L837 */
		/* RUMNprocs is the count, and the highest function supported */
		/* We set our config proc to be one above that */
		indexRoutine->amoptsprocnum = RUMNProcs + 1;
	}

	indexRoutine->ambeginscan = extension_rumbeginscan;
	indexRoutine->amrescan = extension_rumrescan;
	indexRoutine->amgetbitmap = extension_amgetbitmap;
	indexRoutine->amgettuple = extension_amgettuple;
	indexRoutine->amendscan = extension_rumendscan;
	indexRoutine->amcostestimate = extension_rumcostestimate;
	indexRoutine->ambuild = extension_rumbuild;
	indexRoutine->aminsert = extension_ruminsert;
	indexRoutine->amcanreturn = NULL;

	return indexRoutine;
}


void
LoadRumRoutine(void)
{
	bool missingOk = false;
	void **ignoreLibFileHandle = NULL;

	/* Load the rum handler function from the shared library
	 * Allow overrides via the documentdb_rum extension
	 */

	Datum (*rumhandler) (FunctionCallInfo);
	const char *rumLibPath;

	ereport(LOG, (errmsg("Loading RUM handler with DocumentDBRumLibraryLoadOption: %d",
						 DocumentDBRumLibraryLoadOption)));
	switch (DocumentDBRumLibraryLoadOption)
	{
		case RumLibraryLoadOption_RequireDocumentDBRum:
		{
			rumLibPath = DocumentdbRumPath;
			rumhandler = load_external_function(rumLibPath,
												"documentdb_rumhandler", !missingOk,
												ignoreLibFileHandle);
			ereport(LOG, (errmsg(
							  "Loaded documentdb_rumhandler successfully via pg_documentdb_extended_rum")));
			break;
		}

		case RumLibraryLoadOption_PreferDocumentDBRum:
		{
			rumLibPath = DocumentdbRumPath;
			rumhandler = load_external_function(rumLibPath,
												"documentdb_rumhandler", missingOk,
												ignoreLibFileHandle);

			if (rumhandler == NULL)
			{
				rumLibPath = "$libdir/rum";
				rumhandler = load_external_function(rumLibPath, "rumhandler",
													!missingOk,
													ignoreLibFileHandle);
				ereport(LOG,
						(errmsg(
							 "Loaded documentdb_rum handler successfully via rum as a fallback")));
			}
			else
			{
				ereport(LOG,
						(errmsg(
							 "Loaded documentdb_rumhandler successfully via pg_documentdb_extended_rum")));
			}

			break;
		}

		case RumLibraryLoadOption_None:
		{
			rumLibPath = "$libdir/rum";
			rumhandler = load_external_function(rumLibPath, "rumhandler", !missingOk,
												ignoreLibFileHandle);
			ereport(LOG, (errmsg("Loaded documentdb_rum handler successfully via rum")));
			break;
		}

		default:
		{
			ereport(ERROR, (errmsg("Unknown RUM library load option: %d",
								   DocumentDBRumLibraryLoadOption)));
		}
	}

	LOCAL_FCINFO(fcinfo, 0);

	InitFunctionCallInfoData(*fcinfo, NULL, 1, InvalidOid, NULL, NULL);
	Datum rumHandlerDatum = rumhandler(fcinfo);
	IndexAmRoutine *indexRoutine = (IndexAmRoutine *) DatumGetPointer(rumHandlerDatum);
	rum_index_routine = *indexRoutine;

	/* Load optional explain function */
	missingOk = true;
	TryExplainIndexFunc explain_index_func =
		load_external_function(rumLibPath,
							   "try_explain_rum_index", !missingOk,
							   ignoreLibFileHandle);

	if (explain_index_func != NULL)
	{
		RumIndexAmEntry.add_explain_output = explain_index_func;
	}

	CanOrderInIndexScan scanOrderedFunc =
		load_external_function(rumLibPath,
							   "can_rum_index_scan_ordered", !missingOk,
							   ignoreLibFileHandle);
	if (scanOrderedFunc != NULL)
	{
		rum_index_scan_ordered = scanOrderedFunc;
	}

	if (IndexArrayStateFuncs == NULL)
	{
		/* Try to see if the custom rum handler has support for multi-key indexes */
		GetIndexArrayStateFuncsFunc get_index_array_state_funcs =
			load_external_function(rumLibPath,
								   "get_rum_index_array_state_funcs", !missingOk,
								   ignoreLibFileHandle);

		if (get_index_array_state_funcs != NULL)
		{
			ereport(LOG, (errmsg(
							  "Loaded RUM index array state functions successfully via the rum library")));
			RegisterIndexArrayStateFuncs(get_index_array_state_funcs());
		}
		else
		{
			ereport(LOG, (errmsg(
							  "RUM index array state functions not found, skipping registration")));
		}
	}
	else
	{
		ereport(LOG, (errmsg(
						  "RUM index array state functions already registered, skipping registration")));
	}


	loaded_rum_routine = true;
	pfree(indexRoutine);
}


/*
 * Custom cost estimation function for RUM.
 * While Function support handles matching against specific indexes
 * and ensuring pushdowns happen properly (see dollar_support),
 * There is one case that is not yet handled.
 * If an index has a predicate (partial index), and the *only* clauses
 * in the query are ones that match the predicate, indxpath.create_index_paths
 * creates quals that exclude the predicate. Consequently we're left with no clauses.
 * Because RUM also sets amoptionalkey to true (the first key in the index is not required
 * to be specified), we will still continue to consider the index (per useful_predicate in
 * build_index_paths). In this case, we need to check that at least one predicate matches the
 * index for the index to be considered.
 */
void
extension_rumcostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
						  Cost *indexStartupCost, Cost *indexTotalCost,
						  Selectivity *indexSelectivity, double *indexCorrelation,
						  double *indexPages)
{
	if (!IsIndexIsValidForQuery(path))
	{
		/* This index is not a match for the given query paths */
		/* In this code path, we set the total cost to infinity */
		/* As the planner walks through all other plans, one will be less */
		/* than infinity (the SeqScan) which will be picked in the worst case */
		*indexStartupCost = 0;
		*indexTotalCost = INFINITY;
		*indexSelectivity = 0;
		return;
	}


	if (IsCompositeOpFamilyOid(path->indexinfo->relam,
							   path->indexinfo->opfamily[0]))
	{
		/* If this is a composite index, then we need to ensure that
		 * the first column of the index matches the query path.
		 * This is because using the composite index would require specifying
		 * the first column.
		 */
		if (!CompositePathHasFirstColumnSpecified(path))
		{
			*indexStartupCost = 0;
			*indexTotalCost = INFINITY;
			*indexSelectivity = 0;
			return;
		}
	}

	/* Index is valid - pick the cost estimate for rum (which currently is the gin cost estimate) */
	gincostestimate(root, path, loop_count, indexStartupCost, indexTotalCost,
					indexSelectivity, indexCorrelation, indexPages);

	/* Do a pass to check for text indexes (We force push down with cost == 0) */
	if (ForceUseIndexIfAvailable || IsTextIndexMatch(path))
	{
		*indexTotalCost = 0;
		*indexStartupCost = 0;
	}
}


/*
 * Currently orderby pushdown only works for RUM indexes if enabled.
 * However, orderby also requires that the index is
 * 1) Not a multi-key index
 * 2) Or the filters on order by are full range filters.
 *
 * Currently we ignore multi-key indexes altogether.
 * TODO: Support multi-key indexes with order by pushdown if the orderby
 * matches the [MinKey, MaxKey] range of the path with an equality prefix.
 */
bool
CompositeIndexSupportsOrderByPushdown(IndexPath *indexPath, List *sortDetails,
									  int32_t *maxPathKeySupported,
									  bool *isReverseOrder, bool isGroupBy)
{
	GetMultikeyStatusFunc getMultiKeyStatusFunc = GetMultiKeyStatusByRelAm(
		indexPath->indexinfo->relam);
	if (getMultiKeyStatusFunc == NULL)
	{
		return false;
	}

	if (!indexPath->indexinfo->amcanorderbyop)
	{
		/* No use if the index can't order by operator */
		return false;
	}

	bool indexSupportsOrderByDesc = GetIndexSupportsBackwardsScan(
		indexPath->indexinfo->relam);

	BsonGinIndexOptionsBase *options =
		(BsonGinIndexOptionsBase *) indexPath->indexinfo->opclassoptions[0];

	if (options->type != IndexOptionsType_Composite)
	{
		return false;
	}

	ListCell *sortCell;
	int32_t maxOrderbyColumn = -1;
	int32_t lastContiguousOrderbyColumn = -1;
	int32_t minOrderbyColumn = INT_MAX;
	int32_t orderByDetailIndex = 0;
	bool hasReverseOrder = false;
	bool hasForwardSortOrder = false;
	foreach(sortCell, sortDetails)
	{
		SortIndexInputDetails *sortDetailsInput = (SortIndexInputDetails *) lfirst(
			sortCell);

		int8_t sortDirection = 0;
		int32_t orderbyColumnNumber = GetCompositeOpClassColumnNumber(
			sortDetailsInput->sortPath,
			options, &sortDirection);
		if (orderbyColumnNumber < 0)
		{
			/* If the order by path does not match the index, we can't push down any further keys */
			break;
		}

		/* If the sort doesn't match the index, then break */
		int32_t sortBtreeStrategy = sortDirection < 0 ? BTGreaterStrategyNumber :
									BTLessStrategyNumber;

		bool currentPathKeyIsReverseSort = sortDetailsInput->sortPathKey->pk_strategy !=
										   sortBtreeStrategy;
		if (currentPathKeyIsReverseSort)
		{
			if (!indexSupportsOrderByDesc)
			{
				/* We can't continue pushdown any further */
				break;
			}

			if (hasForwardSortOrder)
			{
				/* Prior keys were in forward order - we can't match this key */
				break;
			}

			hasReverseOrder = true;
		}
		else
		{
			if (hasReverseOrder)
			{
				/* Prior keys were in reverse order - we can't match this key */
				break;
			}

			hasForwardSortOrder = true;
		}

		if (sortBtreeStrategy == BTGreaterStrategyNumber &&
			!EnableDescendingCompositeIndex)
		{
			break;
		}

		if (maxOrderbyColumn < 0)
		{
			minOrderbyColumn = orderbyColumnNumber;
			maxOrderbyColumn = orderbyColumnNumber;
		}
		else if (orderbyColumnNumber < maxOrderbyColumn + 1)
		{
			/* Can't sort by prior column again*/
			break;
		}
		else if (orderbyColumnNumber != maxOrderbyColumn + 1 &&
				 lastContiguousOrderbyColumn < 0)
		{
			/* order by does not match index ordering */
			lastContiguousOrderbyColumn = maxOrderbyColumn;
		}

		maxOrderbyColumn = orderbyColumnNumber;
		orderByDetailIndex = foreach_current_index(sortCell);
	}

	if (maxOrderbyColumn < 0)
	{
		/* No order by columns found, nothing to push down */
		return false;
	}

	if (hasReverseOrder && hasForwardSortOrder)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_INTERNALERROR),
						errmsg(
							"Unexpected - found push down order by with both forward and reverse order in the same index path")));
	}

	*isReverseOrder = hasReverseOrder;

	/* By default use min of lastContiguousOrderbyColumn & maxOrderbyColumn */
	*maxPathKeySupported = lastContiguousOrderbyColumn >= 0 ?
						   lastContiguousOrderbyColumn : orderByDetailIndex;
	bool isMultiKeyIndex = false;
	Relation indexRel = index_open(indexPath->indexinfo->indexoid, NoLock);
	isMultiKeyIndex = getMultiKeyStatusFunc(indexRel);
	index_close(indexRel, NoLock);

	if (!isMultiKeyIndex && maxOrderbyColumn == 0)
	{
		/* Non multi-key index on the first column always supports order by */
		return true;
	}

	if (isGroupBy && isMultiKeyIndex)
	{
		/* Cannot push group by to a multi-key index */
		return false;
	}

	bool equalityPrefixes[INDEX_MAX_KEYS] = { false };
	bool hasRangePredicate[INDEX_MAX_KEYS] = { false };

	bool isValid = GetEqualityRangePredicatesForIndexPath(indexPath, options,
														  equalityPrefixes,
														  hasRangePredicate);
	if (!isValid)
	{
		return false;
	}

	/* Now for an orderby walk the index paths and ensure we have sanity to push down
	 * We can only push orderby to the index if the preceding columns to the first orderby
	 * are all equality. For multi-key index, equality must go until the max order by clause.
	 */
	for (int i = 0; i < minOrderbyColumn; i++)
	{
		if (!equalityPrefixes[i])
		{
			return false;
		}
	}

	if (isMultiKeyIndex)
	{
		/* For multi-key we may have filters on the order by that restrict rows, but there may be rows
		 * that do not match the filter, but need to be considered for order by.
		 * Given 2 document such as
		 * "a": [ 3, 90, 50 ],  "a": [ 30, 51 ]
		 * a filter of { "a": { "$gt" 50 }} orderby "a": 1 will walk the index as:
		 *  "a": [ 30, 51 ], "a": [ 3, 90, 50 ]
		 * which is incorrect since 3 needs to be ordered first (even though it didn't match the filter).
		 * Consequently, only support orderby pushdown if the filter doesn't cover the orderby column.
		 */
		int32_t maxOrderByConsidered = lastContiguousOrderbyColumn >= 0 ?
									   lastContiguousOrderbyColumn : maxOrderbyColumn;
		for (int i = minOrderbyColumn; i <= maxOrderByConsidered; i++)
		{
			if (hasRangePredicate[i] || equalityPrefixes[i])
			{
				return false;
			}
		}
	}
	else if (lastContiguousOrderbyColumn >= 0)
	{
		/* We hit a case where we had a missing set of order by keys - we require that the remaining columns
		 * are all equality prefixes.
		 */
		for (int i = lastContiguousOrderbyColumn + 1; i <= maxOrderbyColumn; i++)
		{
			if (!equalityPrefixes[i])
			{
				break;
			}

			*maxPathKeySupported = i;
		}
	}

	/* Equality prefix with order by on the column is supported. */
	return true;
}


/* Check if the index supports index-only scans based on the index rel am. */
bool
CompositeIndexSupportsIndexOnlyScan(const IndexPath *indexPath)
{
	GetMultikeyStatusFunc getMultiKeyStatusFunc = NULL;
	GetTruncationStatusFunc getTruncationStatusFunc = NULL;

	bool supports = GetIndexAmSupportsIndexOnlyScan(indexPath->indexinfo->relam,
													indexPath->indexinfo->opfamily[0],
													&getMultiKeyStatusFunc,
													&getTruncationStatusFunc);

	if (!supports || getMultiKeyStatusFunc == NULL || getTruncationStatusFunc == NULL)
	{
		/* If the index does not support index only scan, return false */
		return false;
	}

	Relation indexRelation = index_open(indexPath->indexinfo->indexoid, NoLock);
	bool multiKeyStatus = getMultiKeyStatusFunc(indexRelation);
	bool hasTruncatedTerms = getTruncationStatusFunc(indexRelation);
	index_close(indexRelation, NoLock);

	/* can only support index only scan if the index is not multikey and there are no truncated terms. */
	return !multiKeyStatus && !hasTruncatedTerms;
}


static bool
RumScanOrderedFalse(IndexScanDesc scan)
{
	return false;
}


/*
 * Validates whether an index path descriptor
 * can be satisfied by the current index.
 */
static bool
IsIndexIsValidForQuery(IndexPath *path)
{
	if (IsA(path, IndexOnlyScan))
	{
		/* We don't support index only scans in RUM */
		return false;
	}

	if (path->indexorderbys != NIL &&
		!ValidateMatchForOrderbyQuals(path))
	{
		/* Only return valid cost if the order by present
		 * matches the index fully
		 */
		return false;
	}

	if (list_length(path->indexclauses) >= 1)
	{
		/* if there's at least one other index clause,
		 * then this index is already valid
		 */
		return true;
	}

	if (path->indexinfo->indpred == NIL)
	{
		/*
		 * if the index is not a partial index, the useful_predicate
		 * clause does not apply. If there's no filter clauses, we
		 * can't really use this index (don't wanna do a full index scan)
		 */
		return false;
	}

	if (path->indexinfo->indpred != NIL)
	{
		ListCell *cell;
		foreach(cell, path->indexinfo->indpred)
		{
			Node *predQual = (Node *) lfirst(cell);

			/* walk the index predicates and check if they match the index */
			/* TODO: Do we need a query walk here */
			if (IsA(predQual, OpExpr))
			{
				OpExpr *expr = (OpExpr *) predQual;
				for (int32_t indexCol = 0; indexCol < path->indexinfo->nkeycolumns;
					 indexCol++)
				{
					if (MatchClauseWithIndexForFuncExpr(path, indexCol, expr->opfuncid,
														expr->args))
					{
						return true;
					}
				}
			}
			else if (IsA(predQual, FuncExpr))
			{
				FuncExpr *expr = (FuncExpr *) predQual;
				for (int32_t indexCol = 0; indexCol < path->indexinfo->nkeycolumns;
					 indexCol++)
				{
					if (MatchClauseWithIndexForFuncExpr(path, indexCol, expr->funcid,
														expr->args))
					{
						return true;
					}
				}
			}
		}
	}

	return false;
}


/* Given an operator expression and an index column with an index
 * Validates whether that operator + column is supported in this index */
static bool
MatchClauseWithIndexForFuncExpr(IndexPath *path, int32_t indexcol, Oid funcId, List *args)
{
	Node *operand = (Node *) lsecond(args);

	/* not a const - can't evaluate this here */
	if (!IsA(operand, Const))
	{
		return true;
	}

	/* if no options - thunk to default cost estimation */
	bytea *options = path->indexinfo->opclassoptions[indexcol];
	if (options == NULL)
	{
		return true;
	}

	BsonIndexStrategy strategy = GetBsonStrategyForFuncId(funcId);
	if (strategy == BSON_INDEX_STRATEGY_INVALID)
	{
		return false;
	}

	Datum queryValue = ((Const *) operand)->constvalue;
	return ValidateIndexForQualifierValue(options, queryValue, strategy);
}


/*
 * ValidateMatchForOrderbyQuals walks the order by operator
 * clauses and ensures that every clause is valid for the
 * current index.
 */
static bool
ValidateMatchForOrderbyQuals(IndexPath *path)
{
	ListCell *orderbyCell;
	int index = 0;
	foreach(orderbyCell, path->indexorderbys)
	{
		Expr *orderQual = (Expr *) lfirst(orderbyCell);

		/* Order by on RUM only supports OpExpr clauses */
		if (!IsA(orderQual, OpExpr))
		{
			return false;
		}

		/* Validate that it's a supported operator */
		OpExpr *opQual = (OpExpr *) orderQual;
		if (opQual->opfuncid != BsonOrderByFunctionOid())
		{
			return false;
		}

		/* OpExpr for order by always has 2 args */
		Assert(list_length(opQual->args) == 2);
		Expr *secondArg = lsecond(opQual->args);
		if (!IsA(secondArg, Const))
		{
			return false;
		}

		Const *secondConst = (Const *) secondArg;
		int indexColInt = list_nth_int(path->indexorderbycols, index);
		bytea *options = path->indexinfo->opclassoptions[indexColInt];
		if (options == NULL)
		{
			return false;
		}

		/* Validate that the path can be pushed to the index. */
		if (!ValidateIndexForQualifierValue(options, secondConst->constvalue,
											BSON_INDEX_STRATEGY_DOLLAR_ORDERBY))
		{
			return false;
		}

		index++;
	}

	return true;
}


/*
 * Returns true if the IndexPath corresponds to a "text"
 * index. This is used to force the index cost to 0 to make sure
 * we use the text index.
 */
static bool
IsTextIndexMatch(IndexPath *path)
{
	ListCell *cell;
	foreach(cell, path->indexclauses)
	{
		IndexClause *clause = lfirst(cell);
		if (IsTextPathOpFamilyOid(
				path->indexinfo->relam,
				path->indexinfo->opfamily[clause->indexcol]))
		{
			return true;
		}
	}

	return false;
}


static IndexScanDesc
extension_rumbeginscan(Relation rel, int nkeys, int norderbys)
{
	EnsureRumLibLoaded();
	if (!EnableNewCompositeIndexOpclass)
	{
		return rum_index_routine.ambeginscan(rel, nkeys, norderbys);
	}

	return extension_rumbeginscan_core(rel, nkeys, norderbys,
									   &rum_index_routine);
}


IndexScanDesc
extension_rumbeginscan_core(Relation rel, int nkeys, int norderbys,
							IndexAmRoutine *coreRoutine)
{
	if (IsCompositeOpClass(rel))
	{
		IndexScanDesc scan = RelationGetIndexScan(rel, nkeys, norderbys);

		DocumentDBRumIndexState *outerScanState = palloc0(
			sizeof(DocumentDBRumIndexState));
		scan->opaque = outerScanState;
		outerScanState->scanDirection = ForwardScanDirection;

		/* Don't yet start inner scan here - instead wait until rescan to begin */
		return scan;
	}
	else
	{
		return coreRoutine->ambeginscan(rel, nkeys, norderbys);
	}
}


static void
extension_rumendscan(IndexScanDesc scan)
{
	EnsureRumLibLoaded();

	if (!EnableNewCompositeIndexOpclass)
	{
		rum_index_routine.amendscan(scan);
		return;
	}

	extension_rumendscan_core(scan, &rum_index_routine);
}


void
extension_rumendscan_core(IndexScanDesc scan, IndexAmRoutine *coreRoutine)
{
	if (IsCompositeOpClass(scan->indexRelation))
	{
		DocumentDBRumIndexState *outerScanState =
			(DocumentDBRumIndexState *) scan->opaque;
		if (outerScanState->innerScan)
		{
			coreRoutine->amendscan(outerScanState->innerScan);
		}

		pfree(outerScanState);
	}
	else
	{
		coreRoutine->amendscan(scan);
	}
}


static void
extension_rumrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
					ScanKey orderbys, int norderbys)
{
	EnsureRumLibLoaded();
	if (!EnableNewCompositeIndexOpclass)
	{
		rum_index_routine.amrescan(scan, scankey, nscankeys, orderbys, norderbys);
		return;
	}

	extension_rumrescan_core(scan, scankey, nscankeys,
							 orderbys, norderbys, &rum_index_routine,
							 RumGetMultikeyStatus, rum_index_scan_ordered);
}


void
extension_rumrescan_core(IndexScanDesc scan, ScanKey scankey, int nscankeys,
						 ScanKey orderbys, int norderbys,
						 IndexAmRoutine *coreRoutine,
						 GetMultikeyStatusFunc multiKeyStatusFunc,
						 CanOrderInIndexScan isIndexScanOrdered)
{
	if (IsCompositeOpClass(scan->indexRelation))
	{
		/* Copy the scan keys to our scan */
		if (scankey && scan->numberOfKeys > 0)
		{
			memmove(scan->keyData, scankey,
					scan->numberOfKeys * sizeof(ScanKeyData));
		}
		if (orderbys && scan->numberOfOrderBys > 0)
		{
			memmove(scan->orderByData, orderbys,
					scan->numberOfOrderBys * sizeof(ScanKeyData));
		}

		/* get the opaque scans */
		DocumentDBRumIndexState *outerScanState =
			(DocumentDBRumIndexState *) scan->opaque;

		if (outerScanState->multiKeyStatus == IndexMultiKeyStatus_Unknown)
		{
			outerScanState->multiKeyStatus = multiKeyStatusFunc(scan->indexRelation);
		}

		ScanKey innerOrderBy = NULL;
		int32_t nInnerorderbys = 0;
		if (EnableIndexOrderbyPushdown)
		{
			innerOrderBy = orderbys;
			nInnerorderbys = norderbys;

			outerScanState->scanDirection =
				DetermineCompositeScanDirection(
					scan->indexRelation->rd_opcoptions[0],
					orderbys, norderbys);
		}

		ScanKey innerScanKey = scankey;
		int32_t nInnerScanKeys = nscankeys;

		/* There are 2 paths here, regular queries, or unique order by
		 * If this is a unique order by, we need to modify the scan keys
		 * for both paths.
		 */
		if (ModifyScanKeysForCompositeScan(scankey, nscankeys,
										   &outerScanState->compositeKey,
										   outerScanState->multiKeyStatus ==
										   IndexMultiKeyStatus_HasArrays,
										   nInnerorderbys > 0,
										   outerScanState->scanDirection))
		{
			innerScanKey = &outerScanState->compositeKey;
			nInnerScanKeys = 1;
		}

		if (outerScanState->innerScan == NULL)
		{
			/* Initialize the inner scan if not initialized using the order by and keys */
			outerScanState->innerScan = coreRoutine->ambeginscan(scan->indexRelation,
																 nInnerScanKeys,
																 nInnerorderbys);

			outerScanState->innerScan->xs_want_itup = scan->xs_want_itup;
		}

		coreRoutine->amrescan(outerScanState->innerScan,
							  innerScanKey, nInnerScanKeys,
							  innerOrderBy,
							  nInnerorderbys);

		if (isIndexScanOrdered(outerScanState->innerScan) || nInnerorderbys > 0)
		{
			if (outerScanState->multiKeyStatus == IndexMultiKeyStatus_HasArrays)
			{
				if (IndexArrayStateFuncs != NULL)
				{
					if (outerScanState->indexArrayState != NULL)
					{
						/* free the state */
						IndexArrayStateFuncs->freeState(outerScanState->indexArrayState);
					}

					outerScanState->indexArrayState = IndexArrayStateFuncs->createState();
				}
				else if (nInnerorderbys > 0)
				{
					ereport(ERROR, (errmsg(
										"Cannot push down order by on path with arrays")));
				}
			}
		}
		else if (outerScanState->innerScan->xs_want_itup)
		{
			ereport(ERROR, (errmsg(
								"Cannot use index only scan on a non-ordered index scan")));
		}
	}
	else
	{
		coreRoutine->amrescan(scan, scankey, nscankeys, orderbys, norderbys);
	}
}


static int64
extension_amgetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	EnsureRumLibLoaded();
	if (!EnableNewCompositeIndexOpclass)
	{
		return rum_index_routine.amgetbitmap(scan, tbm);
	}

	return extension_rumgetbitmap_core(scan, tbm, &rum_index_routine);
}


int64
extension_rumgetbitmap_core(IndexScanDesc scan, TIDBitmap *tbm,
							IndexAmRoutine *coreRoutine)
{
	if (IsCompositeOpClass(scan->indexRelation))
	{
		DocumentDBRumIndexState *outerScanState =
			(DocumentDBRumIndexState *) scan->opaque;
		return coreRoutine->amgetbitmap(outerScanState->innerScan, tbm);
	}
	else
	{
		return coreRoutine->amgetbitmap(scan, tbm);
	}
}


static bool
extension_amgettuple(IndexScanDesc scan, ScanDirection direction)
{
	EnsureRumLibLoaded();
	if (!EnableNewCompositeIndexOpclass)
	{
		return rum_index_routine.amgettuple(scan, direction);
	}

	return extension_rumgettuple_core(scan, direction, &rum_index_routine);
}


static bool
GetOneTupleCore(DocumentDBRumIndexState *outerScanState,
				IndexScanDesc scan, ScanDirection direction,
				IndexAmRoutine *coreRoutine)
{
	bool result = coreRoutine->amgettuple(outerScanState->innerScan, direction);

	scan->xs_heaptid = outerScanState->innerScan->xs_heaptid;
	scan->xs_recheck = outerScanState->innerScan->xs_recheck;
	scan->xs_recheckorderby = outerScanState->innerScan->xs_recheckorderby;

	/* Set the pointers to handle order by values */
	scan->xs_orderbyvals = outerScanState->innerScan->xs_orderbyvals;
	scan->xs_orderbynulls = outerScanState->innerScan->xs_orderbynulls;

	scan->xs_itup = outerScanState->innerScan->xs_itup;
	scan->xs_itupdesc = outerScanState->innerScan->xs_itupdesc;

	return result;
}


bool
extension_rumgettuple_core(IndexScanDesc scan, ScanDirection direction,
						   IndexAmRoutine *coreRoutine)
{
	if (IsCompositeOpClass(scan->indexRelation))
	{
		DocumentDBRumIndexState *outerScanState =
			(DocumentDBRumIndexState *) scan->opaque;

		/* The caller will always pass ForwardScanDirection
		 * since PG always uses ForwardScanDirection in cases where we do
		 * amcanorderbyop. For the inner scan, we would need to pass the
		 * scanDirection as determined in amrescan from the index state.
		 */
		if (unlikely(direction != ForwardScanDirection))
		{
			ereport(ERROR, (errmsg("rumgettuple only supports forward scans")));
		}

		if (outerScanState->indexArrayState == NULL)
		{
			/* No arrays, or we don't support dedup - just return the basics */
			return GetOneTupleCore(outerScanState, scan, outerScanState->scanDirection,
								   coreRoutine);
		}
		else
		{
			bool result = GetOneTupleCore(outerScanState, scan,
										  outerScanState->scanDirection, coreRoutine);
			while (result)
			{
				/* if we could add it to the bitmap, return */
				if (IndexArrayStateFuncs->addItem(outerScanState->indexArrayState,
												  &scan->xs_heaptid))
				{
					return true;
				}
				else
				{
					outerScanState->numDuplicates++;
				}

				/* else, get the next tuple */
				result = GetOneTupleCore(outerScanState, scan,
										 outerScanState->scanDirection, coreRoutine);
			}

			return result;
		}
	}
	else
	{
		return coreRoutine->amgettuple(scan, direction);
	}
}


static IndexBuildResult *
extension_rumbuild(Relation heapRelation,
				   Relation indexRelation,
				   struct IndexInfo *indexInfo)
{
	EnsureRumLibLoaded();

	if (!EnableNewCompositeIndexOpclass)
	{
		return rum_index_routine.ambuild(heapRelation, indexRelation, indexInfo);
	}

	bool amCanBuildParallel = true;
	return extension_rumbuild_core(heapRelation, indexRelation,
								   indexInfo, &rum_index_routine,
								   RumUpdateMultiKeyStatus,
								   amCanBuildParallel);
}


IndexBuildResult *
extension_rumbuild_core(Relation heapRelation, Relation indexRelation,
						struct IndexInfo *indexInfo, IndexAmRoutine *coreRoutine,
						UpdateMultikeyStatusFunc updateMultikeyStatus,
						bool amCanBuildParallel)
{
	RumHasMultiKeyPaths = false;
	IndexBuildResult *result = coreRoutine->ambuild(heapRelation, indexRelation,
													indexInfo);

	/* Update statistics to track that we're a multi-key index:
	 * Note: We don't use HasMultiKeyPaths here as we want to handle the parallel build
	 * scenario where we may have multiple workers building the index.
	 */
	if (amCanBuildParallel && IsCompositeOpClass(indexRelation))
	{
		IndexMultiKeyStatus status = CheckIndexHasArrays(indexRelation, coreRoutine);
		if (status == IndexMultiKeyStatus_HasArrays)
		{
			bool isBuild = true;
			updateMultikeyStatus(isBuild, indexRelation);
		}
	}
	else if (RumHasMultiKeyPaths)
	{
		bool isBuild = true;
		updateMultikeyStatus(isBuild, indexRelation);
	}

	return result;
}


static bool
extension_ruminsert(Relation indexRelation,
					Datum *values,
					bool *isnull,
					ItemPointer heap_tid,
					Relation heapRelation,
					IndexUniqueCheck checkUnique,
					bool indexUnchanged,
					struct IndexInfo *indexInfo)
{
	EnsureRumLibLoaded();

	if (!EnableNewCompositeIndexOpclass)
	{
		return rum_index_routine.aminsert(indexRelation, values, isnull,
										  heap_tid, heapRelation, checkUnique,
										  indexUnchanged, indexInfo);
	}

	return extension_ruminsert_core(indexRelation, values, isnull,
									heap_tid, heapRelation, checkUnique,
									indexUnchanged, indexInfo,
									&rum_index_routine, RumUpdateMultiKeyStatus);
}


bool
extension_ruminsert_core(Relation indexRelation,
						 Datum *values,
						 bool *isnull,
						 ItemPointer heap_tid,
						 Relation heapRelation,
						 IndexUniqueCheck checkUnique,
						 bool indexUnchanged,
						 struct IndexInfo *indexInfo,
						 IndexAmRoutine *coreRoutine,
						 UpdateMultikeyStatusFunc updateMultikeyStatus)
{
	RumHasMultiKeyPaths = false;
	bool result = coreRoutine->aminsert(indexRelation, values, isnull,
										heap_tid, heapRelation, checkUnique,
										indexUnchanged, indexInfo);

	if (RumHasMultiKeyPaths)
	{
		bool isBuild = false;
		updateMultikeyStatus(isBuild, indexRelation);
	}

	return result;
}


static IndexMultiKeyStatus
CheckIndexHasArrays(Relation indexRelation, IndexAmRoutine *coreRoutine)
{
	/* Start a nested query lookup */
	IndexScanDesc innerDesc = coreRoutine->ambeginscan(indexRelation, 1, 0);

	ScanKeyData arrayKey = { 0 };
	arrayKey.sk_attno = 1;
	arrayKey.sk_collation = InvalidOid;
	arrayKey.sk_strategy = BSON_INDEX_STRATEGY_IS_MULTIKEY;
	arrayKey.sk_argument = PointerGetDatum(PgbsonInitEmpty());

	coreRoutine->amrescan(innerDesc, &arrayKey, 1, NULL, 0);
	bool hasArrays = coreRoutine->amgettuple(innerDesc, ForwardScanDirection);
	coreRoutine->amendscan(innerDesc);
	return hasArrays ? IndexMultiKeyStatus_HasArrays : IndexMultiKeyStatus_HasNoArrays;
}


bool
RumGetTruncationStatus(Relation indexRelation)
{
	EnsureRumLibLoaded();

	if (!IsCompositeOpClass(indexRelation))
	{
		return false;
	}

	/* Start a nested query lookup */
	IndexScanDesc innerDesc = rum_index_routine.ambeginscan(indexRelation, 1, 0);

	ScanKeyData truncatedKey = { 0 };
	truncatedKey.sk_attno = 1;
	truncatedKey.sk_collation = InvalidOid;
	truncatedKey.sk_strategy = BSON_INDEX_STRATEGY_HAS_TRUNCATED_TERMS;
	truncatedKey.sk_argument = PointerGetDatum(PgbsonInitEmpty());

	rum_index_routine.amrescan(innerDesc, &truncatedKey, 1, NULL, 0);
	bool hasTruncation = rum_index_routine.amgettuple(innerDesc, ForwardScanDirection);
	rum_index_routine.amendscan(innerDesc);
	return hasTruncation;
}


void
ExplainCompositeScan(IndexScanDesc scan, ExplainState *es)
{
	if (IsCompositeOpClass(scan->indexRelation))
	{
		DocumentDBRumIndexState *outerScanState =
			(DocumentDBRumIndexState *) scan->opaque;

		ExplainPropertyBool("isMultiKey",
							outerScanState->multiKeyStatus ==
							IndexMultiKeyStatus_HasArrays, es);

		/* From the composite keys, get the lower bounds of the scans */
		/* Call extract_query to get the index details */
		uint32_t nentries = 0;
		bool *partialMatch = NULL;
		Pointer *extraData = NULL;
		int32_t ginScanType = scan->numberOfOrderBys > 0 ? GIN_SEARCH_MODE_ALL :
							  GIN_SEARCH_MODE_DEFAULT;

		if (outerScanState->compositeKey.sk_argument != (Datum) 0)
		{
			LOCAL_FCINFO(fcinfo, 7);
			fcinfo->flinfo = palloc(sizeof(FmgrInfo));
			fmgr_info_copy(fcinfo->flinfo,
						   index_getprocinfo(scan->indexRelation, 1,
											 GIN_EXTRACTQUERY_PROC),
						   CurrentMemoryContext);

			fcinfo->args[0].value = outerScanState->compositeKey.sk_argument;
			fcinfo->args[1].value = PointerGetDatum(&nentries);
			fcinfo->args[2].value = Int16GetDatum(BSON_INDEX_STRATEGY_COMPOSITE_QUERY);
			fcinfo->args[3].value = PointerGetDatum(&partialMatch);
			fcinfo->args[4].value = PointerGetDatum(&extraData);
			fcinfo->args[6].value = PointerGetDatum(&ginScanType);

			Datum *entryRes = (Datum *) gin_bson_composite_path_extract_query(fcinfo);

			/* Now write out the result for explain */
			List *boundsList = NIL;
			for (uint32_t i = 0; i < nentries; i++)
			{
				bytea *entry = DatumGetByteaPP(entryRes[i]);

				char *serializedBound = SerializeBoundsStringForExplain(entry,
																		extraData[i],
																		fcinfo);
				boundsList = lappend(boundsList, serializedBound);
			}

			ExplainPropertyList("indexBounds", boundsList, es);
		}

		if (outerScanState->numDuplicates > 0)
		{
			/* If we have duplicates, explain the number of duplicates */
			ExplainPropertyInteger("numDuplicates", "entries",
								   outerScanState->numDuplicates, es);
		}

		if (ScanDirectionIsBackward(outerScanState->scanDirection))
		{
			ExplainPropertyBool("isBackwardScan", true, es);
		}

		/* Explain the inner scan using underlying am */
		TryExplainByIndexAm(outerScanState->innerScan, es);
	}
}


void
ExplainRegularIndexScan(IndexScanDesc scan, struct ExplainState *es)
{
	if (IsBsonRegularIndexAm(scan->indexRelation->rd_rel->relam))
	{
		/* See if there's a hook to explain more in this index */
		TryExplainByIndexAm(scan, es);
	}
}
