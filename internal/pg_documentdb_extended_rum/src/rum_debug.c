/*-------------------------------------------------------------------------
 *
 * rumdebug.c
 *	  utilities routines for the postgres inverted index access method.
 *
 *
 * Portions Copyright (c) 2015-2022, Postgres Professional
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * These functions are supposed to be used in tandem with pageinspect to
 * be able to introspect and debug RUM index pages
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "access/htup_details.h"
#include "miscadmin.h"
#include "storage/indexfsm.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/index_selfuncs.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"
#include "utils/jsonb.h"
#include "utils/numeric.h"
#include "funcapi.h"

#include "pg_documentdb_rum.h"

static Jsonb * GetResultJsonB(int count, char **keys, JsonbValue *values);
static Page get_page_from_raw(bytea *raw_page);
static Jsonb * RumPrintEntryToJsonB(Page page, uint64 counter, Oid firstEntryOid);
static Jsonb * RumPrintDataPageLineToJsonB(Page page, uint64 counter);

PG_FUNCTION_INFO_V1(rum_get_meta_page_info);
PG_FUNCTION_INFO_V1(rum_page_get_stats);
PG_FUNCTION_INFO_V1(rum_page_get_entries);
PG_FUNCTION_INFO_V1(rum_page_get_data_items);


Datum
rum_get_meta_page_info(PG_FUNCTION_ARGS)
{
	bytea *page = PG_GETARG_BYTEA_P(0);

	Page meta_page = get_page_from_raw(page);

	RumMetaPageData *pageData = RumPageGetMeta(meta_page);
	int nargs = 5;
	char *args[5] = { 0 };
	JsonbValue values[5] = { 0 };

	if (pageData->rumVersion != RUM_CURRENT_VERSION)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("Invalid RUM version in metadata page"),
				 errdetail("Expected version %u, got %u.",
						   RUM_CURRENT_VERSION, pageData->rumVersion)));
	}

	args[0] = "totalPages";
	values[0].type = jbvNumeric;
	values[0].val.numeric = int64_to_numeric(pageData->nTotalPages);

	args[1] = "entryPages";
	values[1].type = jbvNumeric;
	values[1].val.numeric = int64_to_numeric(pageData->nEntryPages);

	args[2] = "dataPages";
	values[2].type = jbvNumeric;
	values[2].val.numeric = int64_to_numeric(pageData->nDataPages);

	args[3] = "entries";
	values[3].type = jbvNumeric;
	values[3].val.numeric = int64_to_numeric(pageData->nEntries);

	args[4] = "pendingHeapTuples";
	values[4].type = jbvNumeric;
	values[4].val.numeric = int64_to_numeric(pageData->nPendingHeapTuples);

	PG_RETURN_POINTER(GetResultJsonB(nargs, args, values));
}


inline static char *
RumPageFlagsToString(Page page)
{
	StringInfo flagsStr = makeStringInfo();
	char *separator = "";

	if (RumPageIsLeaf(page))
	{
		appendStringInfo(flagsStr, "%sLEAF", separator);
		separator = "|";
	}

	if (RumPageIsData(page))
	{
		appendStringInfo(flagsStr, "%sDATA", separator);
		separator = "|";
	}

	if (RumPageIsDeleted(page))
	{
		appendStringInfo(flagsStr, "%sDELETED", separator);
		separator = "|";
	}

	if (RumPageIsHalfDead(page))
	{
		appendStringInfo(flagsStr, "%sHALFDEAD", separator);
		separator = "|";
	}

	return flagsStr->data;
}


Datum
rum_page_get_stats(PG_FUNCTION_ARGS)
{
	bytea *raw_page = PG_GETARG_BYTEA_P(0);
	Page page = get_page_from_raw(raw_page);
	char *flagsStr;
	int nargs = 5;
	char *args[5] = { 0 };
	JsonbValue values[5] = { 0 };

	args[0] = "flags";
	values[0].type = jbvNumeric;
	values[0].val.numeric = int64_to_numeric(RumPageGetOpaque(page)->flags);


	flagsStr = RumPageFlagsToString(page);
	args[1] = "flagsStr";
	values[1].type = jbvString;
	values[1].val.string.len = strlen(flagsStr);
	values[1].val.string.val = flagsStr;

	args[2] = "leftLink";
	if (RumPageGetOpaque(page)->leftlink == InvalidBlockNumber)
	{
		values[2].type = jbvNull;
	}
	else
	{
		values[2].type = jbvNumeric;
		values[2].val.numeric = int64_to_numeric(RumPageGetOpaque(page)->leftlink);
	}

	args[3] = "rightLink";
	if (RumPageGetOpaque(page)->rightlink == InvalidBlockNumber)
	{
		values[3].type = jbvNull;
	}
	else
	{
		values[3].type = jbvNumeric;
		values[3].val.numeric = int64_to_numeric(RumPageGetOpaque(page)->rightlink);
	}

	if (RumPageIsData(page))
	{
		args[4] = "nEntries";
		values[4].type = jbvNumeric;
		values[4].val.numeric = int64_to_numeric(RumPageGetOpaque(page)->maxoff);
	}
	else
	{
		args[4] = "nEntries";
		values[4].type = jbvNumeric;
		values[4].val.numeric = int64_to_numeric(PageGetMaxOffsetNumber(page));
	}

	PG_RETURN_POINTER(GetResultJsonB(nargs, args, values));
}


Datum
rum_page_get_entries(PG_FUNCTION_ARGS)
{
	Oid entryTypeOid = PG_GETARG_OID(1);
	FuncCallContext *fctx;
	Page page;
	if (SRF_IS_FIRSTCALL())
	{
		bytea *raw_page = PG_GETARG_BYTEA_P(0);
		MemoryContext mctx;

		fctx = SRF_FIRSTCALL_INIT();

		mctx = MemoryContextSwitchTo(fctx->multi_call_memory_ctx);
		page = get_page_from_raw(raw_page);
		MemoryContextSwitchTo(mctx);

		if (RumPageIsData(page) || RumPageIsDeleted(page))
		{
			ereport(WARNING, (errmsg("Cannot yet enumerate data or deleted pages")));
			PG_RETURN_NULL();
		}

		fctx->max_calls = PageGetMaxOffsetNumber(page);
		fctx->user_fctx = page;
	}

	fctx = SRF_PERCALL_SETUP();
	page = (Page) fctx->user_fctx;
	if (fctx->call_cntr < fctx->max_calls)
	{
		Jsonb *result = RumPrintEntryToJsonB(page, fctx->call_cntr, entryTypeOid);
		SRF_RETURN_NEXT(fctx, PointerGetDatum(result));
	}

	SRF_RETURN_DONE(fctx);

	PG_RETURN_NULL();
}


Datum
rum_page_get_data_items(PG_FUNCTION_ARGS)
{
	FuncCallContext *fctx;
	Page page;
	if (SRF_IS_FIRSTCALL())
	{
		bytea *raw_page = PG_GETARG_BYTEA_P(0);
		MemoryContext mctx;

		fctx = SRF_FIRSTCALL_INIT();

		mctx = MemoryContextSwitchTo(fctx->multi_call_memory_ctx);
		page = get_page_from_raw(raw_page);
		MemoryContextSwitchTo(mctx);

		if (!RumPageIsData(page) || RumPageIsDeleted(page))
		{
			ereport(WARNING, (errmsg("Cannot yet enumerate deleted pages")));
			PG_RETURN_NULL();
		}

		/* data pages use offset 0 to print the right bound */
		fctx->max_calls = RumPageGetOpaque(page)->maxoff + 1;
		fctx->user_fctx = page;
	}

	fctx = SRF_PERCALL_SETUP();
	page = (Page) fctx->user_fctx;
	if (fctx->call_cntr < fctx->max_calls)
	{
		Jsonb *result = RumPrintDataPageLineToJsonB(page, fctx->call_cntr);
		SRF_RETURN_NEXT(fctx, PointerGetDatum(result));
	}

	SRF_RETURN_DONE(fctx);
	PG_RETURN_NULL();
}


