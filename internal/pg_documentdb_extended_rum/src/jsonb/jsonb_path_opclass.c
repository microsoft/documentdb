/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/jsonb/jsonb_path_opclass.c
 *
 * Planner support function for jsonbpath for documentdb_rum
 *-------------------------------------------------------------------------
 */


#include <postgres.h>
#include <miscadmin.h>
#include "utils/builtins.h"
#include <fmgr.h>
#include <utils/jsonb.h>
#include <utils/varlena.h>
#include <utils/numeric.h>
#include <catalog/pg_collation.h>
#include <utils/jsonpath.h>
#include <access/reloptions.h>
#include "pg_documentdb_rum.h"
#include "jsonb_path_opclass.h"

#define Get_Index_Path_Option(options, field, result, resultFieldLength) \
	const char *pathDefinition = GET_STRING_RELOPTION(options, field); \
	if (pathDefinition == NULL) { resultFieldLength = 0; result = NULL; } \
	else { resultFieldLength = *(uint32_t *) pathDefinition; result = pathDefinition + \
																	  sizeof(uint32_t); }

typedef struct JsonbExtendedRumOptions
{
	int32 vl_len_;            /* varlena header (do not touch directly!) */
	bool wildcard;
	int pathSpec;
} JsonbExtendedRumOptions;

typedef struct JsonbExtendedQueryData
{
	JsonPathItemType operator;
	JsonbValue rightArgValue;
} JsonbExtendedQueryData;

PG_FUNCTION_INFO_V1(jsonb_rum_ops_extract_value);
PG_FUNCTION_INFO_V1(jsonb_rum_ops_extract_query);
PG_FUNCTION_INFO_V1(jsonb_rum_ops_consistent);
PG_FUNCTION_INFO_V1(jsonb_rum_ops_compare_partial);
PG_FUNCTION_INFO_V1(jsonb_rum_ops_options);


static Datum * GenerateJsonbTermsCore(Jsonb *inputDoc, JsonbExtendedRumOptions *options,
									  int32_t *nentries);


PGDLLEXPORT Datum
jsonb_rum_ops_extract_value(PG_FUNCTION_ARGS)
{
	Jsonb *inputDoc = PG_GETARG_JSONB_P(0);
	int32_t *nentries = (int32_t *) PG_GETARG_POINTER(1);
	if (!PG_HAS_OPCLASS_OPTIONS())
	{
		ereport(ERROR, (errmsg("Index does not have options")));
	}

	JsonbExtendedRumOptions *options =
		(JsonbExtendedRumOptions *) PG_GET_OPCLASS_OPTIONS();

	Datum *indexEntries = GenerateJsonbTermsCore(inputDoc, options, nentries);
	PG_RETURN_POINTER(indexEntries);
}


static void
JsonPathItemToBound(JsonPathItem *item, JsonbValue *bound)
{
	switch (item->type)
	{
		case jpiString:
		{
			bound->type = jbvString;
			bound->val.string.val = jspGetString(item, &bound->val.string.len);
			return;
		}

		case jpiNumeric:
		{
			bound->type = jbvNumeric;
			bound->val.numeric = jspGetNumeric(item);
			return;
		}

		case jpiBool:
		{
			bound->type = jbvBool;
			bound->val.boolean = jspGetBool(item);
			return;
		}

		default:
		{
			ereport(ERROR, (errmsg("Unsupported jsonpath type for index bound %d",
								   item->type)));
		}
	}
}


PGDLLEXPORT Datum
jsonb_rum_ops_extract_query(PG_FUNCTION_ARGS)
{
	/* StrategyNumber strategy = PG_GETARG_UINT16(2); */
	JsonPath *query = PG_GETARG_JSONPATH_P(0);
	int32 *nentries = (int32 *) PG_GETARG_POINTER(1);
	bool **partialmatch = (bool **) PG_GETARG_POINTER(3);
	Pointer **extra_data = (Pointer **) PG_GETARG_POINTER(4);

	/* int32_t *searchMode = (int32_t *) PG_GETARG_POINTER(6); */

	if (!PG_HAS_OPCLASS_OPTIONS())
	{
		ereport(ERROR, (errmsg("Index does not have options")));
	}

	JsonbExtendedRumOptions *options =
		(JsonbExtendedRumOptions *) PG_GET_OPCLASS_OPTIONS();

	JsonPathItem jpi;
	jspInit(&jpi, query);

	*nentries = 1;
	*partialmatch = palloc0(sizeof(bool) * 1);
	*extra_data = palloc0(sizeof(Pointer) * 1);

	Datum *result = palloc0(sizeof(Datum) * 1);

	JsonPathItem rightArg;
	jspGetRightArg(&jpi, &rightArg);

	JsonbExtendedQueryData *queryData = palloc0(sizeof(JsonbExtendedQueryData));
	(*extra_data)[0] = (Pointer) queryData;

	queryData->operator = jpi.type;

	JsonbValue bound;
	switch (jpi.type)
	{
		case jpiEqual:
		{
			JsonPathItemToBound(&rightArg, &bound);
			(*partialmatch)[0] = false;
			queryData->rightArgValue = bound;
			break;
		}

		case jpiGreater:
		case jpiGreaterOrEqual:
		{
			JsonPathItemToBound(&rightArg, &bound);
			(*partialmatch)[0] = true;
			queryData->rightArgValue = bound;
			break;
		}

		case jpiLess:
		case jpiLessOrEqual:
		{
			/* Start from null */
			bound.type = jbvNull;
			(*partialmatch)[0] = true;
			JsonPathItemToBound(&rightArg, &queryData->rightArgValue);
			break;
		}

		default:
		{
			ereport(ERROR, (errmsg("Unsupported query operator %d", jpi.type)));
		}
	}

	if (options->wildcard)
	{
		/* We need a path prefix */
		if (!IsAJsonbScalar(&bound))
		{
			ereport(ERROR, (errmsg("Wildcard not yet supported")));
		}

		JsonbValue container = bound;
		container.type = jbvObject;
		container.val.object.nPairs = 1;
		container.val.object.pairs = palloc(sizeof(JsonbPair) * 1);
		container.val.object.pairs[0].key.type = jbvString;
		container.val.object.pairs[0].key.val.string.len = 1;
		container.val.object.pairs[0].key.val.string.val = "$";
		container.val.object.pairs[0].value = bound;

		result[0] = PointerGetDatum(JsonbValueToJsonb(&container));
		*nentries = 1;
	}
	else
	{
		result[0] = PointerGetDatum(JsonbValueToJsonb(&bound));
		*nentries = 1;
	}

	PG_RETURN_POINTER(result);
}


