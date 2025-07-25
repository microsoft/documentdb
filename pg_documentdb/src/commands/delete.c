/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/commands/delete.c
 *
 * Implementation of the delete command.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"

#include "access/xact.h"
#include "executor/spi.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"

#include "io/bson_core.h"
#include "aggregation/bson_project.h"
#include "aggregation/bson_query.h"
#include "collation/collation.h"
#include "commands/commands_common.h"
#include "commands/delete.h"
#include "commands/parse_error.h"
#include "metadata/collection.h"
#include "metadata/metadata_cache.h"
#include "query/query_operator.h"
#include "infrastructure/documentdb_plan_cache.h"
#include "sharding/sharding.h"
#include "commands/retryable_writes.h"
#include "io/pgbsonsequence.h"
#include "utils/error_utils.h"
#include "utils/documentdb_errors.h"
#include "utils/feature_counter.h"
#include "utils/version_utils.h"
#include "utils/query_utils.h"
#include "api_hooks.h"


/*
 * DeletionSpec describes a single delete operation.
 */
typedef struct
{
	DeleteOneParams deleteOneParams;

	/* delete limit (0 for all rows, 1 for 1 row) */
	int limit;
} DeletionSpec;


/*
 * BatchDeletionSpec describes a batch of delete operations.
 */
typedef struct
{
	/* collection in which to perform deletions */
	char *collectionName;

	/* DeletionSpec raw value */
	bson_value_t deletionValue;

	/* if ordered, stop after the first failure */
	bool isOrdered;

	/* The bsonSequence for the delete */
	pgbsonsequence *deletionSequence;

	/* DeletionSpec list describing the deletions */
	List *deletionsProcessed;

	/* parsed variable spec */
	bson_value_t variableSpec;
} BatchDeletionSpec;


/*
 * BatchDeletionResult contains the results that are sent to the
 * client after a delete command.
 */
typedef struct
{
	/* response status (seems to always be 1?) */
	double ok;

	/* number of rows deleted */
	uint64 rowsDeleted;

	/* list of write errors for each deletion, or NIL */
	List *writeErrors;
} BatchDeletionResult;

extern bool UseLocalExecutionShardQueries;
extern bool EnableVariablesSupportForWriteCommands;

PG_FUNCTION_INFO_V1(command_delete);
PG_FUNCTION_INFO_V1(command_delete_one);
PG_FUNCTION_INFO_V1(command_delete_worker);


static BatchDeletionSpec * BuildBatchDeletionSpec(bson_iter_t *deleteCommandIter,
												  pgbsonsequence *deleteDocs);
static List * BuildDeletionSpecList(bson_iter_t *deleteArrayIter,
									const bson_value_t *variableSpec);
static List * BuildDeletionSpecListFromSequence(pgbsonsequence *sequence,
												const bson_value_t *variableSpec);
static DeletionSpec * BuildDeletionSpec(bson_iter_t *deletionIterator,
										const bson_value_t *variableSpec);
static void ProcessBatchDeletion(MongoCollection *collection,
								 BatchDeletionSpec *batchSpec,
								 bool forceInline, text *transactionId,
								 BatchDeletionResult *batchResult);

static pgbson * ProcessBatchDeleteUnsharded(MongoCollection *collection,
											BatchDeletionSpec *batchSpec,
											text *transactionId);
static uint64 ProcessDeletion(MongoCollection *collection, DeletionSpec *deletionSpec,
							  bool forceInlineWrites, text *transactionId);
static uint64 DeleteAllMatchingDocuments(MongoCollection *collection, pgbson *query,
										 const bson_value_t *variableSpec,
										 const char *collationString,
										 bool hasShardKeyValueFilter,
										 int64 shardKeyHash);
static void DeleteOneInternal(MongoCollection *collection,
							  DeleteOneParams *deleteOneParams,
							  int64 shardKeyHash,
							  DeleteOneResult *result);
static void DeleteOneObjectId(MongoCollection *collection,
							  DeleteOneParams *deleteOneParams,
							  bson_value_t *objectId,
							  bool isIdValueCollationAware,
							  bool queryHasNonIdFilters,
							  bool forceInlineWrites,
							  text *transactionId, DeleteOneResult *result);
static List * ValidateQueryDocuments(BatchDeletionSpec *batchSpec);
static pgbson * BuildResponseMessage(BatchDeletionResult *batchResult);
static void DeleteOneInternalCore(MongoCollection *collection, int64 shardKeyHash,
								  DeleteOneParams *deleteOneParams,
								  text *transactionId, DeleteOneResult *deleteOneResult);
static pgbson * CallDeleteWorker(MongoCollection *collection,
								 pgbson *serializedDeleteSpec,
								 int64 shardKeyHash,
								 text *transactionId,
								 pgbsonsequence *sequence);
static pgbson * SerializeDeleteOneParams(const DeleteOneParams *deleteParams);
static void DeserializeDeleteWorkerSpecForDeleteOne(const bson_value_t *workerSpecValue,
													DeleteOneParams *deleteOneParams);
static void DeserializeWorkerDeleteResultForDeleteOne(pgbson *resultBson,
													  DeleteOneResult *result);
static pgbson * SerializeDeleteOneResult(DeleteOneResult *result);
static void PostProcessDeleteBatchSpec(BatchDeletionSpec *spec);
static void DeserializeDeleteWorkerSpecForUnsharded(const
													bson_value_t *deleteInternalSpec,
													BatchDeletionSpec *batchDeletionSpec);
static pgbson * SerializeDeleteWorkerSpecForUnsharded(BatchDeletionSpec *batchSpec);


/*
 * command_delete handles a single delete on a collection.
 */
Datum
command_delete(PG_FUNCTION_ARGS)
{
	if (PG_ARGISNULL(0))
	{
		ereport(ERROR, (errmsg("database name cannot be NULL")));
	}

	if (PG_ARGISNULL(1))
	{
		ereport(ERROR, (errmsg("delete document cannot be NULL")));
	}

	Datum databaseNameDatum = PG_GETARG_DATUM(0);
	pgbson *deleteSpec = PG_GETARG_PGBSON(1);

	pgbsonsequence *deleteDocs = PG_GETARG_MAYBE_NULL_PGBSON_SEQUENCE(2);

	text *transactionId = NULL;
	if (!PG_ARGISNULL(3))
	{
		transactionId = PG_GETARG_TEXT_P(3);
	}

	ReportFeatureUsage(FEATURE_COMMAND_DELETE);

	/* fetch TupleDesc for return value, not interested in resultTypeId */
	Oid *resultTypeId = NULL;
	TupleDesc resultTupDesc;
	TypeFuncClass resultTypeClass =
		get_call_result_type(fcinfo, resultTypeId, &resultTupDesc);

	if (resultTypeClass != TYPEFUNC_COMPOSITE)
	{
		ereport(ERROR, (errmsg("return type must be a row type")));
	}

	ThrowIfServerOrTransactionReadOnly();

	bson_iter_t deleteCommandIter;
	PgbsonInitIterator(deleteSpec, &deleteCommandIter);

	/*
	 * We first validate delete command BSON and build a specification.
	 */
	BatchDeletionSpec *batchSpec = BuildBatchDeletionSpec(&deleteCommandIter, deleteDocs);

	pgbson *batchResponse;

	Datum collectionNameDatum = CStringGetTextDatum(batchSpec->collectionName);
	MongoCollection *collection =
		GetMongoCollectionByNameDatum(databaseNameDatum, collectionNameDatum,
									  RowExclusiveLock);
	if (collection != NULL)
	{
		Oid shardOid = TryGetCollectionShardTable(collection, NoLock);
		if (shardOid == InvalidOid)
		{
			/* Shard not valid on this node anymore (due to shard moves etc) */
			collection->shardTableName[0] = '\0';
		}

		if (DefaultInlineWriteOperations ||
			collection->shardKey != NULL || collection->shardTableName[0] != '\0')
		{
			BatchDeletionResult batchResult = { 0 };
			bool forceInline = false;
			ProcessBatchDeletion(collection, batchSpec, forceInline, transactionId,
								 &batchResult);
			batchResponse = BuildResponseMessage(&batchResult);
		}
		else
		{
			/* Unsharded and the shard table is in a remote node we can push the whole batch to the worker directly. */
			batchResponse = ProcessBatchDeleteUnsharded(collection, batchSpec,
														transactionId);
		}
	}
	else
	{
		BatchDeletionResult batchResult = { 0 };
		StringView collectionView = {
			.length = VARSIZE_ANY_EXHDR(collectionNameDatum),
			.string = VARDATA_ANY(collectionNameDatum)
		};

		PostProcessDeleteBatchSpec(batchSpec);
		ValidateCollectionNameForValidSystemNamespace(&collectionView,
													  databaseNameDatum);

		/*
		 * Delete on non-existent collection is a noop, but we still need to
		 * report (write) errors due to invalid query documents.
		 */
		batchResult.ok = 1;
		batchResult.rowsDeleted = 0;
		batchResult.writeErrors = ValidateQueryDocuments(batchSpec);
		batchResponse = BuildResponseMessage(&batchResult);
	}

	Datum values[2];
	bool isNulls[2] = { false, false };

	/* The second value is true if we had any writeErrors set */
	bson_iter_t writeErrorsIter;
	bool hasWriteErrors = PgbsonInitIteratorAtPath(batchResponse, "writeErrors",
												   &writeErrorsIter);

	values[0] = PointerGetDatum(batchResponse);
	values[1] = BoolGetDatum(!hasWriteErrors);
	HeapTuple resultTuple = heap_form_tuple(resultTupDesc, values, isNulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(resultTuple));
}