static Jsonb *
GetResultJsonB(int count, char **keys, JsonbValue *values)
{
	int i = 0;
	JsonbParseState *state = NULL;
	JsonbValue *res;

	(void) pushJsonbValue(&state, WJB_BEGIN_OBJECT, NULL);

	for (i = 0; i < count; i++)
	{
		JsonbValue key;
		key.type = jbvString;
		key.val.string.val = keys[i];
		key.val.string.len = strlen(keys[i]);
		(void) pushJsonbValue(&state, WJB_KEY, &key);
		(void) pushJsonbValue(&state, WJB_VALUE, &values[i]);
	}

	res = pushJsonbValue(&state, WJB_END_OBJECT, NULL);

	return JsonbValueToJsonb(res);
}


static Page
get_page_from_raw(bytea *raw_page)
{
	Page page;
	int raw_page_size;

	raw_page_size = VARSIZE_ANY_EXHDR(raw_page);

	if (raw_page_size != BLCKSZ)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid page size"),
				 errdetail("Expected %d bytes, got %d.",
						   BLCKSZ, raw_page_size)));
	}

	page = palloc(raw_page_size);

	memcpy(page, VARDATA_ANY(raw_page), raw_page_size);
	return page;
}


static Jsonb *
RumPrintEntryToJsonB(Page page, uint64 counter, Oid firstEntryOid)
{
	OffsetNumber offset = (OffsetNumber) (counter + 1);
	int nargs = 7;
	char *args[7] = { 0 };
	JsonbValue values[7] = { 0 };
	IndexTuple tuple;

	Datum firstEntryDatum, firstEntryCStringDatum;
	char *ptr, *dump, *datacstring, *firstEntryCString;
	Size dlen;
	int off;
	Oid typeOutputFunction;
	bool typIsVarlena;
	char itemPointerToString[128] = { 0 };
	Assert(counter < PageGetMaxOffsetNumber(page));

	tuple = (IndexTuple) PageGetItem(page, PageGetItemId(page, offset));

	args[0] = "offset";
	values[0].type = jbvNumeric;
	values[0].val.numeric = int64_to_numeric(offset);

	sprintf(itemPointerToString, "(%u,%d)",
			((uint32_t) tuple->t_tid.ip_blkid.bi_hi << 16) | tuple->t_tid.ip_blkid.bi_lo,
			tuple->t_tid.ip_posid);
	args[1] = "tupleTid";
	values[1].type = jbvString;
	values[1].val.string.len = strlen(itemPointerToString);
	values[1].val.string.val = itemPointerToString;

	args[2] = "entryType";
	values[2].type = jbvString;
	values[2].val.string.val = RumIsPostingTree(tuple) ? "postingTree" : "postingList";
	values[2].val.string.len = strlen(values[2].val.string.val);

	args[3] = "numPostings";
	values[3].type = jbvNumeric;

	if (RumIsPostingTree(tuple))
	{
		values[3].val.numeric = int64_to_numeric(-1);
	}
	else
	{
		values[3].val.numeric = int64_to_numeric(RumGetNPosting(tuple));
	}

	ptr = (char *) tuple + IndexInfoFindDataOffset(tuple->t_info);
	dlen = IndexTupleSize(tuple) - IndexInfoFindDataOffset(tuple->t_info);

	args[4] = "data";
	values[4].type = jbvString;

	dump = palloc0(dlen * 3 + 1);
	datacstring = dump;
	for (off = 0; off < dlen; off++)
	{
		sprintf(dump, "%02x", *(ptr + off) & 0xff);
		dump += 2;
	}

	firstEntryDatum = fetch_att(ptr, get_typbyval(firstEntryOid), get_typlen(
									firstEntryOid));
	getTypeOutputInfo(firstEntryOid, &typeOutputFunction, &typIsVarlena);

	firstEntryCStringDatum = OidFunctionCall1(typeOutputFunction, firstEntryDatum);
	firstEntryCString = DatumGetCString(firstEntryCStringDatum);

	values[4].val.string.len = dlen * 2;
	values[4].val.string.val = datacstring;

	args[5] = "firstTids";
	values[5].type = jbvNull;
	if (!RumIsPostingTree(tuple) && RumGetNPosting(tuple) > 0)
	{
		RumItem item;
		StringInfo s = makeStringInfo();
		ptr = RumGetPosting(tuple);

		ItemPointerSetMin(&item.iptr);
		for (off = 0; off < RumGetNPosting(tuple) && off < 5; off++)
		{
			ptr = rumDataPageLeafReadItemPointer(ptr, &item.iptr, &item.addInfoIsNull);
			appendStringInfo(s, "(%u,%d),",
							 ((uint32_t) item.iptr.ip_blkid.bi_hi << 16) |
							 item.iptr.ip_blkid.bi_lo,
							 item.iptr.ip_posid);
		}
		values[5].type = jbvString;
		values[5].val.string.len = strlen(s->data);
		values[5].val.string.val = s->data;
	}

	args[6] = "firstEntry";
	values[6].type = jbvString;
	values[6].val.string.len = strlen(firstEntryCString);
	values[6].val.string.val = firstEntryCString;

	return GetResultJsonB(nargs, args, values);
}


