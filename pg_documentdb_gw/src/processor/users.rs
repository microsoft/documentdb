/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/processor/users.rs
 *
 *-------------------------------------------------------------------------
 */

use tokio_postgres::types::Type;

use crate::{
    context::ConnectionContext,
    error::DocumentDBError,
    postgres::{PgDocument, Timeout},
    requests::Request,
    responses::{PgResponse, Response},
};

pub(crate) async fn process_create_user(
    request: &Request<'_>,
    context: &mut ConnectionContext,
) -> Result<Response, DocumentDBError> {
    let request_info = request.extract_common()?;

    let results = context
        .pg()
        .await?
        .query(
            context.service_context.query_catalog().create_user(),
            &[Type::BYTEA],
            &[&PgDocument(request.document())],
            Timeout::transaction(request_info.max_time_ms),
        )
        .await?;
    Ok(Response::Pg(PgResponse::new(results)))
}

pub(crate) async fn process_drop_user(
    request: &Request<'_>,
    context: &mut ConnectionContext,
) -> Result<Response, DocumentDBError> {
    let request_info = request.extract_common()?;

    let results = context
        .pg()
        .await?
        .query(
            context.service_context.query_catalog().drop_user(),
            &[Type::BYTEA],
            &[&PgDocument(request.document())],
            Timeout::transaction(request_info.max_time_ms),
        )
        .await?;
    Ok(Response::Pg(PgResponse::new(results)))
}

pub(crate) async fn process_update_user(
    request: &Request<'_>,
    context: &mut ConnectionContext,
) -> Result<Response, DocumentDBError> {
    let request_info = request.extract_common()?;

    let results = context
        .pg()
        .await?
        .query(
            context.service_context.query_catalog().update_user(),
            &[Type::BYTEA],
            &[&PgDocument(request.document())],
            Timeout::transaction(request_info.max_time_ms),
        )
        .await?;
    Ok(Response::Pg(PgResponse::new(results)))
}

pub(crate) async fn process_users_info(
    request: &Request<'_>,
    context: &mut ConnectionContext,
) -> Result<Response, DocumentDBError> {
    let request_info = request.extract_common()?;

    let results = context
        .pg()
        .await?
        .query(
            context.service_context.query_catalog().users_info(),
            &[Type::BYTEA],
            &[&PgDocument(request.document())],
            Timeout::transaction(request_info.max_time_ms),
        )
        .await?;
    Ok(Response::Pg(PgResponse::new(results)))
}