/*
 * BuildBatchDeletionSpec validates the delete command BSON and builds
 * a BatchDeletionSpec.
 */
static BatchDeletionSpec *
BuildBatchDeletionSpec(bson_iter_t *deleteCommandIter, pgbsonsequence *deleteDocs)
{
	const char *collectionName = NULL;
	bson_value_t deletions = { 0 };
	bson_value_t let = { 0 };
	bool isOrdered = true;
	bool hasDeletes = false;

	while (bson_iter_next(deleteCommandIter))
	{
		const char *field = bson_iter_key(deleteCommandIter);

		if (strcmp(field, "delete") == 0)
		{
			if (!BSON_ITER_HOLDS_UTF8(deleteCommandIter))
			{
				ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE),
								errmsg("collection name has invalid type %s",
									   BsonIterTypeName(deleteCommandIter))));
			}

			collectionName = bson_iter_utf8(deleteCommandIter, NULL);
		}
		else if (strcmp(field, "deletes") == 0)
		{
			EnsureTopLevelFieldType("delete.deletes", deleteCommandIter, BSON_TYPE_ARRAY);

			if (deleteDocs != NULL)
			{
				ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_FAILEDTOPARSE),
								errmsg("Unexpected additional deletes")));
			}

			deletions = *bson_iter_value(deleteCommandIter);
			hasDeletes = true;
		}
		else if (strcmp(field, "ordered") == 0)
		{
			EnsureTopLevelFieldType("delete.ordered", deleteCommandIter, BSON_TYPE_BOOL);

			isOrdered = bson_iter_bool(deleteCommandIter);
		}
		else if (strcmp(field, "maxTimeMS") == 0)
		{
			EnsureTopLevelFieldIsNumberLike("delete.maxTimeMS", bson_iter_value(
												deleteCommandIter));
			SetExplicitStatementTimeout(BsonValueAsInt32(bson_iter_value(
															 deleteCommandIter)));
		}
		else if (EnableVariablesSupportForWriteCommands && strcmp(field, "let") == 0)
		{
			ReportFeatureUsage(FEATURE_LET_TOP_LEVEL);
			bool hasValue = EnsureTopLevelFieldTypeNullOkUndefinedOK("let",
																	 deleteCommandIter,
																	 BSON_TYPE_DOCUMENT);
			if (hasValue)
			{
				let = *bson_iter_value(deleteCommandIter);
			}
		}
		else if (IsCommonSpecIgnoredField(field))
		{
			elog(DEBUG1, "Unrecognized command field: delete.%s", field);

			/*
			 *  Silently ignore now, so that clients don't break
			 *  TODO: implement me
			 *      writeConcern
			 */
		}
		else
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_UNKNOWNBSONFIELD),
							errmsg("BSON field 'delete.%s' is an unknown field",
								   field)));
		}
	}

	if (collectionName == NULL)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_LOCATION40414),
						errmsg("BSON field 'delete.delete' is missing but "
							   "a required field")));
	}

	if (deleteDocs != NULL)
	{
		hasDeletes = true;
	}

	if (!hasDeletes)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_LOCATION40414),
						errmsg("BSON field 'delete.deletes' is missing but "
							   "a required field")));
	}

	BatchDeletionSpec *batchSpec = palloc0(sizeof(BatchDeletionSpec));

	batchSpec->collectionName = (char *) collectionName;
	batchSpec->deletionValue = deletions;
	batchSpec->isOrdered = isOrdered;
	batchSpec->deletionSequence = deleteDocs;

	/* parse and set let and time system variables */
	if (EnableVariablesSupportForWriteCommands)
	{
		TimeSystemVariables *timeSysVars = NULL;
		bool isWriteCommand = true;
		pgbson *parsedVariables = ParseAndGetTopLevelVariableSpec(&let, timeSysVars,
																  isWriteCommand);
		batchSpec->variableSpec = ConvertPgbsonToBsonValue(parsedVariables);
	}

	return batchSpec;
}


/*
 * BuildDeletionSpecList iterates over an array of delete operations and
 * builds a Deletion for each object.
 */
static List *
BuildDeletionSpecList(bson_iter_t *deleteArrayIter, const bson_value_t *variableSpec)
{
	List *deletions = NIL;

	while (bson_iter_next(deleteArrayIter))
	{
		StringInfo fieldNameStr = makeStringInfo();
		int arrIdx = list_length(deletions);
		appendStringInfo(fieldNameStr, "delete.deletes.%d", arrIdx);

		EnsureTopLevelFieldType(fieldNameStr->data, deleteArrayIter, BSON_TYPE_DOCUMENT);

		bson_iter_t deleteOperationIter;
		bson_iter_recurse(deleteArrayIter, &deleteOperationIter);

		DeletionSpec *deletion = BuildDeletionSpec(&deleteOperationIter, variableSpec);

		deletions = lappend(deletions, deletion);
	}

	return deletions;
}


/*
 * Given a lazily initialized BatchDeletionSpec processes it into the
 * List of deleteSpecs needed for processing the deletes.
 */
static void
PostProcessDeleteBatchSpec(BatchDeletionSpec *spec)
{
	if (spec->deletionValue.value_type != BSON_TYPE_EOD)
	{
		bson_iter_t deletionIter;
		BsonValueInitIterator(&spec->deletionValue, &deletionIter);
		spec->deletionsProcessed = BuildDeletionSpecList(&deletionIter,
														 &spec->variableSpec);
	}
	else
	{
		spec->deletionsProcessed = BuildDeletionSpecListFromSequence(
			spec->deletionSequence, &spec->variableSpec);
	}

	int deletionCount = list_length(spec->deletionsProcessed);
	if (deletionCount == 0 || deletionCount > MaxWriteBatchSize)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_INVALIDLENGTH),
						errmsg("Write batch sizes must be between 1 and %d. "
							   "Got %d operations.", MaxWriteBatchSize, deletionCount)));
	}
}


/*
 * BuildDeletionSpecFromSequence builds a list of DeletionSpec from a BsonSequence.
 */
static List *
BuildDeletionSpecListFromSequence(pgbsonsequence *sequence,
								  const bson_value_t *variableSpec)
{
	List *deletions = NIL;

	List *documents = PgbsonSequenceGetDocumentBsonValues(sequence);
	ListCell *documentCell;
	foreach(documentCell, documents)
	{
		bson_iter_t deleteOperationIter;
		BsonValueInitIterator(lfirst(documentCell), &deleteOperationIter);

		DeletionSpec *deletion = BuildDeletionSpec(&deleteOperationIter,
												   variableSpec);

		deletions = lappend(deletions, deletion);
	}

	return deletions;
}


