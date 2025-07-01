/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/processor/transaction.rs
 *
 *-------------------------------------------------------------------------
 */

use crate::{
    context::RequestContext,
    error::{DocumentDBError, ErrorCode, Result},
    postgres::PgDataClient,
    requests::{Request, RequestType},
    responses::Response,
};

// Create the transaction if required, and populate the context information with the transaction info
pub async fn handle(
    request: &Request<'_>,
    request_context: &mut RequestContext<'_>,
    pg_data_client: &impl PgDataClient,
) -> Result<()> {
    request_context.connection_context.transaction = None;
    if let Some(request_transaction_info) = &request_context.request_info.transaction_info {
        if request_transaction_info.auto_commit {
            return Ok(());
        }

        if matches!(
            request.request_type(),
            RequestType::ReIndex
                | RequestType::CreateIndex
                | RequestType::CreateIndexes
                | RequestType::DropIndexes
        ) {
            return Err(DocumentDBError::documentdb_error(
                ErrorCode::OperationNotSupportedInTransaction,
                format!(
                    "Cannot perform operation of type {} inside a transaction",
                    request.request_type()
                ),
            ));
        }

        if matches!(
            request.request_type(),
            RequestType::Aggregate
                | RequestType::FindAndModify
                | RequestType::Update
                | RequestType::Insert
                | RequestType::Count
                | RequestType::Distinct
                | RequestType::Find
                | RequestType::GetMore
        ) && matches!(request.db()?, "config" | "admin" | "local")
        {
            return Err(DocumentDBError::documentdb_error(
                ErrorCode::OperationNotSupportedInTransaction,
                format!(
                    "Cannot perform data operation against database {} inside a transaction",
                    request.db()?
                ),
            ));
        }

        let session_id = request_context
            .request_info
            .session_id
            .expect("Given that there's a transaction, there must be a session")
            .to_vec();
        let store = request_context
            .connection_context
            .service_context
            .transaction_store();
        let transaction_result = store
            .create(
                request_context.connection_context,
                request_transaction_info,
                session_id.clone(),
                pg_data_client,
            )
            .await;
        log::trace!(
            "Transaction acquired: {:?}, {:?}",
            request_transaction_info.transaction_number,
            transaction_result
        );

        if let Err(e) = transaction_result {
            return match (request.request_type(), &e) {
                // Especially allow the transaction to remain unfilled if it is committing a committed transaction
                (
                    RequestType::CommitTransaction,
                    DocumentDBError::DocumentDBError(ErrorCode::TransactionCommitted, _, _),
                ) => Ok(()),
                _ => Err(e),
            };
        }

        request_context.connection_context.transaction =
            Some((session_id, request_transaction_info.transaction_number));
    }
    Ok(())
}

pub async fn process_commit(request_context: &RequestContext<'_>) -> Result<Response> {
    if let Some((session_id, _)) = request_context.connection_context.transaction.as_ref() {
        let store = request_context
            .connection_context
            .service_context
            .transaction_store();
        store.commit(session_id).await?;
    }
    Ok(Response::ok())
}

pub async fn process_abort(request_context: &RequestContext<'_>) -> Result<Response> {
    let (session_id, _) = request_context
        .connection_context
        .transaction
        .as_ref()
        .ok_or(DocumentDBError::internal_error(
            "Transaction information was not populated for abort.".to_string(),
        ))?;

    let store = request_context
        .connection_context
        .service_context
        .transaction_store();
    store.abort(session_id).await?;
    Ok(Response::ok())
}
