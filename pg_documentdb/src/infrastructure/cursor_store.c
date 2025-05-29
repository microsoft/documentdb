/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * include/planner/documentdb_plan_cache.h
 *
 * Common declarations for the pg_documentdb plan cache.
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>
#include <miscadmin.h>
#include <storage/sharedfileset.h>
#include <storage/dsm.h>
#include <storage/buffile.h>
#include <utils/resowner.h>
#include <utils/wait_event.h>
#include <port.h>
#include <utils/timestamp.h>
#include <utils/resowner.h>
#if PG_VERSION_NUM >= 170000
#else
#include <utils/resowner_private.h>
#endif
#include <utils/backend_status.h>

#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include "metadata/metadata_cache.h"
#include "utils/documentdb_errors.h"
#include "io/bson_core.h"
#include "infrastructure/cursor_store.h"

extern char *ApiGucPrefix;
extern bool UseFileBasedPersistedCursors;
extern bool EnableFileBasedPersistedCursors;
extern int MaxAllowedCursorIntermediateFileSize;
extern int DefaultCursorExpiryTimeLimitSeconds;


/*
 * Serialized State that is sent to the client
 * and used to rehydrate the cursor on getMore.
 */
typedef struct SerializedCursorState
{
	/* The name of the file in the cursor directory */
	char cursorFileName[NAMEDATALEN];

	/* The offset into the file */
	uint32_t file_offset;

	/* The total file length (updated during writes) */
	uint32_t file_length;
} SerializedCursorState;

typedef struct CursorFileState
{
	/* The serialized cursor state (see above) */
	SerializedCursorState cursorState;

	/* The file handle to write to*/
	File bufFile;

	/* Whether or not we're in R/W mode or R/O mode */
	bool isReadWrite;

	/* Temporary in-memory buffer for the cursor contents */
	PGAlignedBlock buffer;

	/* Position into the buffer currently written/read */
	int pos;

	/* Number of bytes in buffer that are valid (used in reads) */
	int nbytes;

	uint32_t next_offset;

	/* In read more - whether or not the cursor is complete */
	bool cursorComplete;
} CursorFileState;

PG_FUNCTION_INFO_V1(cursor_directory_cleanup);


static void FlushBuffer(CursorFileState *cursorFileState);
static bool FillBuffer(CursorFileState *cursorFileState, char *buffer, int32_t length);


#if PG_VERSION_NUM >= 170000
static void ResOwnerReleaseCursorFile(Datum res);
static char * ResOwnerPrintCursorFile(Datum res);
static const ResourceOwnerDesc file_resowner_desc =
{
	.name = "File",
	.release_phase = RESOURCE_RELEASE_AFTER_LOCKS,
	.release_priority = RELEASE_PRIO_FILES,
	.ReleaseResource = ResOwnerReleaseCursorFile,
	.DebugPrint = ResOwnerPrintCursorFile
};
#endif

/* Whether or not the cursor_set has been initialized during shared startup */
static bool cursor_set_initialized = false;

static const char *cursor_directory = "pg_documentdb_cursor_files";


/*
 * Runs a cleanup of the cursor directory.
 * Expires cursors that are older than the specified expiry limit.
 * If not set, uses the default GUC value for the expiry.
 * Note:
 * TODO: This should also likely handle scenarios like disk space into
 * the pruning algorithm.
 */
