/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/processor/process.rs
 *
 *-------------------------------------------------------------------------
 */
#![allow(clippy::unnecessary_to_owned)]

use std::{
    sync::Arc,
    time::{Duration, Instant},
};

use bson::{spec::ElementType, RawBsonRef, RawDocumentBuf};
use deadpool_postgres::{HookError, PoolError};
use tokio_postgres::error::SqlState;

use crate::{
    bson::convert_to_bool,
    configuration::DynamicConfiguration,
    context::ConnectionContext,
    error::{DocumentDBError, ErrorCode, Result},
    explain,
    postgres::PgDataClient,
    protocol,
    requests::{Request, RequestInfo, RequestType},
    responses::{PgResponse, Response},
};

use super::{constant, cursor, delete, indexing, ismaster, session, transaction, users};

enum Retry {
    Long,
    Short,
    None,
}

pub async fn process_request(
    request: &Request<'_>,
    request_info: &mut RequestInfo<'_>,
    connection_context: &mut ConnectionContext,
    pg_data_client: impl PgDataClient<'_>,
) -> Result<Response> {
    let dynamic_config = connection_context.dynamic_configuration();
    transaction::handle(request, request_info, connection_context, &pg_data_client).await?;
    let start_time = Instant::now();

    let mut retries = 0;
    let result = loop {
        let response = match request.request_type() {
            RequestType::Aggregate => {
                process_aggregate(request, request_info, connection_context, &pg_data_client).await
            }
            RequestType::BuildInfo => constant::process_build_info(&dynamic_config).await,
            RequestType::CollStats => {
                process_coll_stats(request, request_info, connection_context, &pg_data_client).await
            }
            RequestType::ConnectionStatus => constant::process_connection_status(),
            RequestType::Count => {
                process_count(request, request_info, connection_context, &pg_data_client).await
            }
            RequestType::Create => {
                process_create(request, request_info, connection_context, &pg_data_client).await
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
                delete::process_delete(
                    request,
                    request_info,
                    connection_context,
                    &dynamic_config,
                    &pg_data_client,
                )
                .await
            }
            RequestType::Distinct => {
                process_distinct(request, request_info, connection_context, &pg_data_client).await
            }
            RequestType::Drop => {
                delete::process_drop_collection(
                    request_info,
                    connection_context,
                    &dynamic_config,
                    &pg_data_client,
                )
                .await
            }
            RequestType::DropDatabase => {
                delete::process_drop_database(
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
                process_find(request, request_info, connection_context, &pg_data_client).await
            }
            RequestType::FindAndModify => {
                process_find_and_modify(request, request_info, connection_context, &pg_data_client)
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
                process_insert(request, request_info, connection_context, &pg_data_client).await
            }
            RequestType::IsDBGrid => constant::process_is_db_grid(connection_context),
            RequestType::IsMaster => {
                ismaster::process("ismaster", request, connection_context, &dynamic_config).await
            }
            RequestType::ListCollections => {
                process_list_collections(request, request_info, connection_context, &pg_data_client)
                    .await
            }
            RequestType::ListDatabases => {
                process_list_databases(request, request_info, connection_context, &pg_data_client)
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
                process_update(request, request_info, connection_context, &pg_data_client).await
            }
            RequestType::Validate => {
                process_validate(request, request_info, connection_context, &pg_data_client).await
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
                process_shard_collection(
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
                process_current_op(request, request_info, connection_context, &pg_data_client).await
            }
            RequestType::CollMod => {
                process_coll_mod(request, request_info, connection_context, &pg_data_client).await
            }
            RequestType::GetParameter => {
                process_get_parameter(request, request_info, connection_context, &pg_data_client)
                    .await
            }
            RequestType::KillCursors => {
                cursor::process_kill_cursors(request, connection_context).await
            }
            RequestType::DbStats => {
                process_db_stats(request, request_info, connection_context, &pg_data_client).await
            }
            RequestType::RenameCollection => {
                process_rename_collection(
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
                process_shard_collection(
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

async fn process_find(
    request: &Request<'_>,
    request_info: &mut RequestInfo<'_>,
    connection_context: &ConnectionContext,
    pg_data_client: &impl PgDataClient<'_>,
) -> Result<Response> {
    let (response, conn) = pg_data_client
        .execute_find(request, request_info, connection_context)
        .await?;

    cursor::save_cursor(connection_context, conn, &response, request_info).await?;
    Ok(Response::Pg(response))
}

async fn process_insert(
    request: &Request<'_>,
    request_info: &mut RequestInfo<'_>,
    connection_context: &ConnectionContext,
    pg_data_client: &impl PgDataClient<'_>,
) -> Result<Response> {
    let insert_rows = pg_data_client
        .execute_insert(request, request_info, connection_context)
        .await?;

    PgResponse::new(insert_rows)
        .transform_write_errors(connection_context)
        .await
}

async fn process_aggregate(
    request: &Request<'_>,
    request_info: &mut RequestInfo<'_>,
    connection_context: &ConnectionContext,
    pg_data_client: &impl PgDataClient<'_>,
) -> Result<Response> {
    let (response, conn) = pg_data_client
        .execute_aggregate(request, request_info, connection_context)
        .await?;
    cursor::save_cursor(connection_context, conn, &response, request_info).await?;
    Ok(Response::Pg(response))
}

async fn process_update(
    request: &Request<'_>,
    request_info: &mut RequestInfo<'_>,
    connection_context: &ConnectionContext,
    pg_data_client: &impl PgDataClient<'_>,
) -> Result<Response> {
    let update_rows = pg_data_client
        .execute_update(request, request_info, connection_context)
        .await?;

    PgResponse::new(update_rows)
        .transform_write_errors(connection_context)
        .await
}

async fn process_list_databases(
    request: &Request<'_>,
    request_info: &mut RequestInfo<'_>,
    connection_context: &ConnectionContext,
    pg_data_client: &impl PgDataClient<'_>,
) -> Result<Response> {
    pg_data_client
        .execute_list_databases(request, request_info, connection_context)
        .await
}

async fn process_list_collections(
    request: &Request<'_>,
    request_info: &mut RequestInfo<'_>,
    connection_context: &ConnectionContext,
    pg_data_client: &impl PgDataClient<'_>,
) -> Result<Response> {
    let (response, conn) = pg_data_client
        .execute_list_collections(request, request_info, connection_context)
        .await?;

    cursor::save_cursor(connection_context, conn, &response, request_info).await?;
    Ok(Response::Pg(response))
}

async fn process_validate(
    request: &Request<'_>,
    request_info: &mut RequestInfo<'_>,
    connection_context: &ConnectionContext,
    pg_data_client: &impl PgDataClient<'_>,
) -> Result<Response> {
    pg_data_client
        .execute_validate(request, request_info, connection_context)
        .await
}

async fn process_find_and_modify(
    request: &Request<'_>,
    request_info: &mut RequestInfo<'_>,
    connection_context: &ConnectionContext,
    pg_data_client: &impl PgDataClient<'_>,
) -> Result<Response> {
    pg_data_client
        .execute_find_and_modify(request, request_info, connection_context)
        .await
}

async fn process_distinct(
    request: &Request<'_>,
    request_info: &mut RequestInfo<'_>,
    connection_context: &ConnectionContext,
    pg_data_client: &impl PgDataClient<'_>,
) -> Result<Response> {
    pg_data_client
        .execute_distinct_query(request, request_info, connection_context)
        .await
}

async fn process_count(
    request: &Request<'_>,
    request_info: &mut RequestInfo<'_>,
    context: &ConnectionContext,
    pg_data_client: &impl PgDataClient<'_>,
) -> Result<Response> {
    // we need to ensure that the collection is correctly set up before we can execute the count query
    let _ = request_info.collection()?;

    pg_data_client
        .execute_count_query(request, request_info, context)
        .await
}

async fn process_create(
    request: &Request<'_>,
    request_info: &mut RequestInfo<'_>,
    context: &ConnectionContext,
    pg_data_client: &impl PgDataClient<'_>,
) -> Result<Response> {
    pg_data_client
        .execute_create_collection(request, request_info, context)
        .await
}

fn convert_to_scale(scale: RawBsonRef) -> Result<f64> {
    match scale.element_type() {
        ElementType::Double => Ok(scale.as_f64().expect("Type of bson was checked.")),
        ElementType::Int32 => Ok(f64::from(
            scale.as_i32().expect("Type of bson was checked."),
        )),
        ElementType::Int64 => Ok(scale.as_i64().expect("Type of bson was checked.") as f64),
        ElementType::Undefined => Ok(1.0),
        ElementType::Null => Ok(1.0),
        _ => Err(DocumentDBError::documentdb_error(
            ErrorCode::TypeMismatch,
            format!(
                "Unexpected bson type for scale: {:#?}",
                scale.element_type()
            ),
        )),
    }
}

async fn process_coll_stats(
    request: &Request<'_>,
    request_info: &mut RequestInfo<'_>,
    connection_context: &ConnectionContext,
    pg_data_client: &impl PgDataClient<'_>,
) -> Result<Response> {
    // allow floats and ints, the backend will truncate
    let scale = if let Some(scale) = request.document().get("scale")? {
        convert_to_scale(scale)?
    } else {
        1.0
    };

    pg_data_client
        .execute_coll_stats(request_info, scale, connection_context)
        .await
}

async fn process_db_stats(
    request: &Request<'_>,
    request_info: &mut RequestInfo<'_>,
    context: &ConnectionContext,
    pg_data_client: &impl PgDataClient<'_>,
) -> Result<Response> {
    // allow floats and ints, the backend will truncate
    let scale = if let Some(scale) = request.document().get("scale")? {
        convert_to_scale(scale)?
    } else {
        1.0
    };

    pg_data_client
        .execute_db_stats(request_info, scale, context)
        .await
}

async fn process_shard_collection(
    request: &Request<'_>,
    request_info: &mut RequestInfo<'_>,
    context: &ConnectionContext,
    reshard: bool,
    pg_data_client: &impl PgDataClient<'_>,
) -> Result<Response> {
    let namespace = request_info.collection()?.to_string();
    let (db, collection) = protocol::extract_namespace(namespace.as_str())?;
    let key = request
        .document()
        .get_document("key")
        .map_err(DocumentDBError::parse_failure())?;

    let _ = pg_data_client
        .execute_shard_collection(request_info, db, collection, key, reshard, context)
        .await?;

    Ok(Response::ok())
}

async fn process_rename_collection(
    request: &Request<'_>,
    request_info: &mut RequestInfo<'_>,
    connection_context: &ConnectionContext,
    pg_data_client: &impl PgDataClient<'_>,
) -> Result<Response> {
    let mut source: Option<String> = None;
    let mut target: Option<String> = None;
    let mut drop_target = false;
    request.extract_fields(|k, v| {
        match k {
            "renameCollection" => {
                source = Some(
                    v.as_str()
                        .ok_or(DocumentDBError::bad_value(
                            "renameCollection was not a string".to_string(),
                        ))?
                        .to_string(),
                )
            }
            "to" => {
                target = Some(
                    v.as_str()
                        .ok_or(DocumentDBError::bad_value(
                            "renameCollection was not a string".to_string(),
                        ))?
                        .to_string(),
                )
            }
            "dropTarget" => {
                drop_target = v.as_bool().unwrap_or(false);
            }
            _ => {}
        };
        Ok(())
    })?;

    let source = source.ok_or(DocumentDBError::bad_value(
        "renameCollection missing".to_string(),
    ))?;
    let target = target.ok_or(DocumentDBError::bad_value("to missing".to_string()))?;

    let (source_db, source_coll) = protocol::extract_namespace(&source)?;
    let (target_db, target_coll) = protocol::extract_namespace(&target)?;

    if source_db != target_db {
        return Err(DocumentDBError::documentdb_error(
            ErrorCode::CommandNotSupported,
            "RenameCollection cannot change databases".to_string(),
        ));
    }

    if source_coll == target_coll {
        return Err(DocumentDBError::documentdb_error(
            ErrorCode::IllegalOperation,
            "Can't rename a collection to itself".to_string(),
        ));
    }

    let _ = pg_data_client
        .execute_rename_collection(
            request_info,
            source_db,
            source_coll,
            target_coll,
            drop_target,
            connection_context,
        )
        .await?;
    Ok(Response::ok())
}

async fn process_current_op(
    request: &Request<'_>,
    request_info: &mut RequestInfo<'_>,
    context: &ConnectionContext,
    pg_data_client: &impl PgDataClient<'_>,
) -> Result<Response> {
    let mut filter = RawDocumentBuf::new();
    let mut all = false;
    let mut own_ops = false;
    request.extract_fields(|k, v| {
        match k {
            "all" => all = v.as_bool().unwrap_or(false),
            "ownOps" => own_ops = v.as_bool().unwrap_or(false),
            _ => filter.append(k, v.to_raw_bson()),
        }
        Ok(())
    })?;

    pg_data_client
        .execute_current_op(request_info, &filter, all, own_ops, context)
        .await
}

async fn process_coll_mod(
    request: &Request<'_>,
    request_info: &mut RequestInfo<'_>,
    connection_context: &ConnectionContext,
    pg_data_client: &impl PgDataClient<'_>,
) -> Result<Response> {
    pg_data_client
        .execute_coll_mod(request, request_info, connection_context)
        .await
}

async fn get_parameter(
    connection_context: &ConnectionContext,
    request_info: &mut RequestInfo<'_>,
    all: bool,
    show_details: bool,
    params: Vec<String>,
    pg_data_client: &impl PgDataClient<'_>,
) -> Result<Response> {
    pg_data_client
        .execute_get_parameter(request_info, all, show_details, params, connection_context)
        .await
}

async fn process_get_parameter(
    request: &Request<'_>,
    request_info: &mut RequestInfo<'_>,
    connection_context: &ConnectionContext,
    pg_data_client: &impl PgDataClient<'_>,
) -> Result<Response> {
    let mut all_parameters = false;
    let mut show_details = false;
    let mut star = false;
    let mut params = Vec::new();
    request.extract_fields(|k, v| {
        match k {
            "getParameter" => {
                if v.as_str().is_some_and(|s| s == "*") {
                    star = true;
                } else if let Some(doc) = v.as_document() {
                    for pair in doc {
                        let (k, v) = pair?;
                        match k {
                            "allParameters" => {
                                all_parameters =
                                    convert_to_bool(v).ok_or(DocumentDBError::type_mismatch(
                                        "allParameters should be a bool".to_string(),
                                    ))?
                            }
                            "showDetails" => {
                                show_details =
                                    convert_to_bool(v).ok_or(DocumentDBError::type_mismatch(
                                        "showDetails should be convertible to a bool".to_string(),
                                    ))?
                            }
                            _ => {}
                        }
                    }
                }
            }
            _ => params.push(k.to_string()),
        }
        Ok(())
    })?;
    if request_info.db()? != "admin" {
        return Err(DocumentDBError::documentdb_error(
            ErrorCode::Unauthorized,
            "getParameter may only be run against the admin database.".to_string(),
        ));
    }

    if star {
        return get_parameter(
            connection_context,
            request_info,
            true,
            false,
            vec![],
            pg_data_client,
        )
        .await;
    }

    get_parameter(
        connection_context,
        request_info,
        all_parameters,
        show_details,
        params,
        pg_data_client,
    )
    .await
}