PGDLLEXPORT Datum
jsonb_rum_ops_consistent(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(true);
}


static int32_t
compareJsonbValues(JsonbValue *left, JsonbValue *right)
{
	if (left->type != right->type)
	{
		return left->type - right->type;
	}

	switch (left->type)
	{
		case jbvNull:
		{
			return 0;
		}

		case jbvString:
		{
			return varstr_cmp(left->val.string.val,
							  left->val.string.len,
							  right->val.string.val,
							  right->val.string.len,
							  DEFAULT_COLLATION_OID);
		}

		case jbvNumeric:
		{
			return DatumGetInt32(DirectFunctionCall2(numeric_cmp,
													 PointerGetDatum(left->val.numeric),
													 PointerGetDatum(
														 right->val.numeric)));
		}

		case jbvBool:
		{
			if (left->val.boolean == right->val.boolean)
			{
				return 0;
			}
			else if (left->val.boolean > right->val.boolean)
			{
				return 1;
			}
			else
			{
				return -1;
			}
		}

		default:
		{
			elog(ERROR, "unsupported type for comparison");
		}
	}
	return 0;
}


PGDLLEXPORT Datum
jsonb_rum_ops_compare_partial(PG_FUNCTION_ARGS)
{
	Jsonb *compareValue = PG_GETARG_JSONB_P(1);

	/* StrategyNumber strategy = PG_GETARG_UINT16(2); */
	Pointer extraData = PG_GETARG_POINTER(3);

	JsonbExtendedQueryData *runData = (JsonbExtendedQueryData *) extraData;
	JsonbValue *indexValue = getIthJsonbValueFromContainer(&compareValue->root, 0);
	switch (runData->operator)
	{
		case jpiGreater:
		{
			int32_t cmp = compareJsonbValues(indexValue, &runData->rightArgValue);
			return cmp > 0 ? 0 : -1;
		}

		case jpiGreaterOrEqual:
		{
			int32_t cmp = compareJsonbValues(indexValue, &runData->rightArgValue);
			return cmp >= 0 ? 0 : -1;
		}

		case jpiLess:
		{
			int32_t cmp = compareJsonbValues(indexValue, &runData->rightArgValue);
			return cmp < 0 ? 0 : 1;
		}

		case jpiLessOrEqual:
		{
			int32_t cmp = compareJsonbValues(indexValue, &runData->rightArgValue);
			return cmp <= 0 ? 0 : 1;
		}

		default:
		{
			ereport(ERROR, (errmsg("Unsupported query operator %d", runData->operator)));
		}
	}
}


static void
ValidatePathSpec(const char *prefix)
{
	int i;
	if (prefix == NULL)
	{
		/* validate can be called with the default value NULL. */
		return;
	}

	int32_t stringLength = strlen(prefix);
	if (stringLength < 1)
	{
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg(
							"at least one filter path must be specified")));
	}

	if (prefix[0] != '$')
	{
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg(
							"filter path must start with '$'")));
	}

	if ((stringLength > 1 && prefix[1] != '.') || stringLength == 2)
	{
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg(
							"filter path must be exactly '$' or start with '$.'")));
	}

	for (i = 1; i < stringLength; i++)
	{
		if (!isalnum(prefix[i]) && prefix[i] != '.')
		{
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg(
								"only simple jsonbpath filters supported currently")));
		}
	}
}