Datum
cursor_directory_cleanup(PG_FUNCTION_ARGS)
{
	if (!cursor_set_initialized || !UseFileBasedPersistedCursors ||
		!EnableFileBasedPersistedCursors)
	{
		PG_RETURN_VOID();
	}

	int64_t expiryTimeLimitSeconds = 0;
	if (PG_ARGISNULL(0))
	{
		expiryTimeLimitSeconds = DefaultCursorExpiryTimeLimitSeconds;
	}
	else
	{
		expiryTimeLimitSeconds = PG_GETARG_INT64(0);
	}

	DIR *dirdesc;
	struct dirent *de;

	dirdesc = AllocateDir(cursor_directory);
	if (!dirdesc)
	{
		/* Return empty tuplestore if appropriate */
		if (errno == ENOENT)
		{
			PG_RETURN_VOID();
		}
	}

	Size totalCursorSize = 0;
	while ((de = ReadDir(dirdesc, cursor_directory)) != NULL)
	{
		char path[MAXPGPATH * 2];
		struct stat attrib;

		/* Skip hidden files */
		if (de->d_name[0] == '.')
		{
			continue;
		}

		/* Get the file info */
		snprintf(path, sizeof(path), "%s/%s", cursor_directory, de->d_name);
		if (stat(path, &attrib) < 0)
		{
			/* Ignore concurrently-deleted files, else complain */
			if (errno == ENOENT)
			{
				continue;
			}

			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not stat file \"%s\": %m", path)));
		}

		/* Ignore anything but regular files */
		if (!S_ISREG(attrib.st_mode))
		{
			continue;
		}

		TimestampTz lastModified = time_t_to_timestamptz(attrib.st_mtime);
		TimestampTz currentTime = GetCurrentTimestamp();
		if (TimestampDifferenceExceeds(lastModified, currentTime, expiryTimeLimitSeconds *
									   1000))
		{
			ereport(LOG, (errmsg("Deleting expired cursor file %s", path)));
			durable_unlink(path, WARNING);
		}
		else
		{
			totalCursorSize += attrib.st_size;
		}
	}

	FreeDir(dirdesc);

	ereport(DEBUG1, (errmsg("Total size of cursor files: %ld", totalCursorSize)));
	PG_RETURN_VOID();
}


/*
 * We set up the shared file set storage for the cursor files.
 * This happens on shared_preload_libraries initialization so we
 * last for the entire life of the server.
 */
void
SetupCursorStorage(void)
{
	if (!EnableFileBasedPersistedCursors)
	{
		return;
	}

	if (!process_shared_preload_libraries_in_progress)
	{
		ereport(ERROR, (errmsg(
							"Cursor storage initialization must happen under shared_preload_libraries")));
	}

	if (!rmtree(cursor_directory, true) && errno != ENOENT)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not remove directory \"%s\": %m", cursor_directory)));
	}

	int result = MakePGDirectory(cursor_directory);
	if (result != 0 && result != EEXIST)
	{
		ereport(ERROR, (errcode_for_file_access(),
						errmsg("could not create directory for cursor files")));
	}

	cursor_set_initialized = true;
}


static inline void
ResourceOwnerRememberCurrentFile(File file)
{
#if PG_VERSION_NUM >= 170000
	ResourceOwnerEnlarge(CurrentResourceOwner);
	ResourceOwnerRemember(CurrentResourceOwner, Int32GetDatum(file), &file_resowner_desc);
#else
	ResourceOwnerEnlargeFiles(CurrentResourceOwner);
	ResourceOwnerRememberFile(CurrentResourceOwner, file);
#endif
}


/*
 * Starts a new query cursor file. This is called
 * on the first page of a query.
 * Registers the file in the cursor directory and
 * returns an opaque structure to track this cursor.
 * TODO: Need to apply storage backpressure for the cursor
 * files.
 */
CursorFileState *
GetCursorFile(const char *cursorName)
{
	if (!cursor_set_initialized)
	{
		ereport(ERROR, (errmsg(
							"Cursor storage not initialized. Before using cursors, the server must be started with"
							" %s.enableFileBasedPersistedCursors set to true",
							ApiGucPrefix)));
	}

	if (!UseFileBasedPersistedCursors)
	{
		ereport(ERROR, (errmsg("File based cursors are not enabled. "
							   "set %s.useFileBasedPersistedCursors to true",
							   ApiGucPrefix)));
	}

	if (strlen(cursorName) + strlen(cursor_directory) >= (NAMEDATALEN - 5))
	{
		ereport(ERROR, (errmsg("Cursor name is too long")));
	}

	CursorFileState *fileState = palloc0(sizeof(CursorFileState));
	snprintf(fileState->cursorState.cursorFileName, NAMEDATALEN, "%s/%s",
			 cursor_directory, cursorName);

	File cursorFile = PathNameOpenFile(fileState->cursorState.cursorFileName, O_RDWR |
									   O_CREAT | O_TRUNC | PG_BINARY);
	fileState->bufFile = cursorFile;
	fileState->isReadWrite = true;

	/* Track the file in the current resource owner:
	 * We will explicitly forget the file when we end the query.
	 * This is so that the file is cleaned if the query fails before
	 * we drain the whole cursor.
	 */
	ResourceOwnerRememberCurrentFile(cursorFile);

	return fileState;
}


