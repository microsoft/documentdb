/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/oss_backend/commands/compact.c
 *
 * Implementation of the compact command.
 *-------------------------------------------------------------------------
 */
#include <postgres.h>
#include <fmgr.h>

#include "metadata/collection.h"
#include "utils/documentdb_errors.h"

/* --------------------------------------------------------- */
/* Top level exports */
/* --------------------------------------------------------- */

PG_FUNCTION_INFO_V1(command_compact);

/*
 * command_compact implements the functionality of compact Database command
 * dbcommand/compact.
 */
Datum
command_compact(PG_FUNCTION_ARGS)
{
	ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_COMMANDNOTSUPPORTED),
					errmsg("Compact command is not supported yet")));
}
