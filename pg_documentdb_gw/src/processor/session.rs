/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/processor/session.rs
 *
 *-------------------------------------------------------------------------
 */

use crate::{
    context::RequestContext,
    error::{DocumentDBError, Result},
    requests::Request,
    responses::Response,
};

pub async fn end_sessions(
    request: &Request<'_>,
    request_context: &RequestContext<'_>,
) -> Result<Response> {
    let store = request_context
        .connection_context
        .service_context
        .transaction_store();
    for value in request
        .document()
        .get_array("endSessions")
        .map_err(DocumentDBError::parse_failure())?
    {
        let value = value?;
        let session_id = value
            .as_document()
            .ok_or(DocumentDBError::bad_value(
                "Session should be a document".to_string(),
            ))?
            .get_binary("id")
            .map_err(DocumentDBError::parse_failure())?
            .bytes;

        // Best effort abort any transaction for the session
        let _ = store.abort(session_id).await;

        // Remove all cursors for the session
        request_context
            .connection_context
            .service_context
            .invalidate_cursors_by_session(session_id)
            .await
    }
    Ok(Response::ok())
}
