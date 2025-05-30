/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/index_am/index_am_utils.c
 *
 *
 * Utlities for alternate index access methods
 *
 *-------------------------------------------------------------------------
 */

#include "index_am/index_am_utils.h"
#include "utils/feature_counter.h"

#include <miscadmin.h>

/* The registry should not be exposed outside this c file to avoid unpredictable behavior */
static BsonIndexAmEntry BsonAlternateAmRegistry[5];
static int BsonNumAlternateAmEntries = 0;

extern bool EnableNewCompositeIndexOpclass;

/*
 * Registers an index access method in the index AM registry.
 * The registry contains all the supported index access methods.
 * If an index was created using a different access methods than
 * the one currently set as default for creating new index on bson
 * data type, then on the read path we look into the regestry to find
 * the appropriate index AM to answer the query.
 */
void
RegisterIndexAm(BsonIndexAmEntry indexAmEntry)
{
	if (!process_shared_preload_libraries_in_progress)
	{
		ereport(ERROR, (errmsg(
							"Alternate index AM registration must happen during shared_preload_libraries")));
	}

	if (BsonNumAlternateAmEntries >= MAX_ALTERNATE_INDEX_AMS)
	{
		ereport(ERROR,
				(errmsg("Only %d alternate index AMs are allowed",
						MAX_ALTERNATE_INDEX_AMS)));
	}

	BsonAlternateAmRegistry[BsonNumAlternateAmEntries++] = indexAmEntry;
}


/* Sets the Oid of the registered alternate indexAms into an input array starting at a given index */
int
SetDynamicIndexAmOidsAndGetCount(Datum *indexAmArray, int32_t indexAmArraySize)
{
	for (int i = 0; i < BsonNumAlternateAmEntries; i++)
	{
		indexAmArray[indexAmArraySize++] = BsonAlternateAmRegistry[i].get_am_oid();
	}

	return BsonNumAlternateAmEntries;
}


/*
 * Gets a registered index AM entry along with all its capabilities and utility functions
 * by the name of the index AM. We throw an error if the requested index AM is not found,
 * as by the time we call them it should already have been registered.
 */
BsonIndexAmEntry *
GetBsonIndexAmByIndexAmName(const char *index_am_name)
{
	for (int i = 0; i < BsonNumAlternateAmEntries; i++)
	{
		if (strcmp(BsonAlternateAmRegistry[i].am_name, index_am_name) == 0)
		{
			return &BsonAlternateAmRegistry[i];
		}
	}

	ereport(ERROR, (errmsg("Index access method %s is not found", index_am_name)));
}


/*
 * Is the Index Acess Method used for indexing bson (as opposed to indexing TEXT, Vector, Points etc)
 * as indicated by enum MongoIndexKind_Regular.
 */
bool
IsBsonRegularIndexAm(Oid indexAm)
{
	if (indexAm == RumIndexAmId())
	{
		return true;
	}
	else if (IsClusterVersionAtleast(DocDB_V0, 104, 0) && BsonNumAlternateAmEntries > 0)
	{
		for (int i = 0; i < BsonNumAlternateAmEntries; i++)
		{
			if (BsonAlternateAmRegistry[i].get_am_oid() == indexAm)
			{
				return true;
			}
		}
	}

	return false;
}


/*
 * Whether the opFamily of an index is a single path index
 */
bool
IsSinglePathOpFamilyOid(Oid opFamilyOid)
{
	if (opFamilyOid == BsonRumSinglePathOperatorFamily())
	{
		return true;
	}
	else if (IsClusterVersionAtleast(DocDB_V0, 104, 0) &&
			 BsonNumAlternateAmEntries > 0)
	{
		for (int i = 0; i < BsonNumAlternateAmEntries; i++)
		{
			if (BsonAlternateAmRegistry[i].get_single_path_op_family_oid() == opFamilyOid)
			{
				return true;
			}
		}
	}

	return false;
}


/*
 * Whether the index relation was created via a composite index opclass
 */
bool
IsCompositeOpClass(Relation indexRelation)
{
	if (!EnableNewCompositeIndexOpclass || IndexRelationGetNumberOfKeyAttributes(
			indexRelation) != 1)
	{
		return false;
	}

	Oid opFamilyOid = indexRelation->rd_opfamily[0];

	if (opFamilyOid == BsonRumCompositeIndexOperatorFamily())
	{
		return true;
	}
	else if (IsClusterVersionAtleast(DocDB_V0, 104, 0) &&
			 BsonNumAlternateAmEntries > 0)
	{
		for (int i = 0; i < BsonNumAlternateAmEntries; i++)
		{
			if (BsonAlternateAmRegistry[i].get_composite_path_op_family_oid() ==
				opFamilyOid)
			{
				return true;
			}
		}
	}

	return false;
}


/*
 * Whether order by is supported for a opclass of an index Am.
 */
bool
IsOrderBySupportedOnOpClass(Oid indexAm, Oid columnOpFamilyAm)
{
	if (indexAm == RumIndexAmId() &&
		columnOpFamilyAm == BsonRumCompositeIndexOperatorFamily())
	{
		return true;
	}
	else if (IsClusterVersionAtleast(DocDB_V0, 104, 0) && BsonNumAlternateAmEntries > 0)
	{
		for (int i = 0; i < BsonNumAlternateAmEntries; i++)
		{
			if (BsonAlternateAmRegistry[i].is_order_by_supported &&
				BsonAlternateAmRegistry[i].get_am_oid() == indexAm &&
				columnOpFamilyAm ==
				BsonAlternateAmRegistry[i].get_composite_path_op_family_oid())
			{
				return true;
			}
		}
	}

	return false;
}
