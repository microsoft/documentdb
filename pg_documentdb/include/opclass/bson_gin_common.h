/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * include/opclass/bson_gin_common.h
 *
 * Common declarations of the bson gin methods.
 *
 *-------------------------------------------------------------------------
 */

#ifndef BSON_GIN_COMMON_H
#define BSON_GIN_COMMON_H

/*
 * Maps the set of operators for the gin index as strategies that are used in
 * gin operator functions.
 */
typedef enum BsonIndexStrategy
{
	BSON_INDEX_STRATEGY_INVALID = 0,
	BSON_INDEX_STRATEGY_DOLLAR_EQUAL = 1,
	BSON_INDEX_STRATEGY_DOLLAR_GREATER = 2,
	BSON_INDEX_STRATEGY_DOLLAR_GREATER_EQUAL = 3,
	BSON_INDEX_STRATEGY_DOLLAR_LESS = 4,
	BSON_INDEX_STRATEGY_DOLLAR_LESS_EQUAL = 5,
	BSON_INDEX_STRATEGY_DOLLAR_IN = 6,
	BSON_INDEX_STRATEGY_DOLLAR_NOT_EQUAL = 7,
	BSON_INDEX_STRATEGY_DOLLAR_NOT_IN = 8,
	BSON_INDEX_STRATEGY_DOLLAR_REGEX = 9,
	BSON_INDEX_STRATEGY_DOLLAR_EXISTS = 10,
	BSON_INDEX_STRATEGY_DOLLAR_SIZE = 11,
	BSON_INDEX_STRATEGY_DOLLAR_TYPE = 12,
	BSON_INDEX_STRATEGY_DOLLAR_ALL = 13,
	BSON_INDEX_STRATEGY_UNIQUE_EQUAL = 14,
	BSON_INDEX_STRATEGY_DOLLAR_BITS_ALL_CLEAR = 15,
	BSON_INDEX_STRATEGY_DOLLAR_BITS_ANY_CLEAR = 16,
	BSON_INDEX_STRATEGY_DOLLAR_ELEMMATCH = 17,
	BSON_INDEX_STRATEGY_DOLLAR_BITS_ALL_SET = 18,
	BSON_INDEX_STRATEGY_DOLLAR_BITS_ANY_SET = 19,
	BSON_INDEX_STRATEGY_DOLLAR_MOD = 20,
	BSON_INDEX_STRATEGY_DOLLAR_ORDERBY = 21,
	BSON_INDEX_STRATEGY_DOLLAR_TEXT = 22,
	BSON_INDEX_STRATEGY_DOLLAR_GEOWITHIN = 23,
	BSON_INDEX_STRATEGY_DOLLAR_GEOINTERSECTS = 24,
	BSON_INDEX_STRATEGY_DOLLAR_RANGE = 25,
	BSON_INDEX_STRATEGY_DOLLAR_NOT_GT = 26,
	BSON_INDEX_STRATEGY_DOLLAR_NOT_GTE = 27,
	BSON_INDEX_STRATEGY_DOLLAR_NOT_LT = 28,
	BSON_INDEX_STRATEGY_DOLLAR_NOT_LTE = 29,
	BSON_INDEX_STRATEGY_GEONEAR = 30,
	BSON_INDEX_STRATEGY_GEONEAR_RANGE = 31,
	BSON_INDEX_STRATEGY_COMPOSITE_QUERY = 32,
	BSON_INDEX_STRATEGY_IS_MULTIKEY = 33,
} BsonIndexStrategy;

#endif
