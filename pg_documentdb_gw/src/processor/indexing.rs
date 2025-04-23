/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/processor/indexing.rs
 *
 *-------------------------------------------------------------------------
 */

use std::sync::Arc;
use std::time::{Duration, Instant};

use bson::{Document, RawDocumentBuf};
use tokio_postgres::types::Type;

use crate::{
    configuration::DynamicConfiguration,
    context::ConnectionContext,
    error::{DocumentDBError, ErrorCode, Result},
    postgres::{PgDocument, Timeout},
    requests::{Request, RequestInfo},
    responses::{PgResponse, RawResponse, Response},
};

use super::cursor::save_cursor;

pub async fn process_create_indexes(
    request: &Request<'_>,
    request_info: &RequestInfo<'_>,
    connection_context: &ConnectionContext,
    dynamic_config: &Arc<dyn DynamicConfiguration>,
) -> Result<Response> {
    let db = request_info.db()?;
    if db == "config" || db == "admin" {
        return Err(DocumentDBError::documentdb_error(
            ErrorCode::IllegalOperation,
            "Indexes cannot be created in config and admin databases.".to_string(),
        ));
    }

    let results = connection_context
        .pg()
        .await?
        .query_db_bson(
            connection_context
                .service_context
                .query_catalog()
                .create_indexes_background(),
            db,
            &PgDocument(request.document()),
            Timeout::command(request_info.max_time_ms),
        )
        .await?;
    let row = results
        .first()
        .ok_or(DocumentDBError::pg_response_empty())?;
    let success: bool = row.get(1);
    let response = PgResponse::new(results);
    if success {
        wait_for_index(request_info, response, connection_context, dynamic_config).await
    } else {
        parse_create_index_error(&response)
    }
}

pub async fn wait_for_index(
    request_info: &RequestInfo<'_>,
    create_result: PgResponse,
    context: &ConnectionContext,
    dynamic_config: &Arc<dyn DynamicConfiguration>,
) -> Result<Response> {
    let start_time = Instant::now();
    let create_request_details: PgDocument = create_result.first()?.get(2);

    if create_request_details.0.is_empty() {
        return Ok(Response::Pg(create_result));
    }

    let mut interval = tokio::time::interval(Duration::from_millis(
        dynamic_config.index_build_sleep_milli_secs().await as u64,
    ));
    loop {
        interval.tick().await;
        let results = context
            .pg()
            .await?
            .query(
                context
                    .service_context
                    .query_catalog()
                    .check_build_index_status(),
                &[Type::BYTEA],
                &[&create_request_details],
                Timeout::command(request_info.max_time_ms),
            )
            .await?;

        let row = results
            .first()
            .ok_or(DocumentDBError::pg_response_empty())?;

        let success: bool = row.get(1);

        if !success {
            return parse_create_index_error(&PgResponse::new(results));
        }

        let complete: bool = row.get(2);
        if complete {
            return Ok(Response::Pg(create_result));
        }

        if let Some(max_time_ms) = request_info.max_time_ms {
            let max_time_ms = max_time_ms.try_into().map_err(|_| {
                DocumentDBError::internal_error("Failed to convert max_time_ms to u128".to_string())
            })?;
            if start_time.elapsed().as_millis() > max_time_ms {
                return Err(DocumentDBError::documentdb_error(ErrorCode::ExceededTimeLimit, "The command being executed was terminated due to a command timeout. This may be due to concurrent transactions. Consider increasing the maxTimeMS on the command.".to_string()));
            }
        }
    }
}

fn parse_create_index_error(response: &PgResponse) -> Result<Response> {
    let response = response.as_raw_document()?;
    let raw = response
        .get_document("raw")
        .map_err(DocumentDBError::pg_response_invalid)?;

    let mut errmsg = None;
    let mut code = None;
    for shard in raw.into_iter() {
        let (_, v) = shard?;
        for entry in v.as_document().ok_or(DocumentDBError::internal_error(
            "CreateIndex shard was not a document".to_string(),
        ))? {
            let (k, v) = entry?;
            match k {
                "errmsg" => {
                    errmsg = Some(v.as_str().ok_or(DocumentDBError::internal_error(
                        "errmsg was not a string".to_string(),
                    ))?);
                }
                "code" => {
                    code = Some(v.as_i32().ok_or(DocumentDBError::internal_error(
                        "Code was not an i32".to_string(),
                    ))?);
                }
                _ => {}
            }
        }
    }
    let code = code.ok_or(DocumentDBError::internal_error(
        "errmsg was missing in create index result".to_string(),
    ))?;
    let errmsg = errmsg.ok_or(DocumentDBError::internal_error(
        "errmsg was missing in create index result".to_string(),
    ))?;
    Err(DocumentDBError::PostgresDocumentDBError(
        code,
        errmsg.to_string(),
        std::backtrace::Backtrace::capture(),
    ))
}

pub async fn process_reindex(
    _request: &Request<'_>,
    request_info: &RequestInfo<'_>,
    context: &ConnectionContext,
) -> Result<Response> {
    let results = context
        .pg()
        .await?
        .query(
            context.service_context.query_catalog().re_index(),
            &[Type::TEXT, Type::TEXT],
            &[&request_info.db()?, &request_info.collection()?],
            Timeout::command(request_info.max_time_ms),
        )
        .await?;
    Ok(Response::Pg(PgResponse::new(results)))
}

pub async fn process_drop_indexes(
    request: &Request<'_>,
    request_info: &RequestInfo<'_>,
    context: &ConnectionContext,
) -> Result<Response> {
    let results = context
        .pg()
        .await?
        .query_db_bson(
            context.service_context.query_catalog().drop_indexes(),
            request_info.db()?,
            &PgDocument(request.document()),
            Timeout::transaction(request_info.max_time_ms),
        )
        .await?;

    let response = PgResponse::new(results);

    // TODO: It should not be needed to convert the document, but the backend returns ok:true instead of ok:1
    let mut response = Document::try_from(response.as_raw_document()?)?;
    let response_code = response.get_bool("ok").map_err(|e| {
        DocumentDBError::internal_error(format!("PG returned invalid response: {}", e))
    })?;

    response.insert("ok", if response_code { 1 } else { 0 });
    Ok(Response::Raw(RawResponse(RawDocumentBuf::from_document(
        &response,
    )?)))
}

pub async fn process_list_indexes(
    request: &Request<'_>,
    request_info: &RequestInfo<'_>,
    context: &ConnectionContext,
) -> Result<Response> {
    let client = context.pg().await?;

    let results = client
        .query_db_bson(
            context
                .service_context
                .query_catalog()
                .list_indexes_cursor_first_page(),
            request_info.db()?,
            &PgDocument(request.document()),
            Timeout::transaction(request_info.max_time_ms),
        )
        .await?;

    let response = PgResponse::new(results);
    save_cursor(context, client, &response, request_info).await?;
    Ok(Response::Pg(response))
}
