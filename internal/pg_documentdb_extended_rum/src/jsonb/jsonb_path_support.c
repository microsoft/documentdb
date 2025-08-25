/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/jsonb/jsonb_path_support.c
 *
 * Planner support function for jsonbpath for documentdb_rum
 *-------------------------------------------------------------------------
 */


#include <postgres.h>
#include <miscadmin.h>
#include <fmgr.h>
#include <nodes/pathnodes.h>
#include <catalog/pg_am.h>
#include <utils/syscache.h>
#include <parser/parse_func.h>
#include <nodes/supportnodes.h>
#include <nodes/makefuncs.h>
#include <utils/jsonpath.h>
#include <commands/defrem.h>
#include "pg_documentdb_rum.h"
#include "jsonb_path_opclass.h"


PG_FUNCTION_INFO_V1(jsonb_path_support);
PG_FUNCTION_INFO_V1(jsonb_path_match_docdb_rum);

extern Datum jsonb_path_match_opr(PG_FUNCTION_ARGS);

static Oid JsonbPathMatchFuncOid(void);
static Expr * HandleSupportJsonPathMatch(SupportRequestIndexCondition *indexCond);

typedef struct PlannerOidCache
{
	Oid jsonbPathMatchFuncOid;
	Oid jsonPathMatchIndexOperatorOid;
	Oid documentDBExtendedRumAmOid;
	Oid documentDBJsonPathOpFamily;
} PlannerOidCache;

static PlannerOidCache OidCache = { 0 };


PGDLLEXPORT Datum
jsonb_path_support(PG_FUNCTION_ARGS)
{
	Node *supportRequest = (Node *) PG_GETARG_POINTER(0);
	Pointer responsePointer = NULL;
	if (IsA(supportRequest, SupportRequestIndexCondition))
	{
		SupportRequestIndexCondition *indexSupport =
			(SupportRequestIndexCondition *) supportRequest;
		if (indexSupport->funcid == JsonbPathMatchFuncOid())
		{
			responsePointer = (Pointer) HandleSupportJsonPathMatch(indexSupport);
		}
	}

	PG_RETURN_POINTER(responsePointer);
}


PGDLLEXPORT Datum
jsonb_path_match_docdb_rum(PG_FUNCTION_ARGS)
{
	/* Pass through to the postgres C function */
	return jsonb_path_match_opr(fcinfo);
}


static Oid
JsonbPathMatchFuncOid(void)
{
	if (OidCache.jsonbPathMatchFuncOid == InvalidOid)
	{
		List *functionNameList = list_make2(makeString(
												"documentdb_extended_rum_jsonb_ops"),
											makeString("jsonb_path_match_opr"));
		Oid paramOids[2] = { JSONBOID, JSONPATHOID };
		bool missingOK = false;
		OidCache.jsonbPathMatchFuncOid =
			LookupFuncName(functionNameList, 2, paramOids, missingOK);
	}

	return OidCache.jsonbPathMatchFuncOid;
}


static Oid
JsonbPathMatchIndexOperatorOid(void)
{
	if (OidCache.jsonPathMatchIndexOperatorOid == InvalidOid)
	{
		List *operatorNameList = list_make2(makeString(
												"documentdb_extended_rum_jsonb_ops_internal"),
											makeString("#@@"));

		OidCache.jsonPathMatchIndexOperatorOid =
			OpernameGetOprid(operatorNameList, JSONBOID, JSONPATHOID);
	}

	return OidCache.jsonPathMatchIndexOperatorOid;
}


static Oid
DocumentDBExtendedRumAmOid(void)
{
	if (OidCache.documentDBExtendedRumAmOid == InvalidOid)
	{
		HeapTuple tuple = SearchSysCache1(AMNAME, CStringGetDatum(
											  "documentdb_extended_rum"));
		if (!HeapTupleIsValid(tuple))
		{
			ereport(ERROR,
					(errmsg("Access method \"documentdb_extended_rum\" not supported.")));
		}
		Form_pg_am accessMethodForm = (Form_pg_am) GETSTRUCT(tuple);
		OidCache.documentDBExtendedRumAmOid = accessMethodForm->oid;
		ReleaseSysCache(tuple);
	}

	return OidCache.documentDBExtendedRumAmOid;
}