/*
 * Given a cursor file that is created, writes a given document to that
 * cursor file. The file is written as
 * <length><document> where length is the size of the pgbson including the
 * varlen header.
 * The data is buffered in memory and flushed every BLCKSZ bytes.
 */
void
WriteToCursorFile(CursorFileState *cursorFileState, pgbson *dataBson)
{
	int32_t sizeRemaining = BLCKSZ - cursorFileState->pos;

	/* We don't have enough to write even the length - flush the buffer */
	if (sizeRemaining < 4)
	{
		FlushBuffer(cursorFileState);
	}

	int32_t dataSize = VARSIZE(dataBson);
	char *data = (char *) dataBson;

	/* Write the length to the buffer */
	memcpy(cursorFileState->buffer.data + cursorFileState->pos, &dataSize, 4);
	cursorFileState->pos += 4;

	/* Now write the file into the buffer and then write it out to the file */
	while (dataSize > 0)
	{
		sizeRemaining = BLCKSZ - cursorFileState->pos;
		if (sizeRemaining >= dataSize)
		{
			memcpy(cursorFileState->buffer.data + cursorFileState->pos, data, dataSize);
			cursorFileState->pos += dataSize;
			break;
		}
		else
		{
			memcpy(cursorFileState->buffer.data + cursorFileState->pos, data,
				   sizeRemaining);
			cursorFileState->pos += sizeRemaining;
			data += sizeRemaining;
			dataSize -= sizeRemaining;
			FlushBuffer(cursorFileState);
		}
	}
}


/*
 * Given an opaque serialized cursor state as a bytea, creates
 * a CursorFileState object that can be used to read from the
 * cursor file. This is the inverse of GetCursorFile.
 * The file is opened in read-only mode.
 * The file is expected to be in the cursor directory.
 */
CursorFileState *
DeserializeFileState(bytea *cursorFileState)
{
	if (!cursor_set_initialized)
	{
		ereport(ERROR, (errmsg("Cursor storage not initialized")));
	}

	if (!UseFileBasedPersistedCursors)
	{
		ereport(ERROR, (errmsg("File based cursor is not enabled")));
	}

	CursorFileState *fileState = palloc0(sizeof(CursorFileState));
	fileState->cursorState = *(SerializedCursorState *) VARDATA(cursorFileState);
	fileState->bufFile = PathNameOpenFile(fileState->cursorState.cursorFileName,
										  O_RDONLY | PG_BINARY);
	fileState->isReadWrite = false;
	fileState->next_offset = fileState->cursorState.file_offset;

	if (fileState->bufFile < 0 && errno == ENOENT)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_CURSORNOTFOUND),
						errmsg("Cursor not found")));
	}
	else if (fileState->bufFile < 0)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m",
						fileState->cursorState.cursorFileName)));
	}

	/* We now have a file ready, track in the current ResourceOwner */
	ResourceOwnerRememberCurrentFile(fileState->bufFile);
	return fileState;
}


/*
 * Given a cursor file state, reads the next document from the
 * cursor file. The file is expected to be in the cursor directory.
 * Blocks are pre-buffered in BLCKSZ chunks.
 *
 * Also updates the flush state of the cursor file state based on the
 * prior value read. This ensures that if we return a document, that we
 * only advance the cursor to include that when the next Read is called.
 */
pgbson *
ReadFromCursorFile(CursorFileState *cursorFileState)
{
	/* First step, advance the file stream forward with what was buffered before */
	cursorFileState->cursorState.file_offset = cursorFileState->next_offset;

	int32_t length = 0;
	if (!FillBuffer(cursorFileState, (char *) &length, 4))
	{
		return NULL;
	}

	if ((Size) length > BSON_MAX_SIZE)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_INTERNALERROR),
						errmsg("Invalid BSON size in cursor file %d", length)));
	}

	pgbson *bson = palloc(length);
	if (!FillBuffer(cursorFileState, (char *) bson, length))
	{
		pfree(bson);
		return NULL;
	}

	return bson;
}


/*
 * Fills the specified buffer with length bytes from the cursor file.
 * returns false if the end of the file is reached.
 */
