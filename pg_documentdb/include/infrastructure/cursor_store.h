/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * include/planner/documentdb_plan_cache.h
 *
 * Common declarations for the pg_documentdb plan cache.
 *
 *-------------------------------------------------------------------------
 */

#ifndef DOCUMENTDB_CURSOR_STORE_H
#define DOCUMENTDB_CURSOR_STORE_H
#include <postgres.h>

typedef struct CursorFileState CursorFileState;

void SetupCursorStorage(void);

CursorFileState * GetCursorFile(const char *cursorName);
void WriteToCursorFile(CursorFileState *cursorFileState, pgbson *bson);
pgbson * ReadFromCursorFile(CursorFileState *cursorFileState);
bytea * CursorFileStateClose(CursorFileState *cursorFileState);

CursorFileState * DeserializeFileState(bytea *cursorFileState);

#endif
