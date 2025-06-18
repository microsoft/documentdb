/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/processor/users.rs
 *
 *-------------------------------------------------------------------------
 */

use crate::{
    context::ConnectionContext,
    error::DocumentDBError,
    postgres::PgDataClient,
    requests::{Request, RequestInfo},
    responses::Response,
};

pub async fn process_create_user(
    request: &Request<'_>,
    request_info: &mut RequestInfo<'_>,
    connection_context: &mut ConnectionContext,
    pg_data_client: &impl PgDataClient,
) -> Result<Response, DocumentDBError> {
    pg_data_client
        .execute_create_user(request, request_info, connection_context)
        .await
}

pub async fn process_drop_user(
    request: &Request<'_>,
    request_info: &mut RequestInfo<'_>,
    connection_context: &mut ConnectionContext,
    pg_data_client: &impl PgDataClient,
) -> Result<Response, DocumentDBError> {
    pg_data_client
        .execute_drop_user(request, request_info, connection_context)
        .await
}

pub async fn process_update_user(
    request: &Request<'_>,
    request_info: &mut RequestInfo<'_>,
    connection_context: &mut ConnectionContext,
    pg_data_client: &impl PgDataClient,
) -> Result<Response, DocumentDBError> {
    pg_data_client
        .execute_update_user(request, request_info, connection_context)
        .await
}

pub async fn process_users_info(
    request: &Request<'_>,
    request_info: &mut RequestInfo<'_>,
    connection_context: &mut ConnectionContext,
    pg_data_client: &impl PgDataClient,
) -> Result<Response, DocumentDBError> {
    pg_data_client
        .execute_users_info(request, request_info, connection_context)
        .await
}