/*
 * BuildDeletionSpec builds a DeletionSpec from the BSON of a single delete
 * operation.
 */
static DeletionSpec *
BuildDeletionSpec(bson_iter_t *deletionIter, const bson_value_t *variableSpec)
{
	bson_value_t *query = NULL;
	int64 limit = -1;

	char collationString[MAX_ICU_COLLATION_LENGTH] = "\0";
	while (bson_iter_next(deletionIter))
	{
		const char *field = bson_iter_key(deletionIter);

		if (strcmp(field, "q") == 0)
		{
			EnsureTopLevelFieldType("delete.deletes.q", deletionIter, BSON_TYPE_DOCUMENT);

			query = CreateBsonValueCopy(bson_iter_value(deletionIter));
		}
		else if (strcmp(field, "limit") == 0)
		{
			if (!BSON_ITER_HOLDS_NUMBER(deletionIter))
			{
				/* we treats arbitrary types as valid limit 0 */
				limit = 0;
			}
			else
			{
				limit = bson_iter_as_int64(deletionIter);
				if (limit != 0 && limit != 1)
				{
					ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_FAILEDTOPARSE),
									errmsg("The limit field in delete objects must be 0 "
										   "or 1. Got " INT64_FORMAT, limit)));
				}
			}
		}
		else if (strcmp(field, "collation") == 0)
		{
			ReportFeatureUsage(FEATURE_COLLATION);

			if (EnableCollation)
			{
				EnsureTopLevelFieldType("delete.collation", deletionIter,
										BSON_TYPE_DOCUMENT);

				const bson_value_t *collationValue = bson_iter_value(deletionIter);
				ParseAndGetCollationString(collationValue,
										   collationString);
			}
			else
			{
				ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_COMMANDNOTSUPPORTED),
								errmsg("BSON field 'delete.deletes.collation' is not yet "
									   "supported")));
			}
		}
		else if (strcmp(field, "hint") == 0)
		{
			/* We ignore this for now (TODO Support this?) */
			continue;
		}
		else if (strcmp(field, "comment") == 0)
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_COMMANDNOTSUPPORTED),
							errmsg("BSON field 'delete.deletes.comment' is not yet "
								   "supported")));
		}
		else
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_UNKNOWNBSONFIELD),
							errmsg("BSON field 'delete.deletes.%s' is an unknown field",
								   field)));
		}
	}

	if (query == NULL)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_LOCATION40414),
						errmsg("BSON field 'delete.deletes.q' is missing but "
							   "a required field")));
	}

	if (limit == -1)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_LOCATION40414),
						errmsg("BSON field 'delete.deletes.limit' is missing but "
							   "a required field")));
	}

	DeletionSpec *deletionSpec = palloc0(sizeof(DeletionSpec));
	deletionSpec->limit = limit;
	deletionSpec->deleteOneParams.query = query;
	deletionSpec->deleteOneParams.variableSpec = variableSpec;

	strlcpy((char *) deletionSpec->deleteOneParams.collationString, collationString,
			strlen(collationString) + 1);

	return deletionSpec;
}


/*
 * ProcessBatchDeletion iterates over the deletes array and executes each
 * deletion in a subtransaction, to allow us to continue after an error.
 *
 * If batchSpec->isOrdered is false, we continue with remaining tasks an
 * error.
 *
 * We Use subtransactions which effectively
 * does each delete operation in a separate transaction.
 */
static void
ProcessBatchDeletion(MongoCollection *collection, BatchDeletionSpec *batchSpec,
					 bool forceInline, text *transactionId,
					 BatchDeletionResult *batchResult)
{
	PostProcessDeleteBatchSpec(batchSpec);
	List *deletions = batchSpec->deletionsProcessed;
	bool isOrdered = batchSpec->isOrdered;

	/*
	 * Execute the query inside a sub-transaction, so we can restore order
	 * after a failure.
	 */
	MemoryContext oldContext = CurrentMemoryContext;
	ResourceOwner oldOwner = CurrentResourceOwner;

	batchResult->ok = 1;
	batchResult->rowsDeleted = 0;
	batchResult->writeErrors = NIL;

	/* declared volatile because of the longjmp in PG_CATCH */
	volatile int deleteIndex = 0;

	ListCell *deletionCell = NULL;
	foreach(deletionCell, deletions)
	{
		CHECK_FOR_INTERRUPTS();

		DeletionSpec *deletionSpec = lfirst(deletionCell);

		/* declared volatile because of the longjmp in PG_CATCH */
		volatile uint64 rowsDeleted = 0;
		volatile bool isSuccess = false;

		/* use a subtransaction to correctly handle failures */
		BeginInternalSubTransaction(NULL);

		PG_TRY();
		{
			rowsDeleted = ProcessDeletion(collection, deletionSpec, forceInline,
										  transactionId);

			/* Commit the inner transaction, return to outer xact context */
			ReleaseCurrentSubTransaction();
			MemoryContextSwitchTo(oldContext);
			CurrentResourceOwner = oldOwner;

			isSuccess = true;
		}
		PG_CATCH();
		{
			MemoryContextSwitchTo(oldContext);
			ErrorData *errorData = CopyErrorDataAndFlush();

			/* Abort the inner transaction */
			RollbackAndReleaseCurrentSubTransaction();
			MemoryContextSwitchTo(oldContext);
			CurrentResourceOwner = oldOwner;

			if (IsOperatorInterventionError(errorData))
			{
				ReThrowError(errorData);
			}

			batchResult->writeErrors = lappend(batchResult->writeErrors,
											   GetWriteErrorFromErrorData(errorData,
																		  deleteIndex));

			isSuccess = false;
		}
		PG_END_TRY();

		if (!isSuccess && isOrdered)
		{
			/* stop trying delete operations after a failure if using ordered:true */
			break;
		}

		batchResult->rowsDeleted += rowsDeleted;
		deleteIndex++;
	}
}


static pgbson *
ProcessBatchDeleteUnsharded(MongoCollection *collection, BatchDeletionSpec *batchSpec,
							text *transactionId)
{
	Assert(collection->shardKey == NULL);

	pgbson *deleteWorkerSpecs = SerializeDeleteWorkerSpecForUnsharded(batchSpec);

	/* since this is unsharded, the keyHash is just the collection id. */
	int shardKeyHash = collection->collectionId;
	return CallDeleteWorker(collection, deleteWorkerSpecs,
							shardKeyHash, transactionId, batchSpec->deletionSequence);
}


/*
 * ProcessDeletion processes a single deletion operation defined in
 * deletionSpec on the given collection.
 */
