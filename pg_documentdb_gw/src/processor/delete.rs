use std::sync::Arc;

use bson::rawdoc;
use futures::Future;
use tokio_postgres::{types::Type, Row};

use crate::{
    configuration::DynamicConfiguration,
    context::ConnectionContext,
    error::Result,
    postgres::{self, Client, PgDocument, Timeout},
    protocol::OK_SUCCEEDED,
    requests::{Request, RequestInfo},
    responses::{PgResponse, RawResponse, Response},
    QueryCatalog,
};

async fn run_readonly_if_needed<F, Fut>(
    client: Arc<Client>,
    dynamic_config: &Arc<dyn DynamicConfiguration>,
    query_catalog: &QueryCatalog,
    f: F,
) -> Result<Vec<Row>>
where
    F: FnOnce(Arc<Client>) -> Fut,
    Fut: Future<Output = Result<Vec<Row>>>,
{
    if !client.in_transaction && dynamic_config.is_read_only_for_disk_full().await {
        log::trace!("Executing delete operation in readonly state.");
        let mut transaction =
            postgres::Transaction::start(client, tokio_postgres::IsolationLevel::RepeatableRead)
                .await?;

        // Allow write for this transaction
        let client = transaction.get_client();
        client
            .batch_execute(query_catalog.set_allow_write())
            .await?;

        let result = f(client).await?;
        transaction.commit().await?;
        Ok(result)
    } else {
        f(client).await
    }
}

pub async fn process_drop_database(
    _request: &Request<'_>,
    request_info: &RequestInfo<'_>,
    context: &ConnectionContext,
    dynamic_config: &Arc<dyn DynamicConfiguration>,
) -> Result<Response> {
    let db = request_info.db()?;

    // Invalidate cursors
    context
        .service_context
        .invalidate_cursors_by_database(db)
        .await;

    run_readonly_if_needed(
        context.pg().await?,
        dynamic_config,
        context.service_context.query_catalog(),
        move |client| async move {
            client
                .query(
                    context.service_context.query_catalog().drop_database(),
                    &[Type::TEXT],
                    &[&db],
                    Timeout::transaction(request_info.max_time_ms),
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
    request_info: &RequestInfo<'_>,
    connection_context: &ConnectionContext,
    dynamic_config: &Arc<dyn DynamicConfiguration>,
) -> Result<Response> {
    let coll = request_info.collection()?;

    // Invalidate cursors
    connection_context
        .service_context
        .invalidate_cursors_by_collection(request_info.db()?, coll)
        .await;

    run_readonly_if_needed(
        connection_context.pg().await?,
        dynamic_config,
        connection_context.service_context.query_catalog(),
        move |client| async move {
            client
                .query(
                    connection_context
                        .service_context
                        .query_catalog()
                        .drop_collection(),
                    &[Type::TEXT, Type::TEXT],
                    &[&request_info.db()?, &coll],
                    Timeout::transaction(request_info.max_time_ms),
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
    request_info: &RequestInfo<'_>,
    connection_context: &ConnectionContext,
    dynamic_config: &Arc<dyn DynamicConfiguration>,
) -> Result<Response> {
    let results = run_readonly_if_needed(
        connection_context.pg().await?,
        dynamic_config,
        connection_context.service_context.query_catalog(),
        move |client| async move {
            client
                .query(
                    connection_context.service_context.query_catalog().delete(),
                    &[Type::TEXT, Type::BYTEA, Type::BYTEA],
                    &[
                        &request_info.db()?,
                        &PgDocument(request.document()),
                        &request.extra(),
                    ],
                    Timeout::transaction(request_info.max_time_ms),
                )
                .await
        },
    )
    .await?;

    PgResponse::new(results)
        .transform_write_errors(connection_context)
        .await
}
