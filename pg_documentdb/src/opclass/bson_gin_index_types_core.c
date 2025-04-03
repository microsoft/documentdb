/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/opclass/bson_gin_index_types_core.c
 *
 *
 * Core logic for type management of BSON for indexes
 * See also: https://www.postgresql.org/docs/current/gin-extensibility.html
 *
 *-------------------------------------------------------------------------
 */


#include <postgres.h>
#include <fmgr.h>
#include <miscadmin.h>
#include <math.h>

#include "io/bson_core.h"
#include "query/bson_compare.h"
#include <opclass/bson_gin_index_types_core.h>

bson_value_t
GetUpperBound(bson_type_t type, bool *isUpperBoundInclusive)
{
	bson_value_t upperBound = { 0 };
	*isUpperBoundInclusive = true;
	switch (type)
	{
		case BSON_TYPE_MINKEY:
		{
			upperBound.value_type = BSON_TYPE_MINKEY;
			break;
		}

		case BSON_TYPE_UNDEFINED:
		case BSON_TYPE_NULL:
		{
			/* Since these are equivalent it doesn't matter
			 * which one you pick, they'll be treated as the same
			 */
			upperBound.value_type = BSON_TYPE_NULL;
			break;
		}

		case BSON_TYPE_DOUBLE:
		case BSON_TYPE_INT32:
		case BSON_TYPE_INT64:
		case BSON_TYPE_DECIMAL128:
		{
			/* For numbers positive infinity inclusive is the largest number */
			upperBound.value_type = BSON_TYPE_DOUBLE;
			upperBound.value.v_double = INFINITY;
			break;
		}


		case BSON_TYPE_UTF8:
		case BSON_TYPE_SYMBOL:
		{
			/* If we want to be able to accept UTF8 strings with invalid sequences
			 * then the max UTf8 string can only be the smallest document exclusive
			 */
			bool isBoundInclusiveIgnore = false;
			upperBound = GetLowerBound(BSON_TYPE_DOCUMENT, &isBoundInclusiveIgnore);
			*isUpperBoundInclusive = false;
			break;
		}

		case BSON_TYPE_DOCUMENT:
		{
			/* If we want to be able to accept any document
			 * then the max document string can only be the smallest array exclusive
			 */
			bool isBoundInclusiveIgnore = false;
			upperBound = GetLowerBound(BSON_TYPE_ARRAY, &isBoundInclusiveIgnore);
			*isUpperBoundInclusive = false;
			break;
		}

		case BSON_TYPE_ARRAY:
		{
			/* Pick the smallest binary value exclusive */
			bool isBoundInclusiveIgnore = false;
			upperBound = GetLowerBound(BSON_TYPE_BINARY, &isBoundInclusiveIgnore);
			*isUpperBoundInclusive = false;
			break;
		}

		case BSON_TYPE_BINARY:
		{
			/* Exclusive to the lower bound of the next type */
			bool isBoundInclusiveIgnore = false;
			upperBound = GetLowerBound(BSON_TYPE_OID, &isBoundInclusiveIgnore);
			*isUpperBoundInclusive = false;
			break;
		}

		case BSON_TYPE_OID:
		{
			bool isBoundInclusiveIgnore = false;
			upperBound = GetLowerBound(BSON_TYPE_BOOL, &isBoundInclusiveIgnore);
			*isUpperBoundInclusive = false;
			break;
		}

		case BSON_TYPE_BOOL:
		{
			/* Highest bool value is true - inclusive */
			upperBound.value_type = BSON_TYPE_BOOL;
			upperBound.value.v_bool = true;
			break;
		}

		case BSON_TYPE_DATE_TIME:
		{
			upperBound.value_type = BSON_TYPE_DATE_TIME;
			upperBound.value.v_datetime = INT64_MAX;
			*isUpperBoundInclusive = true;  /* INT64_MAX is inclusive */
			break;
		}

		case BSON_TYPE_TIMESTAMP:
		{
			upperBound.value_type = BSON_TYPE_TIMESTAMP;
			upperBound.value.v_timestamp.increment = UINT32_MAX;
			upperBound.value.v_timestamp.timestamp = UINT32_MAX;
			*isUpperBoundInclusive = true;
			break;
		}

		case BSON_TYPE_REGEX:
		{
			/* Exclusive to the lower bound of the next type */
			bool isBoundInclusiveIgnore = false;
			upperBound = GetLowerBound(BSON_TYPE_DBPOINTER, &isBoundInclusiveIgnore);
			*isUpperBoundInclusive = false;
			break;
		}

		case BSON_TYPE_DBPOINTER:
		{
			/* Exclusive to the lower bound of the next type */
			bool isBoundInclusiveIgnore = false;
			upperBound = GetLowerBound(BSON_TYPE_CODE, &isBoundInclusiveIgnore);
			*isUpperBoundInclusive = false;
			break;
		}

		case BSON_TYPE_CODE:
		{
			/* Exclusive to the lower bound of the next type */
			bool isBoundInclusiveIgnore = false;
			upperBound = GetLowerBound(BSON_TYPE_CODEWSCOPE, &isBoundInclusiveIgnore);
			*isUpperBoundInclusive = false;
			break;
		}

		case BSON_TYPE_CODEWSCOPE:
		{
			/* Exclusive to the lower bound of the next type */
			bool isBoundInclusiveIgnore = false;
			upperBound = GetLowerBound(BSON_TYPE_MAXKEY, &isBoundInclusiveIgnore);
			*isUpperBoundInclusive = false;
			break;
		}

		case BSON_TYPE_MAXKEY:
		{
			upperBound.value_type = BSON_TYPE_MAXKEY;
			break;
		}

		default:
		{
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg(
								"Unsupported BSON type for upper bound determination: %d",
								type),
							errdetail_log(
								"The type %s is not supported in composite index upper bounds.",
								BsonTypeName(type))));
		}
	}

	return upperBound;
}