static uint64
ProcessDeletion(MongoCollection *collection, DeletionSpec *deletionSpec,
				bool forceInlineWrites, text *transactionId)
{
	if (deletionSpec->deleteOneParams.returnDeletedDocument)
	{
		ereport(ERROR, (errmsg("cannot return deleted document via "
							   "regular delete")));
	}

	pgbson *query = PgbsonInitFromDocumentBsonValue(deletionSpec->deleteOneParams.query);

	/* determine whether query filters by a single shard key value */
	int64 shardKeyHash = 0;

	/* if the collection is sharded, check whether we can use a single hash value */
	bool isShardKeyValueCollationAware = false;
	bool hasShardKeyValueFilter =
		ComputeShardKeyHashForQuery(collection->shardKey, collection->collectionId, query,
									&shardKeyHash, &isShardKeyValueCollationAware);

	/* determine whether query filters by a single object ID */
	bson_iter_t queryDocIter;
	PgbsonInitIterator(query, &queryDocIter);

	bson_value_t idFromQueryDocument = { 0 };
	bool errorOnConflict = false;
	bool queryHasNonIdFilters = false;
	bool isIdFilterCollationAware = false;
	bool hasObjectIdFilter =
		TraverseQueryDocumentAndGetId(&queryDocIter, &idFromQueryDocument,
									  errorOnConflict, &queryHasNonIdFilters,
									  &isIdFilterCollationAware);

	uint64_t result = 0;
	const char *collationString = deletionSpec->deleteOneParams.collationString;
	bool applyCollationToShardKeyValue = IsCollationApplicable(collationString) &&
										 isShardKeyValueCollationAware;
	if (deletionSpec->limit == 0)
	{
		if (applyCollationToShardKeyValue)
		{
			/* if shard key value is collation sensitive, we distribute the query */
			/* by omitting the shard key value filter */
			hasShardKeyValueFilter = false;
		}

		/*
		 * Delete as many document as match the query. This is not a retryable
		 * operation, so we ignore transactionId.
		 */
		result = DeleteAllMatchingDocuments(collection, query,
											deletionSpec->deleteOneParams.variableSpec,
											collationString, hasShardKeyValueFilter,
											shardKeyHash);
	}
	else
	{
		DeleteOneResult deleteOneResult = { 0 };

		/* With limit = 1, we currently target only one shard for the deletion. */
		/* If the shard key value is collation-sensitive, we cannot target a single */
		/* shard with it.*/
		/* We then fall on any _id value filter. If none is provided, we fail. */
		bool useShardKeyValueFilter = hasShardKeyValueFilter &&
									  !applyCollationToShardKeyValue;
		if (useShardKeyValueFilter)
		{
			/*
			 * Delete at most 1 document that matches the query on a single shard.
			 *
			 * For unsharded collection, this is the shard that contains all the
			 * data.
			 */
			CallDeleteOne(collection, &deletionSpec->deleteOneParams, shardKeyHash,
						  transactionId, forceInlineWrites, &deleteOneResult);
		}
		else if (hasObjectIdFilter)
		{
			/*
			 * Delete at most 1 document that matches an _id equality filter from
			 * a sharded collection without specifying a a shard key filter.
			 */
			DeleteOneObjectId(collection, &deletionSpec->deleteOneParams,
							  &idFromQueryDocument, isIdFilterCollationAware,
							  queryHasNonIdFilters, forceInlineWrites,
							  transactionId, &deleteOneResult);
		}
		else
		{
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("delete query with limit 1 must include either "
								   "_id or%s shard key filter",
								   isShardKeyValueCollationAware ?
								   " collation-insensitive" : "")));
		}

		result = deleteOneResult.isRowDeleted ? 1 : 0;
	}

	pfree(query);
	return result;
}


/*
 * DeleteAllMatchingDocuments deletes all documents that match the query.
 */
static uint64
DeleteAllMatchingDocuments(MongoCollection *collection, pgbson *queryDoc,
						   const bson_value_t *variableSpec,
						   const char *collationString, bool hasShardKeyValueFilter,
						   int64 shardKeyHash)
{
	uint64 collectionId = collection->collectionId;

	bool applyCollation = IsCollationApplicable(collationString);

	bool queryHasNonIdFilters = false;
	bool isIdFilterCollationAware = false;
	pgbson *objectIdFilter = GetObjectIdFilterFromQueryDocument(queryDoc,
																&queryHasNonIdFilters,
																&isIdFilterCollationAware);
	bool applyObjectIdFilter = objectIdFilter != NULL;
	if (applyObjectIdFilter && applyCollation)
	{
		/* if the _id filter value is collation-sensitive, we will omit */
		/* filtering by _id in the WHERE clause. */
		applyObjectIdFilter = !isIdFilterCollationAware;
	}

	int argCount = 0;
	int nextSqlArgIndex = 1;
	uint64 rowsDeleted = 0;
	StringInfoData deleteQuery;

	SPI_connect();

	initStringInfo(&deleteQuery);
	appendStringInfo(&deleteQuery, "DELETE FROM ");

	if (collection->shardTableName[0] != '\0')
	{
		appendStringInfo(&deleteQuery, " %s.%s", ApiDataSchemaName,
						 collection->shardTableName);
	}
	else
	{
		appendStringInfo(&deleteQuery, " %s.documents_" UINT64_FORMAT, ApiDataSchemaName,
						 collectionId);
	}

	pgbson *variableSpecBson = NULL;
	if (EnableVariablesSupportForWriteCommands && queryHasNonIdFilters)
	{
		variableSpecBson = variableSpec != NULL &&
						   variableSpec->value_type == BSON_TYPE_DOCUMENT ?
						   PgbsonInitFromDocumentBsonValue(variableSpec) : NULL;
	}

	bool applyVariableSpec = variableSpecBson != NULL;
	if (applyVariableSpec || applyCollation)
	{
		/* utilize the collation and/or variables in matching the document */
		appendStringInfo(&deleteQuery,
						 " WHERE %s.bson_query_match(document, $1::%s.bson, $2::%s.bson, $3::text)",
						 DocumentDBApiInternalSchemaName, CoreSchemaName, CoreSchemaName);

		argCount += 3;
		nextSqlArgIndex = 4;
	}
	else
	{
		appendStringInfo(&deleteQuery,
						 " WHERE document OPERATOR(%s.@@) $1::%s",
						 ApiCatalogSchemaName, FullBsonTypeName);

		argCount++;
		nextSqlArgIndex = 2;
	}

	uint64 planId = (applyVariableSpec || applyCollation) ?
					QUERY_DELETE_WITH_FILTER_LET_AND_COLLATION :
					QUERY_DELETE_WITH_FILTER;

	int shardKeyArgIndex = -1;
	if (hasShardKeyValueFilter)
	{
		planId = (applyVariableSpec || applyCollation) ?
				 QUERY_DELETE_WITH_FILTER_SHARDKEY_LET_AND_COLLATION :
				 QUERY_DELETE_WITH_FILTER_SHARDKEY;
		appendStringInfo(&deleteQuery, " AND shard_key_value = $%d", nextSqlArgIndex);

		argCount++;
		shardKeyArgIndex = nextSqlArgIndex - 1;
		nextSqlArgIndex++;
	}

	int objectIdArgIndex = -1;
	if (applyObjectIdFilter)
	{
		if (hasShardKeyValueFilter)
		{
			planId = (applyVariableSpec || applyCollation) ?
					 QUERY_DELETE_WITH_FILTER_SHARDKEY_ID_LET_AND_COLLATION :
					 QUERY_DELETE_WITH_FILTER_SHARDKEY_ID;

			appendStringInfo(&deleteQuery,
							 " AND object_id OPERATOR(%s.=) $%d::%s",
							 CoreSchemaName, nextSqlArgIndex, FullBsonTypeName);
		}
		else
		{
			planId = (applyVariableSpec || applyCollation) ?
					 QUERY_DELETE_WITH_FILTER_ID_LET_AND_COLLATION :
					 QUERY_DELETE_WITH_FILTER_ID;

			appendStringInfo(&deleteQuery,
							 " AND object_id OPERATOR(%s.=) $%d::%s",
							 CoreSchemaName, nextSqlArgIndex, FullBsonTypeName);
		}

		argCount++;
		objectIdArgIndex = nextSqlArgIndex - 1;
		nextSqlArgIndex++;
	}

	Datum *argValues = palloc0(sizeof(Datum) * argCount);
	Oid *argTypes = palloc0(sizeof(Oid) * argCount);
	char *argNulls = palloc0(sizeof(char) * argCount);

	/* set query value */
	Oid bsonTypeId = BsonTypeId();
	argTypes[0] = bsonTypeId;
	argValues[0] = PointerGetDatum(queryDoc);
	argNulls[0] = ' ';

	if (applyVariableSpec || applyCollation)
	{
		/* set the variable spec */
		argTypes[1] = bsonTypeId;
		argValues[1] = applyVariableSpec ? PointerGetDatum(variableSpecBson) : (Datum) 0;
		argNulls[1] = applyVariableSpec ? ' ' : 'n';

		/* set the collation string */
		argTypes[2] = TEXTOID;
		argValues[2] = applyCollation ? CStringGetTextDatum(collationString) : (Datum) 0;
		argNulls[2] = applyCollation ? ' ' : 'n';
	}

	/* set shard key value */
	if (shardKeyArgIndex != -1)
	{
		argTypes[shardKeyArgIndex] = INT8OID;
		argValues[shardKeyArgIndex] = Int64GetDatum(shardKeyHash);
		argNulls[shardKeyArgIndex] = ' ';
	}

	/* set object id value */
	if (objectIdArgIndex != -1)
	{
		argTypes[objectIdArgIndex] = BYTEAOID;
		argValues[objectIdArgIndex] = PointerGetDatum(CastPgbsonToBytea(objectIdFilter));
		argNulls[objectIdArgIndex] = ' ';
	}

	bool readOnly = false;
	long maxTupleCount = 0;
	SPIPlanPtr plan = GetSPIQueryPlanWithLocalShard(collectionId,
													collection->shardTableName, planId,
													deleteQuery.data, argTypes, argCount);

	SPI_execute_plan(plan, argValues, argNulls, readOnly, maxTupleCount);
	rowsDeleted = SPI_processed;

	pfree(deleteQuery.data);

	SPI_finish();

	return rowsDeleted;
}