static Oid
DocumentDBJsonPathOpFamily(void)
{
	if (OidCache.documentDBJsonPathOpFamily == InvalidOid)
	{
		bool missingOk = false;
		OidCache.documentDBJsonPathOpFamily = get_opfamily_oid(
			DocumentDBExtendedRumAmOid(), list_make2(makeString(
														 "documentdb_extended_rum_jsonb_ops"),
													 makeString(
														 "jsonb_path_extended_rum_ops")),
			missingOk);
	}

	return OidCache.documentDBJsonPathOpFamily;
}


static bool
IndexSupportsPath(JsonPathItem *jpi, const char *indexPath, uint32_t indexPathLength)
{
	if (jpi->type == jpiRoot)
	{
		if (indexPathLength < 1 || indexPath[0] != '$')
		{
			return false;
		}

		JsonPathItem next;
		if (!jspGetNext(jpi, &next))
		{
			return indexPathLength == 1;
		}

		return IndexSupportsPath(&next, indexPath + 2, indexPathLength - 2);
	}
	else if (jpi->type == jpiKey)
	{
		int32_t keyLength = 0;
		const char *key = jspGetString(jpi, &keyLength);

		if (keyLength > indexPathLength)
		{
			return false;
		}

		if (strncmp(key, indexPath, keyLength) != 0)
		{
			return false;
		}

		JsonPathItem next;
		if (!jspGetNext(jpi, &next))
		{
			return indexPathLength == keyLength;
		}

		if (indexPathLength < keyLength + 2)
		{
			return false;
		}

		return IndexSupportsPath(&next, indexPath + 2, indexPathLength - 2);
	}

	return false;
}


static Expr *
HandleSupportJsonPathMatch(SupportRequestIndexCondition *indexCond)
{
	if (indexCond->index->relam != DocumentDBExtendedRumAmOid())
	{
		return NULL;
	}

	if (indexCond->index->opfamily[indexCond->indexcol] != DocumentDBJsonPathOpFamily() ||
		indexCond->index->opclassoptions[indexCond->indexcol] == NULL)
	{
		return NULL;
	}

	bytea *opclassOptions =
		(bytea *) indexCond->index->opclassoptions[indexCond->indexcol];

	uint32_t searchPathLength = 0;
	const char *searchPath = GetIndexPathFromOptions(opclassOptions, &searchPathLength);

	List *exprArgs = NIL;
	if (IsA(indexCond->node, FuncExpr))
	{
		FuncExpr *function = (FuncExpr *) indexCond->node;
		exprArgs = function->args;
	}
	else if (IsA(indexCond->node, OpExpr))
	{
		OpExpr *opExpr = (OpExpr *) indexCond->node;
		exprArgs = opExpr->args;
	}

	if (list_length(exprArgs) != 2)
	{
		return NULL;
	}

	Node *secondArg = lsecond(exprArgs);
	if (!IsA(secondArg, Const))
	{
		return NULL;
	}

	Const *secondConst = (Const *) secondArg;
	if (secondConst->constisnull)
	{
		return NULL;
	}

	JsonPath *path = DatumGetJsonPathP(secondConst->constvalue);
	JsonPathItem jpi;
	jspInit(&jpi, path);

	if (jpi.type == jpiEqual ||
		(jpi.type >= jpiLess && jpi.type <= jpiGreaterOrEqual))
	{
		if (jspHasNext(&jpi))
		{
			return NULL;
		}

		JsonPathItem leftArg, rightArg;
		jspGetLeftArg(&jpi, &leftArg);
		jspGetRightArg(&jpi, &rightArg);

		/* Valid support - others are not */
		if (rightArg.type > jpiBool)
		{
			return NULL;
		}

		if (!IndexSupportsPath(&leftArg, searchPath, searchPathLength))
		{
			return NULL;
		}

		/* Valid index expr */
		OpExpr *opExpr = (OpExpr *) make_opclause(JsonbPathMatchIndexOperatorOid(),
												  BOOLOID,
												  false,
												  linitial(exprArgs),
												  lsecond(exprArgs),
												  InvalidOid,
												  InvalidOid);
		opExpr->opfuncid = JsonbPathMatchFuncOid();
		return (Expr *) list_make1(opExpr);
	}

	return NULL;
}