static Jsonb *
RumPrintDataPageLineToJsonB(Page page, uint64 counter)
{
	OffsetNumber offset = (OffsetNumber) counter;
	int nargs = 2;
	char *args[6] = { 0 };
	JsonbValue values[6] = { 0 };
	RumItem item;
	BlockNumber childBlock = InvalidBlockNumber;

	int off;
	char itemPointerToString[128] = { 0 };

	args[0] = "offset";
	values[0].type = jbvNumeric;
	values[0].val.numeric = int64_to_numeric(offset);

	if (offset == 0)
	{
		item = *RumDataPageGetRightBound(page);
	}
	else if (RumPageIsLeaf(page))
	{
		char *ptr = RumDataPageGetData(page);
		RumItemSetMin(&item);

		/* Enumerate until the requested offset */
		for (off = FirstOffsetNumber; off <= offset; off++)
		{
			ptr = rumDataPageLeafReadItemPointer(ptr, &item.iptr,
												 &item.addInfoIsNull);
		}
	}
	else
	{
		/* Intermediate data page */
		PostingItem *pitem = (PostingItem *) RumDataPageGetItem(page, offset);
		item = pitem->item;
		childBlock = BlockIdGetBlockNumber(&pitem->child_blkno);
	}

	args[1] = "itemTid";
	sprintf(itemPointerToString, "(%u,%d)",
			((uint32_t) item.iptr.ip_blkid.bi_hi << 16) | item.iptr.ip_blkid.bi_lo,
			item.iptr.ip_posid);
	values[1].type = jbvString;
	values[1].val.string.len = strlen(itemPointerToString);
	values[1].val.string.val = itemPointerToString;

	if (childBlock != InvalidBlockNumber)
	{
		args[2] = "childBlock";
		values[2].type = jbvNumeric;
		values[2].val.numeric = int64_to_numeric(childBlock);
		nargs++;
	}

	return GetResultJsonB(nargs, args, values);
}
