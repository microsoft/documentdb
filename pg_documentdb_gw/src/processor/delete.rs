/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/processor/delete.rs
 *
 *-------------------------------------------------------------------------
 */

use std::sync::Arc;

use bson::rawdoc;
use futures::Future;
use tokio_postgres::{types::Type, Row};

use crate::{
    configuration::DynamicConfiguration,
    context::ConnectionContext,
    error::Result,
    postgres::{self, Connection, PgDocument, Timeout},
    protocol::OK_SUCCEEDED,
    requests::{Request, RequestInfo},
    responses::{PgResponse, RawResponse, Response},
    QueryCatalog,
};

async fn run_readonly_if_needed<F, Fut>(
    conn: Arc<Connection>,
    dynamic_config: &Arc<dyn DynamicConfiguration>,
    query_catalog: &QueryCatalog,
    f: F,
) -> Result<Vec<Row>>
where
    F: FnOnce(Arc<Connection>) -> Fut,
    Fut: Future<Output = Result<Vec<Row>>>,
{
    if !conn.in_transaction && dynamic_config.is_read_only_for_disk_full().await {
        log::trace!("Executing delete operation in readonly state.");
        let mut transaction =
            postgres::Transaction::start(conn, tokio_postgres::IsolationLevel::RepeatableRead)
                .await?;

        // Allow write for this transaction
        let conn = transaction.get_connection();
        conn.batch_execute(query_catalog.set_allow_write()).await?;

        let result = f(conn).await?;
        transaction.commit().await?;
        Ok(result)
    } else {
        f(conn).await
    }
}

pub async fn process_drop_database(
    _request: &Request<'_>,
    request_info: &mut RequestInfo<'_>,
    conn_context: &ConnectionContext,
    dynamic_config: &Arc<dyn DynamicConfiguration>,
) -> Result<Response> {
    let db = request_info.db()?.to_string();

    // Invalidate cursors
    conn_context
        .service_context
        .invalidate_cursors_by_database(&db)
        .await;

    run_readonly_if_needed(
        conn_context.pull_connection().await?,
        dynamic_config,
        conn_context.service_context.query_catalog(),
        move |conn| async move {
            conn.query(
                conn_context.service_context.query_catalog().drop_database(),
                &[Type::TEXT],
                &[&request_info.db()?.to_string()],
                Timeout::transaction(request_info.max_time_ms),
                request_info,
            )
            .await
        },
    )
    .await?;

    Ok(Response::Raw(RawResponse(rawdoc! {
        "ok": OK_SUCCEEDED,
        "dropped": db,
    })))
}

pub async fn process_drop_collection(
    _request: &Request<'_>,
    request_info: &mut RequestInfo<'_>,
    connection_context: &ConnectionContext,
    dynamic_config: &Arc<dyn DynamicConfiguration>,
) -> Result<Response> {
    let coll = request_info.collection()?.to_string();

    // Invalidate cursors
    connection_context
        .service_context
        .invalidate_cursors_by_collection(request_info.db()?, request_info.collection()?)
        .await;

    run_readonly_if_needed(
        connection_context.pull_connection().await?,
        dynamic_config,
        connection_context.service_context.query_catalog(),
        move |conn| async move {
            conn.query(
                connection_context
                    .service_context
                    .query_catalog()
                    .drop_collection(),
                &[Type::TEXT, Type::TEXT],
                &[
                    &request_info.db()?.to_string(),
                    &request_info.collection()?.to_string(),
                ],
                Timeout::transaction(request_info.max_time_ms),
                request_info,
            )
            .await
        },
    )
    .await?;

    Ok(Response::Raw(RawResponse(rawdoc! {
        "ok": OK_SUCCEEDED,
        "dropped": coll,
    })))
}

pub async fn process_delete(
    request: &Request<'_>,
    request_info: &mut RequestInfo<'_>,
    connection_context: &ConnectionContext,
    dynamic_config: &Arc<dyn DynamicConfiguration>,
) -> Result<Response> {
    let results = run_readonly_if_needed(
        connection_context.pull_connection().await?,
        dynamic_config,
        connection_context.service_context.query_catalog(),
        move |conn| async move {
            conn.query(
                connection_context.service_context.query_catalog().delete(),
                &[Type::TEXT, Type::BYTEA, Type::BYTEA],
                &[
                    &request_info.db()?.to_string(),
                    &PgDocument(request.document()),
                    &request.extra(),
                ],
                Timeout::transaction(request_info.max_time_ms),
                request_info,
            )
            .await
        },
    )
    .await?;

    PgResponse::new(results)
        .transform_write_errors(connection_context)
        .await
}
