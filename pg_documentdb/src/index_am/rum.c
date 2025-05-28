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

#include "api_hooks.h"
#include "planner/mongo_query_operator.h"
#include "opclass/bson_gin_index_mgmt.h"
#include "index_am/documentdb_rum.h"
#include "metadata/metadata_cache.h"
#include "opclass/bson_gin_composite_scan.h"


extern bool ForceUseIndexIfAvailable;
extern bool EnableNewCompositeIndexOpclass;
extern bool EnableIndexOrderbyPushdown;

/* --------------------------------------------------------- */
/* Forward declaration */
/* --------------------------------------------------------- */

static bool loaded_rum_routine = false;
static IndexAmRoutine rum_index_routine = { 0 };

RumIndexArrayStateFuncs *IndexArrayStateFuncs = NULL;

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
} DocumentDBRumIndexState;

static bool IsIndexIsValidForQuery(IndexPath *path);
static bool MatchClauseWithIndexForFuncExpr(IndexPath *path, int32_t indexcol,
											Oid funcId, List *args);
static bool ValidateMatchForOrderbyQuals(IndexPath *path);

static bool IsTextIndexMatch(IndexPath *path);

static IndexMultiKeyStatus CheckIndexHasArrays(IndexScanDesc scan,
											   IndexAmRoutine *coreRoutine);

static IndexScanDesc extension_rumbeginscan(Relation rel, int nkeys, int norderbys);
static void extension_rumendscan(IndexScanDesc scan);
static void extension_rumrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
								ScanKey orderbys, int norderbys);
static int64 extension_amgetbitmap(IndexScanDesc scan,
								   TIDBitmap *tbm);
static bool extension_amgettuple(IndexScanDesc scan,
								 ScanDirection direction);

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
	IndexAmRoutine *indexRoutine = GetDocumentDBIndexAmRoutine(fcinfo);
	PG_RETURN_POINTER(indexRoutine);
}


void
RegisterIndexArrayStateFuncs(RumIndexArrayStateFuncs *funcs)
{
	if (IndexArrayStateFuncs != NULL)
	{
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

	return indexRoutine;
}


void
LoadRumRoutine(void)
{
	bool missingOk = false;
	void **ignoreLibFileHandle = NULL;
	Datum (*rumhandler) (FunctionCallInfo) =
		load_external_function("$libdir/rum", "rumhandler", !missingOk,
							   ignoreLibFileHandle);

	LOCAL_FCINFO(fcinfo, 0);

	InitFunctionCallInfoData(*fcinfo, NULL, 1, InvalidOid, NULL, NULL);
	Datum rumHandlerDatum = rumhandler(fcinfo);
	IndexAmRoutine *indexRoutine = (IndexAmRoutine *) DatumGetPointer(rumHandlerDatum);
	rum_index_routine = *indexRoutine;
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
		if (path->indexinfo->opfamily[clause->indexcol] ==
			BsonRumTextPathOperatorFamily())
		{
			return true;
		}
	}

	return false;
}


inline static bool
IsCompositeOpClass(Relation rel)
{
	return EnableNewCompositeIndexOpclass &&
		   IndexRelationGetNumberOfKeyAttributes(rel) == 1 &&
		   rel->rd_opfamily[0] == BsonRumCompositeIndexOperatorFamily();
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

		/* Initialize with 1 composite scan key */
		outerScanState->innerScan = coreRoutine->ambeginscan(rel, 1, norderbys);

		/* return the outer scan */
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
		if (outerScanState && outerScanState->innerScan)
		{
			coreRoutine->amendscan(outerScanState->innerScan);
			pfree(outerScanState);
		}
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
							 orderbys, norderbys, &rum_index_routine);
}


void
extension_rumrescan_core(IndexScanDesc scan, ScanKey scankey, int nscankeys,
						 ScanKey orderbys, int norderbys,
						 IndexAmRoutine *coreRoutine)
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

		/* TODO: We need to check if the index has arrays */
		if (outerScanState->multiKeyStatus == IndexMultiKeyStatus_Unknown)
		{
			outerScanState->multiKeyStatus = CheckIndexHasArrays(scan, coreRoutine);
		}

		ModifyScanKeysForCompositeScan(scankey, nscankeys,
									   &outerScanState->compositeKey,
									   outerScanState->multiKeyStatus ==
									   IndexMultiKeyStatus_HasArrays);

		if (EnableIndexOrderbyPushdown)
		{
			if (norderbys > 1)
			{
				ereport(ERROR, (errmsg("Cannot push down multi-order by yet")));
			}

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
				else
				{
					ereport(ERROR, (errmsg(
										"Cannot push down order by on path with arrays")));
				}
			}

			coreRoutine->amrescan(outerScanState->innerScan,
								  &outerScanState->compositeKey, 1,
								  orderbys,
								  norderbys);
		}
		else
		{
			coreRoutine->amrescan(outerScanState->innerScan,
								  &outerScanState->compositeKey, 1,
								  NULL,
								  0);
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
	scan->xs_orderbyvals = outerScanState->innerScan->xs_orderbyvals;
	scan->xs_orderbynulls = outerScanState->innerScan->xs_orderbynulls;
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

		if (outerScanState->indexArrayState == NULL)
		{
			/* No arrays, or we don't support dedup - just return the basics */
			return GetOneTupleCore(outerScanState, scan, direction, coreRoutine);
		}
		else
		{
			bool result = GetOneTupleCore(outerScanState, scan, direction, coreRoutine);
			while (result)
			{
				/* if we could add it to the bitmap, return */
				if (IndexArrayStateFuncs->addItem(outerScanState->indexArrayState,
												  &scan->xs_heaptid))
				{
					return true;
				}

				/* else, get the next tuple */
				result = GetOneTupleCore(outerScanState, scan, direction, coreRoutine);
			}

			return result;
		}
	}
	else
	{
		return coreRoutine->amgettuple(scan, direction);
	}
}


static IndexMultiKeyStatus
CheckIndexHasArrays(IndexScanDesc scan, IndexAmRoutine *coreRoutine)
{
	/* Start a nested query lookup */
	IndexScanDesc innerDesc = coreRoutine->ambeginscan(scan->indexRelation, 1, 0);

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