bson_value_t
GetLowerBound(bson_type_t type, bool *isLowerBoundInclusive)
{
	bson_value_t lowerBound = { 0 };
	*isLowerBoundInclusive = false;
	switch (type)
	{
		case BSON_TYPE_MINKEY:
		{
			lowerBound.value_type = BSON_TYPE_MINKEY;
			break;
		}

		case BSON_TYPE_UNDEFINED:
		case BSON_TYPE_NULL:
		{
			lowerBound.value_type = BSON_TYPE_NULL;
			break;
		}

		case BSON_TYPE_DOUBLE:
		case BSON_TYPE_INT32:
		case BSON_TYPE_INT64:
		case BSON_TYPE_DECIMAL128:
		{
			lowerBound.value_type = BSON_TYPE_DOUBLE;
			lowerBound.value.v_double = -INFINITY; /* Negative infinity for lower bound */
			*isLowerBoundInclusive = true; /* Negative infinity is inclusive */
			break;
		}


		case BSON_TYPE_UTF8:
		case BSON_TYPE_SYMBOL:
		{
			lowerBound.value_type = BSON_TYPE_UTF8;
			lowerBound.value.v_utf8.str = ""; /* Empty string for lower bound */
			lowerBound.value.v_utf8.len = 0;
			*isLowerBoundInclusive = true; /* Empty string is inclusive */
			break;
		}

		case BSON_TYPE_DOCUMENT:
		{
			pgbson *emptyDoc = PgbsonInitEmpty();
			lowerBound = ConvertPgbsonToBsonValue(emptyDoc);
			*isLowerBoundInclusive = true;
			break;
		}

		case BSON_TYPE_ARRAY:
		{
			pgbson *emptyDoc = PgbsonInitEmpty();
			lowerBound = ConvertPgbsonToBsonValue(emptyDoc);
			lowerBound.value_type = BSON_TYPE_ARRAY;
			*isLowerBoundInclusive = true;
			break;
		}

		case BSON_TYPE_BINARY:
		{
			lowerBound.value_type = BSON_TYPE_BINARY;
			lowerBound.value.v_binary.data_len = 0;
			lowerBound.value.v_binary.data = NULL;
			lowerBound.value.v_binary.subtype = BSON_SUBTYPE_BINARY;
			*isLowerBoundInclusive = true; /* Empty binary is inclusive */
			break;
		}

		case BSON_TYPE_OID:
		{
			lowerBound.value_type = BSON_TYPE_OID;
			memset(lowerBound.value.v_oid.bytes, 0, sizeof(lowerBound.value.v_oid.bytes));
			*isLowerBoundInclusive = true;
			break;
		}

		case BSON_TYPE_BOOL:
		{
			lowerBound.value_type = BSON_TYPE_BOOL;
			lowerBound.value.v_bool = false; /* Lowest bool value is false */
			*isLowerBoundInclusive = true; /* false is inclusive */
			break;
		}

		case BSON_TYPE_DATE_TIME:
		{
			lowerBound.value_type = BSON_TYPE_DATE_TIME;
			lowerBound.value.v_datetime = INT64_MIN; /* Minimum date time value */
			*isLowerBoundInclusive = true;  /* INT64_MIN is inclusive */
			break;
		}

		case BSON_TYPE_TIMESTAMP:
		{
			lowerBound.value_type = BSON_TYPE_TIMESTAMP;
			lowerBound.value.v_timestamp.increment = 0;
			lowerBound.value.v_timestamp.timestamp = 0; /* Minimum timestamp value */
			*isLowerBoundInclusive = true; /* 0 is inclusive */
			break;
		}

		case BSON_TYPE_REGEX:
		{
			lowerBound.value_type = BSON_TYPE_REGEX;
			lowerBound.value.v_regex.regex = ""; /* Empty regex for lower bound */
			lowerBound.value.v_regex.options = NULL; /* Empty options for lower bound */
			*isLowerBoundInclusive = true; /* Empty regex is inclusive */
			break;
		}

		case BSON_TYPE_DBPOINTER:
		{
			lowerBound.value_type = BSON_TYPE_DBPOINTER;
			lowerBound.value.v_dbpointer.collection_len = 0;
			lowerBound.value.v_dbpointer.collection = "";
			memset(lowerBound.value.v_dbpointer.oid.bytes, 0,
				   sizeof(lowerBound.value.v_dbpointer.oid.bytes));
			*isLowerBoundInclusive = true;
			break;
		}

		case BSON_TYPE_CODE:
		{
			lowerBound.value_type = BSON_TYPE_CODE;
			lowerBound.value.v_code.code = ""; /* Empty string for lower bound */
			lowerBound.value.v_code.code_len = 0;
			*isLowerBoundInclusive = true; /* Empty string is inclusive */
			break;
		}

		case BSON_TYPE_CODEWSCOPE:
		{
			lowerBound.value_type = BSON_TYPE_CODEWSCOPE;
			lowerBound.value.v_codewscope.code = ""; /* Empty string for lower bound */
			lowerBound.value.v_codewscope.code_len = 0;
			pgbson *emptyDoc = PgbsonInitEmpty();
			bson_value_t emptyDocValue = ConvertPgbsonToBsonValue(emptyDoc);
			lowerBound.value.v_codewscope.scope_data = emptyDocValue.value.v_doc.data;
			lowerBound.value.v_codewscope.scope_len = emptyDocValue.value.v_doc.data_len;
			*isLowerBoundInclusive = true; /* Empty string is inclusive */
			break;
		}

		case BSON_TYPE_MAXKEY:
		{
			lowerBound.value_type = BSON_TYPE_MAXKEY;
			break;
		}

		default:
		{
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg(
								"Unsupported BSON type for upper bound determination: %d",
								type),
							errdetail_log(
								"The type %s is not supported in composite index upper bounds.",
								BsonTypeName(type))));
		}
	}

	return lowerBound;
}


bson_type_t
GetBsonTypeNameFromStringForDollarType(const char *typeNameStr)
{
	if (strcmp(typeNameStr, "number") == 0)
	{
		/* Since $type on index only validates sort order, double is sufficient */
		return BSON_TYPE_DOUBLE;
	}
	else
	{
		return BsonTypeFromName(typeNameStr);
	}
}
