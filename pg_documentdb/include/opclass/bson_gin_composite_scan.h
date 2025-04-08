/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * include/opclass/bson_gin_index_mgmt.h
 *
 * Common declarations of the bson index management methods.
 *
 *-------------------------------------------------------------------------
 */

 #ifndef BSON_GIN_COMPOSITE_SCAN_H
 #define BSON_GIN_COMPOSITE_SCAN_H

 #include <access/skey.h>

void ModifyScanKeysForCompositeScan(ScanKey scankey, int nscankeys, ScanKey
									targetScanKey, bool hasArrayKeys);
 #endif
