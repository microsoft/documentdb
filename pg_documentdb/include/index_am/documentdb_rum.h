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
										  ambeginscan_function core_beginscan);
void extension_rumendscan_core(IndexScanDesc scan, amendscan_function core_endscan_func);
void extension_rumrescan_core(IndexScanDesc scan, ScanKey scankey, int nscankeys,
							  ScanKey orderbys, int norderbys, amrescan_function
							  core_rescanfunc);
int64 extension_rumgetbitmap_core(IndexScanDesc scan, TIDBitmap *tbm, amgetbitmap_function
								  core_bitmap);
bool extension_rumgettuple_core(IndexScanDesc scan, ScanDirection direction,
								amgettuple_function core_gettuple);


void extension_rumcostestimate(PlannerInfo *root, IndexPath *path, double
							   loop_count,
							   Cost *indexStartupCost, Cost *indexTotalCost,
							   Selectivity *indexSelectivity,
							   double *indexCorrelation,
							   double *indexPages);

#endif