static Size
FillPathSpec(const char *prefix, void *buffer)
{
	if (prefix == NULL)
	{
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg(
							"at least one filter path must be specified")));
	}

	uint32_t totalSize = strlen(prefix) + 5;

	if (buffer != NULL)
	{
		char *bufferPtr = (char *) buffer;
		*((uint32_t *) bufferPtr) = strlen(prefix);
		bufferPtr += sizeof(uint32_t);

		strcpy(bufferPtr, prefix);
	}

	return totalSize;
}


PGDLLEXPORT Datum
jsonb_rum_ops_options(PG_FUNCTION_ARGS)
{
	local_relopts *relopts = (local_relopts *) PG_GETARG_POINTER(0);

	init_local_reloptions(relopts, sizeof(JsonbExtendedRumOptions));

	add_local_string_reloption(relopts, "pathspec",
							   "path to index",
							   NULL, &ValidatePathSpec, &FillPathSpec,
							   offsetof(JsonbExtendedRumOptions, pathSpec));
	add_local_bool_reloption(relopts, "wildcard",
							 "whether or not the path is a wildcard (recursive) index",
							 false, offsetof(JsonbExtendedRumOptions, wildcard));

	PG_RETURN_VOID();
}


const char *
GetIndexPathFromOptions(bytea *options, uint32_t *searchPathLength)
{
	JsonbExtendedRumOptions *rumOptions = (JsonbExtendedRumOptions *) options;
	const char *path;
	Get_Index_Path_Option(rumOptions, pathSpec, path, *searchPathLength);
	return path;
}


static bool
FindPathInJsonbValue(JsonbValue *inputValue, const char *path, uint32_t pathLength)
{
	check_stack_depth();
	CHECK_FOR_INTERRUPTS();
	if (inputValue->type != jbvObject && inputValue->type != jbvBinary)
	{
		return false;
	}

	char *firstDot = strchr(path, '.');

	JsonbValue searchKey = { 0 };
	searchKey.type = jbvString;
	searchKey.val.string.len = pathLength;
	searchKey.val.string.val = (char *) path;
	if (firstDot != NULL)
	{
		searchKey.val.string.len = firstDot - path;
	}

	bool found = false;
	if (inputValue->type == jbvBinary)
	{
		JsonbValue *foundValue = findJsonbValueFromContainer(inputValue->val.binary.data,
															 JB_FOBJECT, &searchKey);
		if (foundValue != NULL)
		{
			found = true;
			*inputValue = *foundValue;
		}
	}
	else
	{
		for (int i = 0; i < inputValue->val.object.nPairs; i++)
		{
			if (inputValue->val.object.pairs[i].key.val.string.len ==
				searchKey.val.string.len &&
				strncmp(inputValue->val.object.pairs[i].key.val.string.val,
						searchKey.val.string.val, searchKey.val.string.len) == 0)
			{
				found = true;
				*inputValue = inputValue->val.object.pairs[i].value;
				break;
			}
		}
	}

	if (!found)
	{
		return false;
	}

	if (firstDot == NULL)
	{
		return true;
	}

	char *suffix = firstDot + 1;
	uint32_t remainingLength = (&path[pathLength - 1] - suffix) + 1;
	return FindPathInJsonbValue(inputValue, suffix, remainingLength);
}


static Datum *
GenerateJsonbTermsCore(Jsonb *inputDoc, JsonbExtendedRumOptions *options,
					   int32_t *nentries)
{
	const char *path;
	uint32_t pathLength;
	Get_Index_Path_Option(options, pathSpec, path, pathLength);

	/* Find the prefix for path term generation first */
	JsonbValue jbv;
	JsonbToJsonbValue(inputDoc, &jbv);

	if (pathLength > 1)
	{
		/* if it's not just '$', then find the subtree */
		if (!FindPathInJsonbValue(&jbv, path + 2, pathLength - 2))
		{
			*nentries = 0;
			return NULL;
		}
	}

	/* We now have a value from which to generate terms */
	Datum *entries = palloc(sizeof(Datum) * 1);
	if (options->wildcard)
	{
		/* We need a path prefix */
		if (!IsAJsonbScalar(&jbv))
		{
			ereport(ERROR, (errmsg("Wildcard not yet supported")));
		}

		JsonbValue container = jbv;
		container.type = jbvObject;
		container.val.object.nPairs = 1;
		container.val.object.pairs = palloc(sizeof(JsonbPair) * 1);
		container.val.object.pairs[0].key.type = jbvString;
		container.val.object.pairs[0].key.val.string.len = 1;
		container.val.object.pairs[0].key.val.string.val = "$";
		container.val.object.pairs[0].value = jbv;

		entries[0] = PointerGetDatum(JsonbValueToJsonb(&container));
		*nentries = 1;
	}
	else
	{
		entries[0] = PointerGetDatum(JsonbValueToJsonb(&jbv));
		*nentries = 1;
	}

	if (message_level_is_interesting(DEBUG1))
	{
		char *jsonbString = JsonbToCString(NULL, &(DatumGetJsonbP(entries[0])->root), 0);
		elog(DEBUG1, "First term %s", jsonbString);
	}

	return entries;
}