void
CallDeleteOne(MongoCollection *collection, DeleteOneParams *deleteOneParams,
			  int64 shardKeyHash, text *transactionId, bool forceInlineWrites,
			  DeleteOneResult *result)
{
	/* In single node scenarios (like DocumentDB where we can inline the write, call the internal)
	 * delete functions directly.
	 * Alternatively in a distributed scenario, if the shard is colocated on the current node anyway,
	 * then we don't need to go remote - we can simply call the delete internal functions directly.
	 */
	if (DefaultInlineWriteOperations || collection->shardTableName[0] != '\0' ||
		forceInlineWrites)
	{
		DeleteOneInternalCore(collection, shardKeyHash, deleteOneParams,
							  transactionId, result);
	}
	else
	{
		/*
		 * If the cluster supports it, and we need to go remote, call the update worker
		 * function with the appropriate spec args.
		 */
		pgbsonsequence *docSequence = NULL;
		pgbson *workerResult = CallDeleteWorker(collection,
												SerializeDeleteOneParams(deleteOneParams),
												shardKeyHash, transactionId, docSequence);
		DeserializeWorkerDeleteResultForDeleteOne(workerResult, result);
	}
}


static pgbson *
CallDeleteWorker(MongoCollection *collection,
				 pgbson *serializedDeleteSpec,
				 int64 shardKeyHash,
				 text *transactionId,
				 pgbsonsequence *sequence)
{
	int argCount = 6;
	Datum argValues[6];

	/* whitespace means not null, n means null */
	char argNulls[6] = { ' ', ' ', ' ', ' ', 'n', 'n' };
	Oid argTypes[6] = { INT8OID, INT8OID, REGCLASSOID, BYTEAOID, BYTEAOID, TEXTOID };

	const char *updateQuery = FormatSqlQuery(
		"SELECT %s.delete_worker($1, $2, $3, $4::%s.bson, $5::%s.bsonsequence, $6) FROM %s.documents_"
		UINT64_FORMAT " WHERE shard_key_value = %ld",
		DocumentDBApiInternalSchemaName, CoreSchemaNameV2, CoreSchemaNameV2,
		ApiDataSchemaName, collection->collectionId,
		shardKeyHash);

	argValues[0] = UInt64GetDatum(collection->collectionId);

	/* p_shard_key_value */
	argValues[1] = Int64GetDatum(shardKeyHash);

	/* p_shard_oid */
	argValues[2] = ObjectIdGetDatum(InvalidOid);

	argValues[3] = PointerGetDatum(serializedDeleteSpec);

	if (sequence != NULL)
	{
		argValues[4] = PointerGetDatum(sequence);
		argNulls[4] = ' ';
	}

	if (transactionId != NULL)
	{
		argValues[5] = PointerGetDatum(transactionId);
		argNulls[5] = ' ';
	}

	bool readOnly = false;

	Datum resultDatum[1] = { 0 };
	bool isNulls[1] = { false };
	int numResults = 1;

	/* forceDelegation assumes nested distribution */
	RunMultiValueQueryWithNestedDistribution(updateQuery, argCount, argTypes, argValues,
											 argNulls,
											 readOnly, SPI_OK_SELECT, resultDatum,
											 isNulls, numResults);

	if (isNulls[0])
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_INTERNALERROR),
						errmsg("delete_worker should not return null")));
	}
	pgbson *resultPgbson = DatumGetPgBson(resultDatum[0]);

	return resultPgbson;
}


static void
DeleteOneInternalCore(MongoCollection *collection, int64 shardKeyHash,
					  DeleteOneParams *deleteOneParams,
					  text *transactionId, DeleteOneResult *deleteOneResult)
{
	if (transactionId != NULL)
	{
		/* transaction ID specified, use retryable write path */
		RetryableWriteResult writeResult;

		/*
		 * If a retry record exists, delete it since only a single retry is allowed.
		 */
		if (DeleteRetryRecord(collection->collectionId, shardKeyHash, transactionId,
							  &writeResult))
		{
			/*
			 * Get rows affected from the retry record.
			 */
			deleteOneResult->isRowDeleted = writeResult.rowsAffected > 0;

			deleteOneResult->resultDeletedDocument = writeResult.resultDocument;
		}
		else
		{
			/*
			 * No retry record exists, delete the row and get the object ID.
			 */
			DeleteOneInternal(collection, deleteOneParams, shardKeyHash,
							  deleteOneResult);

			/*
			 * Remember that we performed a retryable write with the given
			 * transaction ID.
			 */
			InsertRetryRecord(collection->collectionId, shardKeyHash, transactionId,
							  deleteOneResult->objectId, deleteOneResult->isRowDeleted,
							  deleteOneResult->resultDeletedDocument);
		}
	}
	else
	{
		/*
		 * No transaction ID specified, do regular delete.
		 */
		DeleteOneInternal(collection, deleteOneParams, shardKeyHash, deleteOneResult);
	}
}


/*
 * command_delete_one handles a single deletion on a shard.
 */
Datum
command_delete_one(PG_FUNCTION_ARGS)
{
	ereport(ERROR, (errmsg("This function is deprecated and should not be called")));
}


Datum
command_delete_worker(PG_FUNCTION_ARGS)
{
	uint64 collectionId = PG_GETARG_INT64(0);
	int64 shardKeyHash = PG_GETARG_INT64(1);
	Oid shardOid = PG_GETARG_OID(2);

	pgbson *deleteInternalSpec = PG_GETARG_PGBSON_PACKED(3);

	if (shardOid == InvalidOid)
	{
		/* The planner is expected to replace this */
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_INTERNALERROR),
						errmsg("Explicit shardOid must be set - this is a server bug"),
						errdetail_log(
							"Explicit shardOid must be set - this is a server bug")));
	}

	pgbsonsequence *specDocuments = PG_GETARG_MAYBE_NULL_PGBSON_SEQUENCE(4);
	text *transactionId = PG_ARGISNULL(5) ? NULL : PG_GETARG_TEXT_PP(5);

	ThrowIfServerOrTransactionReadOnly();
	AllowNestedDistributionInCurrentTransaction();
	pgbsonelement commandElement;
	PgbsonToSinglePgbsonElement(deleteInternalSpec, &commandElement);

	pgbson *serializedResult;
	if (strcmp(commandElement.path, "deleteOne") == 0)
	{
		DeleteOneParams deleteOneParams = { 0 };
		DeserializeDeleteWorkerSpecForDeleteOne(&commandElement.bsonValue,
												&deleteOneParams);

		DeleteOneResult result;
		memset(&result, 0, sizeof(result));

		MongoCollection mongoCollection = { 0 };
		UpdateMongoCollectionUsingIds(&mongoCollection, collectionId, shardOid);

		DeleteOneInternalCore(&mongoCollection, shardKeyHash, &deleteOneParams,
							  transactionId,
							  &result);

		serializedResult = SerializeDeleteOneResult(&result);
	}
	else if (strcmp(commandElement.path, "deleteUnsharded") == 0)
	{
		BatchDeletionSpec batchDeletionSpec = { 0 };
		BatchDeletionResult result = { 0 };

		DeserializeDeleteWorkerSpecForUnsharded(&commandElement.bsonValue,
												&batchDeletionSpec);
		batchDeletionSpec.deletionSequence = specDocuments;

		MongoCollection mongoCollection = { 0 };
		UpdateMongoCollectionUsingIds(&mongoCollection, collectionId, InvalidOid);

		bool forceInline = true;
		ProcessBatchDeletion(&mongoCollection, &batchDeletionSpec, forceInline,
							 transactionId, &result);
		serializedResult = BuildResponseMessage(&result);
	}
	else
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_INTERNALERROR),
						errmsg(
							"Delete worker only supports deleteOne or deleteUnsharded call")));
	}

	PG_RETURN_POINTER(serializedResult);
}


