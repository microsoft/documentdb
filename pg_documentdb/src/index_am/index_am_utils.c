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
#include "access/relscan.h"

#include <miscadmin.h>

/* The registry should not be exposed outside this c file to avoid unpredictable behavior */
static BsonIndexAmEntry BsonAlternateAmRegistry[5] = { 0 };
static int BsonNumAlternateAmEntries = 0;

extern bool EnableNewCompositeIndexOpclass;

/* Left non-static for internal use */
BsonIndexAmEntry RumIndexAmEntry = {
	.is_single_path_index_supported = true,
	.is_unique_index_supported = true,
	.is_wild_card_supported = true,
	.is_composite_index_supported = true,
	.is_text_index_supported = true,
	.is_hashed_index_supported = true,
	.is_order_by_supported = true,
	.get_am_oid = RumIndexAmId,
	.get_single_path_op_family_oid = BsonRumSinglePathOperatorFamily,
	.get_composite_path_op_family_oid = BsonRumCompositeIndexOperatorFamily,
	.get_text_path_op_family_oid = BsonRumTextPathOperatorFamily,
	.add_explain_output = NULL, /* No explain output for RUM */
	.am_name = "rum"
};

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


static const BsonIndexAmEntry *
GetBsonIndexAmEntryByIndexOid(Oid indexAm)
{
	if (indexAm == RumIndexAmId())
	{
		return &RumIndexAmEntry;
	}
	else if (IsClusterVersionAtleast(DocDB_V0, 104, 0))
	{
		for (int i = 0; i < BsonNumAlternateAmEntries; i++)
		{
			if (BsonAlternateAmRegistry[i].get_am_oid() == indexAm)
			{
				return &BsonAlternateAmRegistry[i];
			}
		}
	}

	return NULL;
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
const BsonIndexAmEntry *
GetBsonIndexAmByIndexAmName(const char *index_am_name)
{
	if (strcmp(index_am_name, RumIndexAmEntry.am_name) == 0)
	{
		return &RumIndexAmEntry;
	}

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
	const BsonIndexAmEntry *amEntry = GetBsonIndexAmEntryByIndexOid(indexAm);
	return amEntry != NULL;
}


void
TryExplainByIndexAm(struct IndexScanDescData *scan, struct ExplainState *es)
{
	const BsonIndexAmEntry *amEntry = GetBsonIndexAmEntryByIndexOid(
		scan->indexRelation->rd_rel->relam);

	if (amEntry == NULL || amEntry->add_explain_output == NULL)
	{
		/* No explain output for this index AM */
		return;
	}

	amEntry->add_explain_output(scan, es);
}


/*
 * Whether the opFamily of an index is a single path index
 */
bool
IsSinglePathOpFamilyOid(Oid relam, Oid opFamilyOid)
{
	const BsonIndexAmEntry *amEntry = GetBsonIndexAmEntryByIndexOid(relam);
	if (amEntry == NULL)
	{
		return false;
	}

	return opFamilyOid == amEntry->get_single_path_op_family_oid();
}


bool
IsTextPathOpFamilyOid(Oid relam, Oid opFamilyOid)
{
	const BsonIndexAmEntry *amEntry = GetBsonIndexAmEntryByIndexOid(relam);
	if (amEntry == NULL)
	{
		return false;
	}

	return opFamilyOid == amEntry->get_text_path_op_family_oid();
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

	const BsonIndexAmEntry *amEntry = GetBsonIndexAmEntryByIndexOid(
		indexRelation->rd_rel->relam);
	if (amEntry == NULL)
	{
		return false;
	}

	return indexRelation->rd_opfamily[0] == amEntry->get_composite_path_op_family_oid();
}


bool
IsCompositeOpFamilyOid(Oid relam, Oid opFamilyOid)
{
	if (!EnableNewCompositeIndexOpclass)
	{
		return false;
	}

	const BsonIndexAmEntry *amEntry = GetBsonIndexAmEntryByIndexOid(relam);

	if (amEntry == NULL)
	{
		return false;
	}

	return amEntry->get_composite_path_op_family_oid() == opFamilyOid;
}


/*
 * Whether order by is supported for a opclass of an index Am.
 */
bool
IsOrderBySupportedOnOpClass(Oid indexAm, Oid columnOpFamilyAm)
{
	const BsonIndexAmEntry *amEntry = GetBsonIndexAmEntryByIndexOid(indexAm);

	if (amEntry == NULL)
	{
		return false;
	}

	return amEntry->is_order_by_supported &&
		   amEntry->get_composite_path_op_family_oid() == columnOpFamilyAm;
}
