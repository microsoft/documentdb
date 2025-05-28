/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * include/commands/commands_common.h
 *
 * Common declarations of Mongo commands.
 *
 *-------------------------------------------------------------------------
 */

#ifndef COMMANDS_COMMON_H
#define COMMANDS_COMMON_H

#include <utils/elog.h>
#include <metadata/collection.h>
#include <io/bson_core.h>
#include <utils/documentdb_errors.h>
#include <access/xact.h>
#include <access/xlog.h>

/*
 * Maximum size of a output bson document is 16MB.
 */
#define BSON_MAX_ALLOWED_SIZE (16 * 1024 * 1024)

/*
 * Maximum size of document produced by an intermediate stage of an aggregation pipeline.
 * This is a Native Mongo constrains. For example, if the pipeline is [$facet, $unwind],
 * $facet is allowed to generate document that is larger than 16MB, since $unwind can
 * break them into smaller document. However, $facet is not allowed to generate a document
 * that's larger than 100MB.
 */
#define BSON_MAX_ALLOWED_SIZE_INTERMEDIATE (100 * 1024 * 1024)

/* StringView that represents the _id field */
extern PGDLLIMPORT const StringView IdFieldStringView;


/*
 * ApiGucPrefix.enable_create_collection_on_insert GUC determines whether
 * an insert into a non-existent collection should create a collection.
 */
extern bool EnableCreateCollectionOnInsert;

/*
 * Whether or not write operations are inlined or if they are dispatched
 * to a remote shard. For single node scenarios like DocumentDB that don't need
 * distributed dispatch. Reset in scenarios that need distributed dispatch.
 */
extern bool DefaultInlineWriteOperations;
extern int BatchWriteSubTransactionCount;
extern int MaxWriteBatchSize;

/*
 * WriteError can be part of the response of a batch write operation.
 */
typedef struct WriteError
{
	/* index in a write batch */
	int index;

	/* error code */
	int code;

	/* description of the error */
	char *errmsg;
} WriteError;


bool FindShardKeyValueForDocumentId(MongoCollection *collection, const
									bson_value_t *queryDoc,
									bson_value_t *objectId, int64 *shardKeyValue);

bool IsCommonSpecIgnoredField(const char *fieldName);

WriteError * GetWriteErrorFromErrorData(ErrorData *errorData, int writeErrorIdx);
bool TryGetErrorMessageAndCode(ErrorData *errorData, int *code, char **errmessage);

pgbson * GetObjectIdFilterFromQueryDocumentValue(const bson_value_t *queryDoc,
												 bool *hasNonIdFields);
pgbson * GetObjectIdFilterFromQueryDocument(pgbson *queryDoc, bool *hasNonIdFields);


pgbson * RewriteDocumentAddObjectId(pgbson *document);
pgbson * RewriteDocumentValueAddObjectId(const bson_value_t *value);
pgbson * RewriteDocumentWithCustomObjectId(pgbson *document,
										   pgbson *objectIdToWrite);

void ValidateIdField(const bson_value_t *idValue);
void SetExplicitStatementTimeout(int timeoutMilliseconds);

void CommitWriteProcedureAndReacquireCollectionLock(MongoCollection *collection,
													bool setSnapshot);

extern bool SkipEnforceTransactionReadOnly;
extern bool SimulateRecoveryState;
extern bool DocumentDBPGReadOnlyForDiskFull;

inline static void
ThrowIfServerOrTransactionReadOnly(void)
{
	if (!XactReadOnly)
	{
		return;
	}

	if (SkipEnforceTransactionReadOnly)
	{
		return;
	}

	if (RecoveryInProgress() || SimulateRecoveryState)
	{
		/*
		 * Skip these checks in recovery mode - let the system throw the appropriate
		 * error.
		 */
		return;
	}

	if (DocumentDBPGReadOnlyForDiskFull)
	{
		ereport(ERROR, (errcode(ERRCODE_DISK_FULL), errmsg(
							"Can't execute write operation, The database disk is full")));
	}

	/* Error is coming because the server has been put in a read-only state, but we're a writable node (primary) */
	if (DefaultXactReadOnly)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_NOTWRITABLEPRIMARY),
						errmsg(
							"cannot execute write operations when the server is in a read-only state."),
						errdetail("the default transaction is read-only"),
						errdetail_log(
							"cannot execute write operations when default_transaction_read_only is set to true")));
	}

	/* Error is coming because the transaction has been in a readonly state */
	ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_OPERATIONNOTSUPPORTEDINTRANSACTION),
					errmsg(
						"cannot execute write operation when the transaction is in a read-only state."),
					errdetail("the current transaction is read-only")));
}


#endif