/*
 * DeleteOneInternal deletes a single row with a specific shard key value filter.
 *
 * Returns 1 if a row was deleted, and 0 if no row matched the query.
 */
static void
DeleteOneInternal(MongoCollection *collection, DeleteOneParams *deleteOneParams,
				  int64 shardKeyHash, DeleteOneResult *result)
{
	uint64 planId = QUERY_DELETE_ONE;
	bool applyCollation = IsCollationApplicable(deleteOneParams->collationString);

	List *sortFieldDocuments = deleteOneParams->sort == NULL ? NIL :
							   BsonValueDocumentDecomposeFields(deleteOneParams->sort);
	int sortFieldDocumentsLength = list_length(sortFieldDocuments);

	int argCount = sortFieldDocumentsLength;

	pgbson *query = PgbsonInitFromDocumentBsonValue(deleteOneParams->query);
	bool queryHasNonIdFilters = false;
	bool isIdFilterCollationAware = false;
	pgbson *objectIdFilter = GetObjectIdFilterFromQueryDocument(query,
																&queryHasNonIdFilters,
																&isIdFilterCollationAware);

	bool applyObjectIdFilter = objectIdFilter != NULL;
	if (applyObjectIdFilter && applyCollation)
	{
		/* if the _id filter value is collation-sensitive, we will omit */
		/* filtering by _id in the WHERE clause. */
		applyObjectIdFilter = !isIdFilterCollationAware;
	}

	argCount += applyObjectIdFilter ? 1 : 0;

	int nextSqlArgIndex = 1;
	MemoryContext outerContext = CurrentMemoryContext;

	SPI_connect();

	/*
	 * We construct a query that the distribution layer can route to a single shard, which
	 * also allows us to use LIMIT 1 and FOR UPDATE in a subquery.
	 *
	 * The LIMIT 1 ensures we only delete a single row even if there are many
	 * that match the query.
	 *
	 * The FOR UPDATE ensures that the matching row does not change or disappear
	 * concurrently. Otherwise, the DELETE might incorrectly become a noop.
	 *
	 * Note that we cannot directly place the SELECT query into the USING clause
	 * due to the reason discussed in the pgsql-bug thread:
	 * https://www.postgresql.org/message-id/3798786.1655133396%40sss.pgh.pa.us.
	 * For this reason, here we use a materialized cte to compute the ctid of the
	 * tuple that needs to be deleted.
	 */
	StringInfoData selectQuery;
	initStringInfo(&selectQuery);
	appendStringInfo(&selectQuery, "WITH s AS MATERIALIZED (SELECT ctid FROM ");

	if (collection->shardTableName[0] != '\0')
	{
		appendStringInfo(&selectQuery, " %s.%s", ApiDataSchemaName,
						 collection->shardTableName);
	}
	else
	{
		appendStringInfo(&selectQuery, " %s.documents_" UINT64_FORMAT, ApiDataSchemaName,
						 collection->collectionId);
	}

	appendStringInfo(&selectQuery, " WHERE shard_key_value = $1 AND");
	nextSqlArgIndex++;
	argCount++;

	const bson_value_t *variableSpec = deleteOneParams->variableSpec;
	pgbson *variableSpecBson = NULL;
	if (EnableVariablesSupportForWriteCommands && queryHasNonIdFilters)
	{
		variableSpecBson = variableSpec != NULL &&
						   variableSpec->value_type == BSON_TYPE_DOCUMENT ?
						   PgbsonInitFromDocumentBsonValue(variableSpec) : NULL;
	}

	bool applyVariableSpec = variableSpecBson != NULL;
	if (applyVariableSpec || applyCollation)
	{
		planId = QUERY_DELETE_ONE_LET_AND_COLLATION;

		/* utilize the collation and/or variables in matching the document */
		appendStringInfo(&selectQuery,
						 " %s.bson_query_match(document, $2, $3, $4)",
						 ApiInternalSchemaNameV2);

		nextSqlArgIndex += 3;
		argCount += 3;
	}
	else
	{
		appendStringInfo(&selectQuery,
						 " document OPERATOR(%s.@@) $2::%s",
						 ApiCatalogSchemaName, FullBsonTypeName);

		nextSqlArgIndex += 1;
		argCount += 1;
	}

	int objectIdArgIndex = -1;
	if (applyObjectIdFilter)
	{
		planId = (applyVariableSpec || applyCollation) ?
				 QUERY_DELETE_ONE_ID_LET_AND_COLLATION :
				 QUERY_DELETE_ONE_ID;

		appendStringInfo(&selectQuery,
						 " AND object_id OPERATOR(%s.=) $%d::%s",
						 CoreSchemaName, nextSqlArgIndex, FullBsonTypeName);

		objectIdArgIndex = nextSqlArgIndex - 1;
		nextSqlArgIndex++;
		argCount++;
	}

	Datum *argValues = palloc0(sizeof(Datum) * argCount);
	Oid *argTypes = palloc0(sizeof(Oid) * argCount);
	char *argNulls = palloc0(sizeof(char) * argCount);

	memset(argNulls, 'n', argCount);

	/* set shard key value */
	argTypes[0] = INT8OID;
	argValues[0] = Int64GetDatum(shardKeyHash);
	argNulls[0] = ' ';

	/* set query value*/
	Oid bsonTypeId = BsonTypeId();
	argTypes[1] = bsonTypeId;
	argValues[1] = PointerGetDatum(query);
	argNulls[1] = ' ';

	if (applyVariableSpec || applyCollation)
	{
		/* set the variable spec */
		argTypes[2] = bsonTypeId;
		argValues[2] = PointerGetDatum(variableSpecBson);
		argNulls[2] = applyVariableSpec ? ' ' : 'n';

		/* TODO: set the collation string */
		argTypes[3] = TEXTOID;
		argValues[3] = CStringGetTextDatum(deleteOneParams->collationString);
		argNulls[3] = applyCollation ? ' ' : 'n';
	}

	/* set id filter value */
	if (objectIdArgIndex != -1)
	{
		argTypes[objectIdArgIndex] = BYTEAOID;
		argValues[objectIdArgIndex] = PointerGetDatum(CastPgbsonToBytea(objectIdFilter));
		argNulls[objectIdArgIndex] = ' ';
	}

	/* set sort values */
	if (sortFieldDocumentsLength > 0)
	{
		appendStringInfoString(&selectQuery, " ORDER BY");

		int sortItemSqlArgBaseIndex = nextSqlArgIndex;
		for (int i = 0; i < sortFieldDocumentsLength; i++)
		{
			pgbson *sortDoc = list_nth(sortFieldDocuments, i);
			bool isAscending = ValidateOrderbyExpressionAndGetIsAscending(sortDoc);

			int sqlArgPosition = i + sortItemSqlArgBaseIndex;

			if (applyCollation)
			{
				appendStringInfo(&selectQuery,
								 "%s %s.bson_orderby(document, $%d::%s.bson, $4) USING OPERATOR(%s.%s)",
								 i > 0 ? "," : "", ApiInternalSchemaNameV2,
								 sqlArgPosition, CoreSchemaNameV2,
								 ApiInternalSchemaNameV2, isAscending ? "<<<" : ">>>");
			}
			else
			{
				appendStringInfo(&selectQuery,
								 "%s %s.bson_orderby(document, $%d) %s",
								 i > 0 ? "," : "", ApiCatalogSchemaName,
								 sqlArgPosition, isAscending ? "ASC" : "DESC");
			}

			argTypes[sqlArgPosition - 1] = BsonTypeId();
			argValues[sqlArgPosition - 1] = PointerGetDatum(sortDoc);
			argNulls[sqlArgPosition - 1] = ' ';
		}
	}

	appendStringInfo(&selectQuery,
					 " LIMIT 1 FOR UPDATE)");

	StringInfoData deleteQuery;
	initStringInfo(&deleteQuery);
	appendStringInfo(&deleteQuery, "%s DELETE FROM", selectQuery.data);

	if (collection->shardTableName[0] != '\0')
	{
		appendStringInfo(&deleteQuery, " %s.%s", ApiDataSchemaName,
						 collection->shardTableName);
	}
	else
	{
		appendStringInfo(&deleteQuery, " %s.documents_" UINT64_FORMAT, ApiDataSchemaName,
						 collection->collectionId);
	}

	appendStringInfo(&deleteQuery,
					 " d USING s WHERE d.ctid = s.ctid AND shard_key_value = $1"
					 " RETURNING object_id");

	if (deleteOneParams->returnDeletedDocument)
	{
		planId = QUERY_DELETE_ONE_ID_RETURN_DOCUMENT;
		appendStringInfo(&deleteQuery, ", document");
	}

	bool readOnly = false;
	long maxTupleCount = 0;

	if (list_length(sortFieldDocuments) > 0)
	{
		/* we can't cache sort query */
		SPI_execute_with_args(deleteQuery.data, argCount, argTypes, argValues, argNulls,
							  readOnly, maxTupleCount);
	}
	else
	{
		SPIPlanPtr plan = GetSPIQueryPlanWithLocalShard(collection->collectionId,
														collection->shardTableName,
														planId, deleteQuery.data,
														argTypes,
														argCount);

		SPI_execute_plan(plan, argValues, argNulls, readOnly, maxTupleCount);
	}

	pfree(deleteQuery.data);
	uint64 rowsDeleted = SPI_processed;
	Assert(rowsDeleted <= 1);

	if (rowsDeleted > 0)
	{
		result->isRowDeleted = true;

		bool isNull = false;
		int columnNumber = 1;
		Datum objectIdDatum = SPI_getbinval(SPI_tuptable->vals[0],
											SPI_tuptable->tupdesc, columnNumber,
											&isNull);

		/* copy object ID into outer memory context */
		pgbson *objectId = DatumGetPgBson(objectIdDatum);
		result->objectId =
			CopyPgbsonIntoMemoryContext(objectId, outerContext);
	}
	else
	{
		/* no row matched the query */
		result->isRowDeleted = false;
		result->objectId = NULL;
	}

	if (deleteOneParams->returnDeletedDocument)
	{
		if (rowsDeleted > 0)
		{
			bool isNull = false;
			int columnNumber = 2;
			Datum documentDatum = SPI_getbinval(SPI_tuptable->vals[0],
												SPI_tuptable->tupdesc, columnNumber,
												&isNull);

			pgbson *resultDeletedDocument = DatumGetPgBson(documentDatum);

			if (deleteOneParams->returnFields)
			{
				bool forceProjectId = false;
				bool allowInclusionExclusion = false;
				bson_iter_t projectIter;
				BsonValueInitIterator(deleteOneParams->returnFields, &projectIter);

				/* no need for a variableSpec in this projection */
				pgbson *variableSpec = NULL;
				const BsonProjectionQueryState *projectionState =
					GetProjectionStateForBsonProject(&projectIter,
													 forceProjectId,
													 allowInclusionExclusion,
													 variableSpec);
				resultDeletedDocument = ProjectDocumentWithState(resultDeletedDocument,
																 projectionState);
			}

			result->resultDeletedDocument =
				CopyPgbsonIntoMemoryContext(resultDeletedDocument, outerContext);
		}
		else
		{
			result->resultDeletedDocument = NULL;
		}
	}

	SPI_finish();
}


