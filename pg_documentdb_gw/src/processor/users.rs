/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/processor/users.rs
 *
 *-------------------------------------------------------------------------
 */

use crate::{
    context::RequestContext, error::Result, postgres::PgDataClient, requests::Request,
    responses::Response,
};

pub async fn process_create_user(
    request: &Request<'_>,
    request_context: &mut RequestContext<'_>,
    pg_data_client: &impl PgDataClient,
) -> Result<Response> {
    pg_data_client
        .execute_create_user(
            request,
            request_context.request_info,
            request_context.connection_context,
        )
        .await
}

pub async fn process_drop_user(
    request: &Request<'_>,
    request_context: &mut RequestContext<'_>,
    pg_data_client: &impl PgDataClient,
) -> Result<Response> {
    pg_data_client
        .execute_drop_user(
            request,
            request_context.request_info,
            request_context.connection_context,
        )
        .await
}

pub async fn process_update_user(
    request: &Request<'_>,
    request_context: &mut RequestContext<'_>,
    pg_data_client: &impl PgDataClient,
) -> Result<Response> {
    pg_data_client
        .execute_update_user(
            request,
            request_context.request_info,
            request_context.connection_context,
        )
        .await
}

pub async fn process_users_info(
    request: &Request<'_>,
    request_context: &mut RequestContext<'_>,
    pg_data_client: &impl PgDataClient,
) -> Result<Response> {
    pg_data_client
        .execute_users_info(
            request,
            request_context.request_info,
            request_context.connection_context,
        )
        .await
}

pub async fn process_connection_status(
    request: &Request<'_>,
    request_context: &mut RequestContext<'_>,
    pg_data_client: &impl PgDataClient,
) -> Result<Response> {
    pg_data_client
        .execute_connection_status(
            request,
            request_context.request_info,
            request_context.connection_context,
        )
        .await
}
