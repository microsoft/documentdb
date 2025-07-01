/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/processor/cursor.rs
 *
 *-------------------------------------------------------------------------
 */

use std::sync::Arc;

use bson::{rawdoc, RawArrayBuf};

use crate::{
    context::{Cursor, CursorStoreEntry, RequestContext},
    error::{DocumentDBError, ErrorCode, Result},
    postgres::{Connection, PgDataClient, PgDocument},
    protocol::OK_SUCCEEDED,
    requests::Request,
    responses::{PgResponse, RawResponse, Response},
};

pub async fn save_cursor(
    request_context: &mut RequestContext<'_>,
    connection: Arc<Connection>,
    response: &PgResponse,
) -> Result<()> {
    if let Some((persist, cursor)) = response.get_cursor()? {
        let connection = if persist { Some(connection) } else { None };
        request_context
            .connection_context
            .add_cursor(
                connection,
                cursor,
                request_context.connection_context.auth_state.username()?,
                request_context.request_info.db()?,
                request_context.request_info.collection()?,
                request_context.request_info.session_id.map(|v| v.to_vec()),
            )
            .await;
    }
    Ok(())
}

pub async fn process_kill_cursors(
    request: &Request<'_>,
    request_context: &mut RequestContext<'_>,
) -> Result<Response> {
    let _ = request
        .document()
        .get_str("killCursors")
        .map_err(DocumentDBError::parse_failure())?;

    let cursors = request
        .document()
        .get("cursors")?
        .ok_or(DocumentDBError::bad_value(
            "killCursors should have a cursors field".to_string(),
        ))?
        .as_array()
        .ok_or(DocumentDBError::documentdb_error(
            ErrorCode::TypeMismatch,
            "KillCursors cursors should be an array".to_string(),
        ))?;

    let mut cursor_ids = Vec::new();
    for value in cursors {
        let cursor = value?.as_i64().ok_or(DocumentDBError::bad_value(
            "Cursor was not a valid i64".to_string(),
        ))?;
        cursor_ids.push(cursor);
    }
    let (removed_cursors, missing_cursors) = request_context
        .connection_context
        .service_context
        .kill_cursors(
            request_context.connection_context.auth_state.username()?,
            &cursor_ids,
        )
        .await;
    let mut removed_cursor_buf = RawArrayBuf::new();
    for cursor in removed_cursors {
        removed_cursor_buf.push(cursor);
    }
    let mut missing_cursor_buf = RawArrayBuf::new();
    for cursor in missing_cursors {
        missing_cursor_buf.push(cursor);
    }

    Ok(Response::Raw(RawResponse(rawdoc! {
        "ok":OK_SUCCEEDED,
        "cursorsKilled": removed_cursor_buf,
        "cursorsNotFound": missing_cursor_buf,
        "cursorsAlive": [],
        "cursorsUnknown":[],
    })))
}

pub async fn process_get_more(
    request: &Request<'_>,
    request_context: &mut RequestContext<'_>,
    pg_data_client: &impl PgDataClient,
) -> Result<Response> {
    let mut id = None;
    request.extract_fields(|k, v| {
        if k == "getMore" {
            id = Some(v.as_i64().ok_or(DocumentDBError::bad_value(
                "getMore value should be an i64".to_string(),
            ))?)
        }
        Ok(())
    })?;
    let id = id.ok_or(DocumentDBError::bad_value(
        "getMore not present in document".to_string(),
    ))?;
    let CursorStoreEntry {
        connection: cursor_connection,
        cursor,
        db,
        collection,
        session_id,
        ..
    } = request_context
        .connection_context
        .get_cursor(
            id,
            request_context.connection_context.auth_state.username()?,
        )
        .await
        .ok_or(DocumentDBError::documentdb_error(
            ErrorCode::CursorNotFound,
            "Provided cursor not found.".to_string(),
        ))?;

    let results = pg_data_client
        .execute_cursor_get_more(
            request,
            request_context.request_info,
            &db,
            &cursor,
            &cursor_connection,
            request_context.connection_context,
        )
        .await?;

    if let Some(row) = results.first() {
        let continuation: Option<PgDocument> = row.try_get(1)?;
        if let Some(continuation) = continuation {
            request_context
                .connection_context
                .add_cursor(
                    cursor_connection,
                    Cursor {
                        cursor_id: id,
                        continuation: continuation.0.to_raw_document_buf(),
                    },
                    request_context.connection_context.auth_state.username()?,
                    &db,
                    &collection,
                    session_id,
                )
                .await;
        }
    }

    Ok(Response::Pg(PgResponse::new(results)))
}