static pgbson *
SerializeDeleteOneParams(const DeleteOneParams *deleteParams)
{
	pgbson_writer commandWriter;
	pgbson_writer writer;
	PgbsonWriterInit(&commandWriter);

	PgbsonWriterStartDocument(&commandWriter, "deleteOne", 9, &writer);

	if (deleteParams->query != NULL)
	{
		PgbsonWriterAppendValue(&writer, "query", 5, deleteParams->query);
	}

	if (deleteParams->sort != NULL)
	{
		PgbsonWriterAppendValue(&writer, "sort", 4, deleteParams->sort);
	}

	PgbsonWriterAppendBool(&writer, "returnDeletedDocument", 21,
						   deleteParams->returnDeletedDocument);

	if (deleteParams->returnFields != NULL)
	{
		PgbsonWriterAppendValue(&writer, "returnFields", 12,
								deleteParams->returnFields);
	}

	if (deleteParams->variableSpec != NULL &&
		deleteParams->variableSpec->value_type == BSON_TYPE_DOCUMENT)
	{
		PgbsonWriterAppendValue(&writer, "variableSpec", 12,
								deleteParams->variableSpec);
	}

	if (IsCollationApplicable(deleteParams->collationString))
	{
		PgbsonWriterAppendUtf8(&writer, "collation", 9,
							   deleteParams->collationString);
	}

	PgbsonWriterEndDocument(&commandWriter, &writer);
	return PgbsonWriterGetPgbson(&commandWriter);
}


static void
DeserializeDeleteWorkerSpecForDeleteOne(const bson_value_t *workerSpecValue,
										DeleteOneParams *deleteOneParams)
{
	bson_iter_t commandIter;
	BsonValueInitIterator(workerSpecValue, &commandIter);

	while (bson_iter_next(&commandIter))
	{
		const char *key = bson_iter_key(&commandIter);
		if (strcmp(key, "query") == 0)
		{
			deleteOneParams->query = CreateBsonValueCopy(bson_iter_value(
															 &commandIter));
		}
		else if (strcmp(key, "sort") == 0)
		{
			deleteOneParams->sort = CreateBsonValueCopy(bson_iter_value(
															&commandIter));
		}
		else if (strcmp(key, "returnDeletedDocument") == 0)
		{
			deleteOneParams->returnDeletedDocument = bson_iter_bool(&commandIter);
		}
		else if (strcmp(key, "returnFields") == 0)
		{
			deleteOneParams->returnFields = CreateBsonValueCopy(
				bson_iter_value(&commandIter));
		}
		else if (strcmp(key, "variableSpec") == 0)
		{
			deleteOneParams->variableSpec = CreateBsonValueCopy(
				bson_iter_value(&commandIter));
		}
		else if (EnableCollation && strcmp(key, "collation") == 0)
		{
			strlcpy((char *) deleteOneParams->collationString,
					bson_iter_utf8(&commandIter, NULL),
					sizeof(deleteOneParams->collationString));
		}
	}
}


static void
DeserializeWorkerDeleteResultForDeleteOne(pgbson *resultBson, DeleteOneResult *result)
{
	bson_iter_t deleteResultIter;
	PgbsonInitIterator(resultBson, &deleteResultIter);

	while (bson_iter_next(&deleteResultIter))
	{
		const char *key = bson_iter_key(&deleteResultIter);
		if (strcmp(key, "isRowDeleted") == 0)
		{
			result->isRowDeleted = bson_iter_bool(&deleteResultIter);
		}
		else if (strcmp(key, "objectId") == 0)
		{
			result->objectId = PgbsonInitFromDocumentBsonValue(bson_iter_value(
																   &deleteResultIter));
		}
		else if (strcmp(key, "resultDeletedDocument") == 0)
		{
			result->resultDeletedDocument = PgbsonInitFromDocumentBsonValue(
				bson_iter_value(&deleteResultIter));
		}
	}
}


static pgbson *
SerializeDeleteOneResult(DeleteOneResult *result)
{
	pgbson_writer writer;
	PgbsonWriterInit(&writer);

	PgbsonWriterAppendBool(&writer, "isRowDeleted", 12, result->isRowDeleted);

	if (result->objectId != NULL)
	{
		PgbsonWriterAppendDocument(&writer, "objectId", 8, result->objectId);
	}

	if (result->resultDeletedDocument != NULL)
	{
		PgbsonWriterAppendDocument(&writer, "resultDeletedDocument", 21,
								   result->resultDeletedDocument);
	}

	return PgbsonWriterGetPgbson(&writer);
}