static bool
FillBuffer(CursorFileState *cursorFileState, char *buffer, int32_t length)
{
	while (length > 0)
	{
		if (cursorFileState->nbytes == 0)
		{
			int bytesRead = FileRead(cursorFileState->bufFile,
									 cursorFileState->buffer.data, BLCKSZ,
									 cursorFileState->next_offset,
									 WAIT_EVENT_BUFFILE_READ);
			cursorFileState->nbytes += bytesRead;
			cursorFileState->pos = 0;
			if (bytesRead == 0)
			{
				/* There's no more bytes left */
				cursorFileState->cursorComplete = true;
				return false;
			}
		}

		int32_t currentAvailable = cursorFileState->nbytes - cursorFileState->pos;
		if (currentAvailable >= length)
		{
			memcpy(buffer, cursorFileState->buffer.data + cursorFileState->pos, length);
			cursorFileState->pos += length;
			cursorFileState->next_offset += length;
			return true;
		}

		memcpy(buffer, cursorFileState->buffer.data + cursorFileState->pos,
			   currentAvailable);

		cursorFileState->pos = cursorFileState->nbytes = 0;
		cursorFileState->next_offset += currentAvailable;
		buffer += currentAvailable;
		length -= currentAvailable;
	}

	return true;
}


/*
 * Writes whatever bytes have been filed into the buffer to the
 * cursor file. This is called when the buffer is full or
 * when the cursor file is closed.
 */
static void
FlushBuffer(CursorFileState *cursorFileState)
{
	if (cursorFileState->pos > 0)
	{
		int bytesWritten = FileWrite(cursorFileState->bufFile,
									 cursorFileState->buffer.data, cursorFileState->pos,
									 cursorFileState->cursorState.file_offset,
									 WAIT_EVENT_BUFFILE_WRITE);

		if (bytesWritten != cursorFileState->pos)
		{
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write to file")));
		}

		cursorFileState->cursorState.file_offset += cursorFileState->pos;
		cursorFileState->pos = 0;

		if (cursorFileState->cursorState.file_offset >
			(uint32_t) MaxAllowedCursorIntermediateFileSize)
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_INTERNALERROR),
							errmsg("Cursor file size %u exceeded the limit %d",
								   cursorFileState->cursorState.file_offset,
								   MaxAllowedCursorIntermediateFileSize)));
		}
	}
}


/*
 * Closes the cursor file and returns the serialized state
 * of the cursor. This is used to rehydrate the cursor
 * on getMore.
 * returns NULL if the cursor is complete.
 */
bytea *
CursorFileStateClose(CursorFileState *cursorFileState)
{
	if (cursorFileState->isReadWrite)
	{
		FlushBuffer(cursorFileState);
		cursorFileState->cursorState.file_length =
			cursorFileState->cursorState.file_offset;
		cursorFileState->cursorState.file_offset = 0;
		pgstat_report_tempfile(cursorFileState->cursorState.file_length);
	}

#if PG_VERSION_NUM >= 170000
	ResourceOwnerForget(CurrentResourceOwner, Int32GetDatum(cursorFileState->bufFile),
						&file_resowner_desc);
#else
	ResourceOwnerForgetFile(CurrentResourceOwner, cursorFileState->bufFile);
#endif
	FileClose(cursorFileState->bufFile);
	if (cursorFileState->cursorComplete)
	{
		/* Continuation state is null, delete the file */
		if (unlink(cursorFileState->cursorState.cursorFileName))
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not delete file \"%s\": %m",
							cursorFileState->cursorState.cursorFileName)));
		}

		return NULL;
	}

	/* Write the state for getMore */
	bytea *serializedSpec = palloc(sizeof(SerializedCursorState) + VARHDRSZ);
	SET_VARSIZE(serializedSpec, sizeof(SerializedCursorState) + VARHDRSZ);
	memcpy(VARDATA(serializedSpec), &cursorFileState->cursorState,
		   sizeof(SerializedCursorState));
	return serializedSpec;
}


#if PG_VERSION_NUM >= 170000
static void
ResOwnerReleaseCursorFile(Datum res)
{
	File file = (File) DatumGetInt32(res);
	FileClose(file);
}


static char *
ResOwnerPrintCursorFile(Datum res)
{
	return psprintf("Cursor File %d", DatumGetInt32(res));
}


#endif
