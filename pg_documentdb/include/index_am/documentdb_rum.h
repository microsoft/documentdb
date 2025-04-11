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

void LoadRumRoutine(void);
IndexAmRoutine *GetRumIndexHandler(PG_FUNCTION_ARGS);

IndexScanDesc extension_rumbeginscan_core(Relation rel, int nkeys, int norderbys,
										  IndexAmRoutine *coreRoutine);
void extension_rumendscan_core(IndexScanDesc scan, IndexAmRoutine *coreRoutine);
void extension_rumrescan_core(IndexScanDesc scan, ScanKey scankey, int nscankeys,
							  ScanKey orderbys, int norderbys,
							  IndexAmRoutine *coreRoutine);
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

#endif