static pgbson *
SerializeDeleteWorkerSpecForUnsharded(BatchDeletionSpec *batchSpec)
{
	pgbson_writer topWriter;
	pgbson_writer writer;
	PgbsonWriterInit(&topWriter);
	PgbsonWriterStartDocument(&topWriter, "deleteUnsharded", 15, &writer);
	PgbsonWriterAppendUtf8(&writer, "collectionName", 14, batchSpec->collectionName);

	if (batchSpec->deletionValue.value_type != BSON_TYPE_EOD)
	{
		PgbsonWriterAppendValue(&writer, "deletionValue", 13, &batchSpec->deletionValue);
	}

	PgbsonWriterAppendBool(&writer, "ordered", 7, batchSpec->isOrdered);

	if (batchSpec->variableSpec.value_type != BSON_TYPE_EOD)
	{
		PgbsonWriterAppendValue(&writer, "variableSpec", 12, &batchSpec->variableSpec);
	}

	PgbsonWriterEndDocument(&topWriter, &writer);
	return PgbsonWriterGetPgbson(&topWriter);
}


static void
DeserializeDeleteWorkerSpecForUnsharded(const bson_value_t *value,
										BatchDeletionSpec *batchDeletionSpec)
{
	bson_iter_t deleteResultIter;
	BsonValueInitIterator(value, &deleteResultIter);

	while (bson_iter_next(&deleteResultIter))
	{
		const char *key = bson_iter_key(&deleteResultIter);
		if (strcmp(key, "collectionName") == 0)
		{
			batchDeletionSpec->collectionName = bson_iter_dup_utf8(&deleteResultIter,
																   NULL);
		}
		else if (strcmp(key, "deletionValue") == 0)
		{
			batchDeletionSpec->deletionValue = *bson_iter_value(&deleteResultIter);
		}
		else if (strcmp(key, "ordered") == 0)
		{
			batchDeletionSpec->isOrdered = bson_iter_bool(&deleteResultIter);
		}
		else if (strcmp(key, "variableSpec") == 0)
		{
			batchDeletionSpec->variableSpec = *bson_iter_value(&deleteResultIter);
		}
	}
}


/*
 * DeleteOneObjectId handles the case where we are deleting a single document
 * by _id from a collection that is sharded on some other key. In this case,
 * we need to look across all shards for a matching _id, then delete only that
 * one.
 *
 * Citus does not support SELECT .. FOR UPDATE, and it is very difficult to
 * support efficiently without running into frequent deadlocks. Therefore,
 * we instead do a regular SELECT. The implication is that the document might
 * be deleted or updated concurrently. In that case, we try again.
 */
static void
DeleteOneObjectId(MongoCollection *collection, DeleteOneParams *deleteOneParams,
				  bson_value_t *objectId, bool isIdValueCollationAware, bool
				  queryHasNonIdFilters,
				  bool forceInlineWrites, text *transactionId, DeleteOneResult *result)
{
	const int maxTries = 5;

	if (transactionId != NULL)
	{
		RetryableWriteResult writeResult;

		/*
		 * Try to find a retryable write record for the transaction ID in any shard.
		 */
		if (FindRetryRecordInAnyShard(collection->collectionId, transactionId,
									  &writeResult))
		{
			/* found a record, return the previous result */
			result->isRowDeleted = writeResult.rowsAffected > 0;
			return;
		}
	}

	for (int tryNumber = 0; tryNumber < maxTries; tryNumber++)
	{
		int64 shardKeyValue = 0;

		if (!FindShardKeyValueForDocumentId(collection, deleteOneParams->query, objectId,
											isIdValueCollationAware, queryHasNonIdFilters,
											&shardKeyValue,
											deleteOneParams->variableSpec,
											deleteOneParams->collationString))
		{
			/* no document matches both the query and the object ID */
			return;
		}

		CallDeleteOne(collection, deleteOneParams, shardKeyValue, transactionId,
					  forceInlineWrites, result);

		if (result->isRowDeleted)
		{
			/* deleted the document */
			return;
		}
	}

	ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
					errmsg("failed to delete document after %d tries", maxTries)));
}


/*
 * ValidateQueryDocuments validates query document of each deletion specified
 * by given BatchDeletionSpec and returns a list of write errors.
 *
 * Stops after the first failure if the deletion mode is ordered.
 *
 * This is useful for performing query document validations when we certainly
 * know that the delete operation would become a noop due to non-existent
 * collection.
 *
 * Otherwise, i.e. if the delete operaton wouldn't become a noop, then it
 * doesn't make sense to call this function both because we already perform
 * those validations at the runtime and also because this is quite expensive,
 * meaning that this function indeed processes "query" documents as if we're
 * in the run-time to implicitly perform necessary validations.
 */
static List *
ValidateQueryDocuments(BatchDeletionSpec *batchSpec)
{
	/* declared volatile because of the longjmp in PG_CATCH */
	List *volatile writeErrorList = NIL;

	/*
	 * Weirdly, compiler complains that writeErrorIdx might be clobbered by
	 * longjmp in PG_CATCH, so declare writeErrorIdx as volatile as well.
	 */
	for (volatile int writeErrorIdx = 0;
		 writeErrorIdx < list_length(batchSpec->deletionsProcessed);
		 writeErrorIdx++)
	{
		DeletionSpec *deletionSpec = list_nth(batchSpec->deletionsProcessed,
											  writeErrorIdx);

		/* declared volatile because of the longjmp in PG_CATCH */
		volatile bool isSuccess = false;

		MemoryContext oldContext = CurrentMemoryContext;
		PG_TRY();
		{
			ValidateQueryDocumentValue(deletionSpec->deleteOneParams.query);
			isSuccess = true;
		}
		PG_CATCH();
		{
			MemoryContextSwitchTo(oldContext);
			ErrorData *errorData = CopyErrorDataAndFlush();

			writeErrorList = lappend(writeErrorList, GetWriteErrorFromErrorData(errorData,
																				writeErrorIdx));
			isSuccess = false;
		}
		PG_END_TRY();

		if (!isSuccess && batchSpec->isOrdered)
		{
			/*
			 * Stop validating query documents after a failure if using
			 * ordered:true.
			 */
			break;
		}
	}

	return writeErrorList;
}


/*
 * BuildResponseMessage builds the response BSON for a delete command.
 */
static pgbson *
BuildResponseMessage(BatchDeletionResult *batchResult)
{
	pgbson_writer resultWriter;
	PgbsonWriterInit(&resultWriter);
	PgbsonWriterAppendInt32(&resultWriter, "n", 1, batchResult->rowsDeleted);
	PgbsonWriterAppendDouble(&resultWriter, "ok", 2, batchResult->ok);

	if (batchResult->writeErrors != NIL)
	{
		pgbson_array_writer writeErrorsArrayWriter;
		PgbsonWriterStartArray(&resultWriter, "writeErrors", 11, &writeErrorsArrayWriter);

		ListCell *writeErrorCell = NULL;
		foreach(writeErrorCell, batchResult->writeErrors)
		{
			WriteError *writeError = lfirst(writeErrorCell);

			pgbson_writer writeErrorWriter;
			PgbsonArrayWriterStartDocument(&writeErrorsArrayWriter, &writeErrorWriter);
			PgbsonWriterAppendInt32(&writeErrorWriter, "index", 5, writeError->index);
			PgbsonWriterAppendInt32(&writeErrorWriter, "code", 4, writeError->code);
			PgbsonWriterAppendUtf8(&writeErrorWriter, "errmsg", 6, writeError->errmsg);
			PgbsonArrayWriterEndDocument(&writeErrorsArrayWriter, &writeErrorWriter);
		}

		PgbsonWriterEndArray(&resultWriter, &writeErrorsArrayWriter);
	}

	return PgbsonWriterGetPgbson(&resultWriter);
}
