/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/processor/delete.rs
 *
 *-------------------------------------------------------------------------
 */

use std::sync::Arc;

use bson::rawdoc;

use crate::{
    configuration::DynamicConfiguration,
    context::ConnectionContext,
    error::Result,
    postgres::PgDataClient,
    protocol::OK_SUCCEEDED,
    requests::{Request, RequestInfo},
    responses::{PgResponse, RawResponse, Response},
};

pub async fn process_drop_database(
    request_info: &mut RequestInfo<'_>,
    connection_context: &ConnectionContext,
    dynamic_config: &Arc<dyn DynamicConfiguration>,
    pg_data_client: &impl PgDataClient,
) -> Result<Response> {
    let db = request_info.db()?.to_string();

    // Invalidate cursors
    connection_context
        .service_context
        .invalidate_cursors_by_database(&db)
        .await;

    let is_read_only_for_disk_full = dynamic_config.is_read_only_for_disk_full().await;
    pg_data_client
        .execute_drop_database(
            request_info,
            db.as_str(),
            is_read_only_for_disk_full,
            connection_context,
        )
        .await?;

    Ok(Response::Raw(RawResponse(rawdoc! {
        "ok": OK_SUCCEEDED,
        "dropped": db,
    })))
}

pub async fn process_drop_collection(
    request_info: &mut RequestInfo<'_>,
    connection_context: &ConnectionContext,
    dynamic_config: &Arc<dyn DynamicConfiguration>,
    pg_data_client: &impl PgDataClient,
) -> Result<Response> {
    let coll = request_info.collection()?.to_string();
    let coll_str = coll.as_str();
    let db = request_info.db()?.to_string();
    let db_str = db.as_str();

    // Invalidate cursors
    connection_context
        .service_context
        .invalidate_cursors_by_collection(db_str, coll_str)
        .await;

    let is_read_only_for_disk_full = dynamic_config.is_read_only_for_disk_full().await;
    pg_data_client
        .execute_drop_collection(
            request_info,
            db_str,
            coll_str,
            is_read_only_for_disk_full,
            connection_context,
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
    pg_data_client: &impl PgDataClient,
) -> Result<Response> {
    let is_read_only_for_disk_full = dynamic_config.is_read_only_for_disk_full().await;
    let delete_rows = pg_data_client
        .execute_delete(
            request,
            request_info,
            is_read_only_for_disk_full,
            connection_context,
        )
        .await?;

    PgResponse::new(delete_rows)
        .transform_write_errors(connection_context)
        .await
}
