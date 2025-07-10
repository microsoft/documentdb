/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/processor/process.rs
 *
 *-------------------------------------------------------------------------
 */

use std::{
    sync::Arc,
    time::{Duration, Instant},
};

use deadpool_postgres::{HookError, PoolError};
use tokio_postgres::error::SqlState;

use crate::{
    configuration::DynamicConfiguration,
    context::ConnectionContext,
    error::{DocumentDBError, ErrorCode, Result},
    explain,
    postgres::PgDataClient,
    processor::{data_description, data_management},
    requests::{Request, RequestInfo, RequestType},
    responses::Response,
};

use super::{constant, cursor, indexing, ismaster, session, transaction, users};

enum Retry {
    Long,
    Short,
    None,
}

pub async fn process_request(
    request: &Request<'_>,
    request_info: &mut RequestInfo<'_>,
    connection_context: &mut ConnectionContext,
    pg_data_client: impl PgDataClient,
) -> Result<Response> {
    let dynamic_config = connection_context.dynamic_configuration();
    transaction::handle(request, request_info, connection_context, &pg_data_client).await?;
    let start_time = Instant::now();

    let mut retries = 0;
    let result = loop {
        let response = match request.request_type() {
            RequestType::Aggregate => {
                data_management::process_aggregate(
                    request,
                    request_info,
                    connection_context,
                    &pg_data_client,
                )
                .await
            }
            RequestType::BuildInfo => constant::process_build_info(&dynamic_config).await,
            RequestType::CollStats => {
                data_management::process_coll_stats(
                    request,
                    request_info,
                    connection_context,
                    &pg_data_client,
                )
                .await
            }
            RequestType::ConnectionStatus => {
                users::process_connection_status(
                    request,
                    request_info,
                    connection_context,
                    &pg_data_client,
                )
                .await
            }
            RequestType::Count => {
                data_management::process_count(
                    request,
                    request_info,
                    connection_context,
                    &pg_data_client,
                )
                .await
            }
            RequestType::Create => {
                data_description::process_create(
                    request,
                    request_info,
                    connection_context,
                    &pg_data_client,
                )
                .await
            }
            RequestType::CreateIndex | RequestType::CreateIndexes => {
                indexing::process_create_indexes(
                    request,
                    request_info,
                    connection_context,
                    &dynamic_config,
                    &pg_data_client,
                )
                .await
            }
            RequestType::Delete => {
                data_management::process_delete(
                    request,
                    request_info,
                    connection_context,
                    &dynamic_config,
                    &pg_data_client,
                )
                .await
            }
            RequestType::Distinct => {
                data_management::process_distinct(
                    request,
                    request_info,
                    connection_context,
                    &pg_data_client,
                )
                .await
            }
            RequestType::Drop => {
                data_description::process_drop_collection(
                    request_info,
                    connection_context,
                    &dynamic_config,
                    &pg_data_client,
                )
                .await
            }
            RequestType::DropDatabase => {
                data_description::process_drop_database(
                    request_info,
                    connection_context,
                    &dynamic_config,
                    &pg_data_client,
                )
                .await
            }
            RequestType::Explain => {
                explain::process_explain(
                    request,
                    request_info,
                    None,
                    connection_context,
                    &pg_data_client,
                )
                .await
            }
            RequestType::Find => {
                data_management::process_find(
                    request,
                    request_info,
                    connection_context,
                    &pg_data_client,
                )
                .await
            }
            RequestType::FindAndModify => {
                data_management::process_find_and_modify(
                    request,
                    request_info,
                    connection_context,
                    &pg_data_client,
                )
                .await
            }
            RequestType::GetCmdLineOpts => constant::process_get_cmd_line_opts(),
            RequestType::GetDefaultRWConcern => {
                constant::process_get_rw_concern(request, request_info)
            }
            RequestType::GetLog => constant::process_get_log(),
            RequestType::GetMore => {
                cursor::process_get_more(request, request_info, connection_context, &pg_data_client)
                    .await
            }
            RequestType::Hello => {
                ismaster::process(
                    "isWritablePrimary",
                    request,
                    connection_context,
                    &dynamic_config,
                )
                .await
            }
            RequestType::HostInfo => constant::process_host_info(),
            RequestType::Insert => {
                data_management::process_insert(
                    request,
                    request_info,
                    connection_context,
                    &pg_data_client,
                )
                .await
            }
            RequestType::IsDBGrid => constant::process_is_db_grid(connection_context),
            RequestType::IsMaster => {
                ismaster::process("ismaster", request, connection_context, &dynamic_config).await
            }
            RequestType::ListCollections => {
                data_management::process_list_collections(
                    request,
                    request_info,
                    connection_context,
                    &pg_data_client,
                )
                .await
            }
            RequestType::ListDatabases => {
                data_management::process_list_databases(
                    request,
                    request_info,
                    connection_context,
                    &pg_data_client,
                )
                .await
            }
            RequestType::ListIndexes => {
                indexing::process_list_indexes(
                    request,
                    request_info,
                    connection_context,
                    &pg_data_client,
                )
                .await
            }
            RequestType::Ping => Ok(constant::ok_response()),
            RequestType::SaslContinue | RequestType::SaslStart | RequestType::Logout => {
                Err(DocumentDBError::internal_error(
                    "Command should have been handled by Auth".to_string(),
                ))
            }
            RequestType::Update => {
                data_management::process_update(
                    request,
                    request_info,
                    connection_context,
                    &pg_data_client,
                )
                .await
            }
            RequestType::Validate => {
                data_management::process_validate(
                    request,
                    request_info,
                    connection_context,
                    &pg_data_client,
                )
                .await
            }
            RequestType::DropIndexes => {
                indexing::process_drop_indexes(
                    request,
                    request_info,
                    connection_context,
                    &pg_data_client,
                )
                .await
            }
            RequestType::ShardCollection => {
                data_description::process_shard_collection(
                    request,
                    request_info,
                    connection_context,
                    false,
                    &pg_data_client,
                )
                .await
            }
            RequestType::ReIndex => {
                indexing::process_reindex(request_info, connection_context, &pg_data_client).await
            }
            RequestType::CurrentOp => {
                data_management::process_current_op(
                    request,
                    request_info,
                    connection_context,
                    &pg_data_client,
                )
                .await
            }
            RequestType::CollMod => {
                data_description::process_coll_mod(
                    request,
                    request_info,
                    connection_context,
                    &pg_data_client,
                )
                .await
            }
            RequestType::GetParameter => {
                data_management::process_get_parameter(
                    request,
                    request_info,
                    connection_context,
                    &pg_data_client,
                )
                .await
            }
            RequestType::KillCursors => {
                cursor::process_kill_cursors(request, connection_context).await
            }
            RequestType::DbStats => {
                data_management::process_db_stats(
                    request,
                    request_info,
                    connection_context,
                    &pg_data_client,
                )
                .await
            }
            RequestType::RenameCollection => {
                data_description::process_rename_collection(
                    request,
                    request_info,
                    connection_context,
                    &pg_data_client,
                )
                .await
            }
            RequestType::PrepareTransaction => constant::process_prepare_transaction(),
            RequestType::CommitTransaction => transaction::process_commit(connection_context).await,
            RequestType::AbortTransaction => transaction::process_abort(connection_context).await,
            RequestType::ListCommands => constant::list_commands(),
            RequestType::EndSessions => session::end_sessions(request, connection_context).await,
            RequestType::ReshardCollection => {
                data_description::process_shard_collection(
                    request,
                    request_info,
                    connection_context,
                    true,
                    &pg_data_client,
                )
                .await
            }
            RequestType::WhatsMyUri => constant::process_whats_my_uri(),
            RequestType::CreateUser => {
                users::process_create_user(
                    request,
                    request_info,
                    connection_context,
                    &pg_data_client,
                )
                .await
            }
            RequestType::DropUser => {
                users::process_drop_user(request, request_info, connection_context, &pg_data_client)
                    .await
            }
            RequestType::UpdateUser => {
                users::process_update_user(
                    request,
                    request_info,
                    connection_context,
                    &pg_data_client,
                )
                .await
            }
            RequestType::UsersInfo => {
                users::process_users_info(
                    request,
                    request_info,
                    connection_context,
                    &pg_data_client,
                )
                .await
            }
            RequestType::UnshardCollection => {
                data_description::process_unshard_collection(
                    request,
                    request_info,
                    connection_context,
                    &pg_data_client,
                )
                .await
            }
        };

        if response.is_ok()
            || start_time.elapsed()
                > Duration::from_secs(
                    connection_context
                        .service_context
                        .setup_configuration()
                        .postgres_command_timeout_secs(),
                )
        {
            return response;
        }

        let retry = match &response {
            // in the case of write conflict, we need to remove the transaction.
            Err(DocumentDBError::PostgresError(error, _)) => {
                retry_policy(&dynamic_config, error, request.request_type()).await
            }
            Err(DocumentDBError::PoolError(
                PoolError::PostCreateHook(HookError::Backend(error)),
                _,
            )) => retry_policy(&dynamic_config, error, request.request_type()).await,
            Err(DocumentDBError::PoolError(PoolError::Backend(error), _)) => {
                retry_policy(&dynamic_config, error, request.request_type()).await
            }
            // Any other errors are not retriable
            _ => Retry::None,
        };

        retries += 1;

        match retry {
            Retry::Short => {
                log::trace!("Retrying short: {retries}");
                tokio::time::sleep(Duration::from_millis(50)).await;
            }
            Retry::Long => {
                log::trace!("Retrying long: {retries}");
                tokio::time::sleep(Duration::from_secs(5)).await;
            }

            Retry::None => break response,
        }
    };

    if connection_context.transaction.is_some() {
        match result {
            Err(DocumentDBError::UntypedDocumentDBError(112, _, _, _))
            | Err(DocumentDBError::DocumentDBError(ErrorCode::WriteConflict, _, _))
            | Err(_)
                if request.request_type() == &RequestType::Find
                    || request.request_type() == &RequestType::Aggregate =>
            {
                transaction::process_abort(connection_context).await?;
            }
            _ => {}
        }
    }

    result
}

async fn retry_policy(
    dynamic_config: &Arc<dyn DynamicConfiguration>,
    error: &tokio_postgres::Error,
    request_type: &RequestType,
) -> Retry {
    if error.is_closed() {
        return Retry::Short;
    }
    match error.code() {
        Some(&SqlState::ADMIN_SHUTDOWN) => Retry::Short,
        Some(&SqlState::READ_ONLY_SQL_TRANSACTION) if dynamic_config.is_replica_cluster().await => {
            Retry::None
        }
        Some(&SqlState::READ_ONLY_SQL_TRANSACTION) => Retry::Long,
        Some(&SqlState::CONNECTION_FAILURE) => Retry::Long,
        Some(&SqlState::INVALID_AUTHORIZATION_SPECIFICATION) => Retry::Long,
        Some(&SqlState::T_R_DEADLOCK_DETECTED) if request_type == &RequestType::Update => {
            Retry::Long
        }
        _ => Retry::None,
    }
}
