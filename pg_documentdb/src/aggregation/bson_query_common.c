/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/aggregation/bson_query_common.c
 *
 * Implementation of the common BSON query utility.
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>

#include "io/bson_core.h"
#include "aggregation/bson_query_common.h"


DollarRangeParams *
ParseQueryDollarRange(pgbsonelement *filterElement)
{
	DollarRangeParams *rangeParams = palloc0(sizeof(DollarRangeParams));
	InitializeQueryDollarRange(filterElement, rangeParams);
	return rangeParams;
}


void
InitializeQueryDollarRange(pgbsonelement *filterElement, DollarRangeParams *rangeParams)
{
	bson_iter_t rangeIter;
	BsonValueInitIterator(&filterElement->bsonValue, &rangeIter);
	while (bson_iter_next(&rangeIter))
	{
		const char *key = bson_iter_key(&rangeIter);
		if (strcmp(key, "min") == 0)
		{
			rangeParams->minValue = *bson_iter_value(&rangeIter);
		}
		else if (strcmp(key, "max") == 0)
		{
			rangeParams->maxValue = *bson_iter_value(&rangeIter);
		}
		else if (strcmp(key, "minInclusive") == 0)
		{
			rangeParams->isMinInclusive = bson_iter_bool(&rangeIter);
		}
		else if (strcmp(key, "maxInclusive") == 0)
		{
			rangeParams->isMaxInclusive = bson_iter_bool(&rangeIter);
		}
		else if (strcmp(key, "fullScan") == 0)
		{
			rangeParams->isFullScan = true;
		}
		else
		{
			ereport(ERROR, (errmsg("Unsupported range predicate: %s", key), errdetail_log(
								"Unsupported range predicate: %s", key)));
		}
	}

	if (rangeParams->isFullScan)
	{
		/* If full scan is requested, we ignore min and max values */
		rangeParams->minValue.value_type = BSON_TYPE_MINKEY;
		rangeParams->maxValue.value_type = BSON_TYPE_MAXKEY;
		rangeParams->isMinInclusive = true;
		rangeParams->isMaxInclusive = true;
	}
}
