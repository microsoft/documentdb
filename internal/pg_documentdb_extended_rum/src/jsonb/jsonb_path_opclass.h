/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/jsonb/jsonb_path_opclass.h
 *
 * Common declarations of the jsonb opclass management
 *
 *-------------------------------------------------------------------------
 */

#ifndef JSONB_PATH_OPCLASS_H
#define JSONB_PATH_OPCLASS_H

const char * GetIndexPathFromOptions(bytea *options, uint32_t *searchPathLength);

#endif
