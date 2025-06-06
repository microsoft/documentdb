/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * include/index_am/documentdb_rum.h
 *
 * Common declarations for RUM specific helper functions.
 *
 *-------------------------------------------------------------------------
 */

#ifndef DOCUMENTDB_RUM_H
#define DOCUMENTDB_RUM_H

#include <fmgr.h>
#include <access/amapi.h>
#include <nodes/pathnodes.h>

typedef void *(*CreateIndexArrayTrackerState)(void);
typedef bool (*IndexArrayTrackerAdd)(void *state, ItemPointer item);
typedef void (*FreeIndexArrayTrackerState)(void *);
typedef void (*UpdateMultikeyStatusFunc)(bool isBuild, Relation index);

typedef bool (*GetMultikeyStatusFunc)(Relation indexRelation);

/*
 * Adapter struct that provides function pointers to allow
 * for extensibility in managing index array state for index scans.
 * The current requirements on the interface is to provide an abstraction
 * that can be used to deduplicate array entries in the index scan.
 */
typedef struct RumIndexArrayStateFuncs
{
	/* Create opaque state to manage entries in this specific index scan */
	CreateIndexArrayTrackerState createState;

	/* Add an item to the index scan and return whether or not it is new or existing */
	IndexArrayTrackerAdd addItem;

	/* Frees the temporary state used for the adding of items */
	FreeIndexArrayTrackerState freeState;
} RumIndexArrayStateFuncs;


/* Registers an extensibility that handles index array deduplication */
void RegisterIndexArrayStateFuncs(RumIndexArrayStateFuncs *funcs);

void LoadRumRoutine(void);
IndexAmRoutine *GetRumIndexHandler(PG_FUNCTION_ARGS);

IndexScanDesc extension_rumbeginscan_core(Relation rel, int nkeys, int norderbys,
										  IndexAmRoutine *coreRoutine);
void extension_rumendscan_core(IndexScanDesc scan, IndexAmRoutine *coreRoutine);
void extension_rumrescan_core(IndexScanDesc scan, ScanKey scankey, int nscankeys,
							  ScanKey orderbys, int norderbys,
							  IndexAmRoutine *coreRoutine,
							  GetMultikeyStatusFunc multiKeyStatusFunc);
int64 extension_rumgetbitmap_core(IndexScanDesc scan, TIDBitmap *tbm,
								  IndexAmRoutine *coreRoutine);
bool extension_rumgettuple_core(IndexScanDesc scan, ScanDirection direction,
								IndexAmRoutine *coreRoutine);


void extension_rumcostestimate(PlannerInfo *root, IndexPath *path, double
							   loop_count,
							   Cost *indexStartupCost, Cost *indexTotalCost,
							   Selectivity *indexSelectivity,
							   double *indexCorrelation,
							   double *indexPages);

IndexBuildResult * extension_rumbuild_core(Relation heapRelation, Relation indexRelation,
										   struct IndexInfo *indexInfo,
										   IndexAmRoutine *coreRoutine,
										   UpdateMultikeyStatusFunc updateMultikeyStatus,
										   bool amCanBuildParallel);

bool extension_ruminsert_core(Relation indexRelation,
							  Datum *values,
							  bool *isnull,
							  ItemPointer heap_tid,
							  Relation heapRelation,
							  IndexUniqueCheck checkUnique,
							  bool indexUnchanged,
							  struct IndexInfo *indexInfo,
							  IndexAmRoutine *coreRoutine,
							  UpdateMultikeyStatusFunc updateMultikeyStatus);

void RumUpdateMultiKeyStatus(bool isBuild, Relation index);
bool RumGetMultikeyStatus(Relation indexRelation);

struct ExplainState;
void ExplainCompositeScan(IndexScanDesc scan, struct ExplainState *es);

void ExplainRegularIndexScan(IndexScanDesc scan, struct ExplainState *es);
#endif
