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

use bson::{spec::ElementType, RawBsonRef, RawDocumentBuf};
use deadpool_postgres::{HookError, PoolError};
use tokio_postgres::{error::SqlState, types::Type};

use crate::{
    bson::convert_to_bool,
    configuration::DynamicConfiguration,
    context::ConnectionContext,
    error::{DocumentDBError, ErrorCode, Result},
    explain,
    postgres::{PgDocument, Timeout},
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
    request_info: &RequestInfo<'_>,
    connection_context: &mut ConnectionContext,
) -> Result<Response> {
    let dynamic_config = connection_context.dynamic_configuration();
    transaction::handle(request, request_info, connection_context).await?;
    let start_time = Instant::now();

    let mut retries = 0;
    let result = loop {
        let response = match request.request_type() {
            RequestType::Aggregate => {
                process_aggregate(request, request_info, connection_context).await
            }
            RequestType::BuildInfo => constant::process_build_info(&dynamic_config).await,
            RequestType::CollStats => {
                process_coll_stats(request, request_info, connection_context).await
            }
            RequestType::ConnectionStatus => constant::process_connection_status(),
            RequestType::Count => process_count(request, request_info, connection_context).await,
            RequestType::Create => process_create(request, request_info, connection_context).await,
            RequestType::CreateIndexes => {
                indexing::process_create_indexes(
                    request,
                    request_info,
                    connection_context,
                    &dynamic_config,
                )
                .await
            }
            RequestType::Delete => {
                delete::process_delete(request, request_info, connection_context, &dynamic_config)
                    .await
            }
            RequestType::Distinct => {
                process_distinct(request, request_info, connection_context).await
            }
            RequestType::Drop => {
                delete::process_drop_collection(
                    request,
                    request_info,
                    connection_context,
                    &dynamic_config,
                )
                .await
            }
            RequestType::DropDatabase => {
                delete::process_drop_database(
                    request,
                    request_info,
                    connection_context,
                    &dynamic_config,
                )
                .await
            }
            RequestType::Explain => {
                explain::process_explain(request, request_info, None, connection_context).await
            }
            RequestType::Find => process_find(request, request_info, connection_context).await,
            RequestType::FindAndModify => {
                process_find_and_modify(request, request_info, connection_context).await
            }
            RequestType::GetCmdLineOpts => constant::process_get_cmd_line_opts(),
            RequestType::GetDefaultRWConcern => {
                constant::process_get_rw_concern(request, request_info, connection_context)
            }
            RequestType::GetLog => constant::process_get_log(),
            RequestType::GetMore => {
                cursor::process_get_more(request, request_info, connection_context).await
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
            RequestType::Insert => process_insert(request, request_info, connection_context).await,
            RequestType::IsDBGrid => constant::process_is_db_grid(connection_context),
            RequestType::IsMaster => {
                ismaster::process("ismaster", request, connection_context, &dynamic_config).await
            }
            RequestType::ListCollections => {
                process_list_collections(request, request_info, connection_context).await
            }
            RequestType::ListDatabases => {
                process_list_databases(request, request_info, connection_context).await
            }
            RequestType::ListIndexes => {
                indexing::process_list_indexes(request, request_info, connection_context).await
            }
            RequestType::Ping => Ok(constant::ok_response()),
            RequestType::SaslContinue | RequestType::SaslStart | RequestType::Logout => {
                Err(DocumentDBError::internal_error(
                    "Command should have been handled by Auth".to_string(),
                ))
            }
            RequestType::Update => process_update(request, request_info, connection_context).await,
            RequestType::Validate => {
                process_validate(request, request_info, connection_context).await
            }
            RequestType::DropIndexes => {
                indexing::process_drop_indexes(request, request_info, connection_context).await
            }
            RequestType::ShardCollection => {
                process_shard_collection(request, request_info, connection_context, false).await
            }
            RequestType::ReIndex => {
                indexing::process_reindex(request, request_info, connection_context).await
            }
            RequestType::CurrentOp => {
                process_current_op(request, request_info, connection_context).await
            }
            RequestType::CollMod => {
                process_coll_mod(request, request_info, connection_context).await
            }
            RequestType::GetParameter => {
                process_get_parameter(request, request_info, connection_context).await
            }
            RequestType::KillCursors => {
                cursor::process_kill_cursors(request, connection_context).await
            }
            RequestType::DbStats => {
                process_db_stats(request, request_info, connection_context).await
            }
            RequestType::RenameCollection => {
                process_rename_collection(request, request_info, connection_context).await
            }
            RequestType::PrepareTransaction => constant::process_prepare_transaction(),
            RequestType::CommitTransaction => transaction::process_commit(connection_context).await,
            RequestType::AbortTransaction => transaction::process_abort(connection_context).await,
            RequestType::ListCommands => constant::list_commands(),
            RequestType::EndSessions => session::end_sessions(request, connection_context).await,
            RequestType::ReshardCollection => {
                process_shard_collection(request, request_info, connection_context, true).await
            }
            RequestType::WhatsMyUri => constant::process_whats_my_uri(),
            RequestType::CreateUser => {
                users::process_create_user(request, connection_context).await
            }
            RequestType::DropUser => users::process_drop_user(request, connection_context).await,
            RequestType::UpdateUser => {
                users::process_update_user(request, connection_context).await
            }
            RequestType::UsersInfo => users::process_users_info(request, connection_context).await,
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
    request_info: &RequestInfo<'_>,
    context: &ConnectionContext,
) -> Result<Response> {
    let client = context.pg().await?;

    let results = client
        .query_db_bson(
            context
                .service_context
                .query_catalog()
                .find_cursor_first_page(),
            request_info.db()?,
            &PgDocument(request.document()),
            Timeout::command(request_info.max_time_ms),
        )
        .await?;

    let response = PgResponse::new(results);
    cursor::save_cursor(context, client, &response, request_info).await?;
    Ok(Response::Pg(response))
}

async fn process_insert(
    request: &Request<'_>,
    request_info: &RequestInfo<'_>,
    connection_context: &ConnectionContext,
) -> Result<Response> {
    let results = connection_context
        .pg()
        .await?
        .query(
            connection_context.service_context.query_catalog().insert(),
            &[Type::TEXT, Type::BYTEA, Type::BYTEA],
            &[
                &request_info.db()?,
                &PgDocument(request.document()),
                &request.extra(),
            ],
            Timeout::transaction(request_info.max_time_ms),
        )
        .await?;

    PgResponse::new(results)
        .transform_write_errors(connection_context)
        .await
}

async fn process_aggregate(
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
                .aggregate_cursor_first_page(),
            request_info.db()?,
            &PgDocument(request.document()),
            Timeout::command(request_info.max_time_ms),
        )
        .await?;

    let response = PgResponse::new(results);
    cursor::save_cursor(context, client, &response, request_info).await?;
    Ok(Response::Pg(response))
}

async fn process_update(
    request: &Request<'_>,
    request_info: &RequestInfo<'_>,
    connection_context: &ConnectionContext,
) -> Result<Response> {
    let results = connection_context
        .pg()
        .await?
        .query(
            connection_context
                .service_context
                .query_catalog()
                .process_update(),
            &[Type::TEXT, Type::BYTEA, Type::BYTEA],
            &[
                &request_info.db()?,
                &PgDocument(request.document()),
                &request.extra(),
            ],
            Timeout::transaction(request_info.max_time_ms),
        )
        .await?;

    PgResponse::new(results)
        .transform_write_errors(connection_context)
        .await
}

async fn process_list_databases(
    request: &Request<'_>,
    request_info: &RequestInfo<'_>,
    context: &ConnectionContext,
) -> Result<Response> {
    // TODO: Handle the case where !nameOnly - the legacy gateway simply returns 0s in the appropriate format
    let filter = request.document().get_document("filter").ok();

    let filter_string = filter.map_or("", |_| "WHERE document @@ $1");
    let query = context
        .service_context
        .query_catalog()
        .list_databases(filter_string);
    let results = match filter {
        None => {
            context
                .pg()
                .await?
                .query(
                    &query,
                    &[],
                    &[],
                    Timeout::transaction(request_info.max_time_ms),
                )
                .await?
        }
        Some(filter) => {
            let doc = PgDocument(filter);
            context
                .pg()
                .await?
                .query(
                    &query,
                    &[Type::BYTEA],
                    &[&doc],
                    Timeout::transaction(request_info.max_time_ms),
                )
                .await?
        }
    };

    Ok(Response::Pg(PgResponse::new(results)))
}

async fn process_list_collections(
    request: &Request<'_>,
    request_info: &RequestInfo<'_>,
    context: &ConnectionContext,
) -> Result<Response> {
    let client = context.pg().await?;

    let results = client
        .query_db_bson(
            context.service_context.query_catalog().list_collections(),
            request_info.db()?,
            &PgDocument(request.document()),
            Timeout::transaction(request_info.max_time_ms),
        )
        .await?;

    let response = PgResponse::new(results);
    cursor::save_cursor(context, client, &response, request_info).await?;
    Ok(Response::Pg(response))
}

async fn process_validate(
    request: &Request<'_>,
    request_info: &RequestInfo<'_>,
    context: &ConnectionContext,
) -> Result<Response> {
    let results = context
        .pg()
        .await?
        .query_db_bson(
            context.service_context.query_catalog().validate(),
            request_info.db()?,
            &PgDocument(request.document()),
            Timeout::transaction(request_info.max_time_ms),
        )
        .await?;
    Ok(Response::Pg(PgResponse::new(results)))
}

async fn process_find_and_modify(
    request: &Request<'_>,
    request_info: &RequestInfo<'_>,
    context: &ConnectionContext,
) -> Result<Response> {
    let results = context
        .pg()
        .await?
        .query_db_bson(
            context.service_context.query_catalog().find_and_modify(),
            request_info.db()?,
            &PgDocument(request.document()),
            Timeout::transaction(request_info.max_time_ms),
        )
        .await?;
    Ok(Response::Pg(PgResponse::new(results)))
}

async fn process_distinct(
    request: &Request<'_>,
    request_info: &RequestInfo<'_>,
    context: &ConnectionContext,
) -> Result<Response> {
    let results = context
        .pg()
        .await?
        .query_db_bson(
            context.service_context.query_catalog().distinct_query(),
            request_info.db()?,
            &PgDocument(request.document()),
            Timeout::transaction(request_info.max_time_ms),
        )
        .await?;
    Ok(Response::Pg(PgResponse::new(results)))
}

async fn process_count(
    request: &Request<'_>,
    request_info: &RequestInfo<'_>,
    context: &ConnectionContext,
) -> Result<Response> {
    let _ = request_info.collection()?;
    let results = context
        .pg()
        .await?
        .query_db_bson(
            context.service_context.query_catalog().count_query(),
            request_info.db()?,
            &PgDocument(request.document()),
            Timeout::transaction(request_info.max_time_ms),
        )
        .await?;
    Ok(Response::Pg(PgResponse::new(results)))
}

async fn process_create(
    request: &Request<'_>,
    request_info: &RequestInfo<'_>,
    context: &ConnectionContext,
) -> Result<Response> {
    let results = context
        .pg()
        .await?
        .query_db_bson(
            context
                .service_context
                .query_catalog()
                .create_collection_view(),
            request_info.db()?,
            &PgDocument(request.document()),
            Timeout::transaction(request_info.max_time_ms),
        )
        .await?;
    Ok(Response::Pg(PgResponse::new(results)))
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
    request_info: &RequestInfo<'_>,
    context: &ConnectionContext,
) -> Result<Response> {
    // allow floats and ints, the backend will truncate
    let scale = if let Some(scale) = request.document().get("scale")? {
        convert_to_scale(scale)?
    } else {
        1.0
    };
    let results = context
        .pg()
        .await?
        .query(
            context.service_context.query_catalog().coll_stats(),
            &[Type::TEXT, Type::TEXT, Type::FLOAT8],
            &[&request_info.db()?, &request_info.collection()?, &scale],
            Timeout::transaction(request_info.max_time_ms),
        )
        .await?;
    Ok(Response::Pg(PgResponse::new(results)))
}

async fn process_db_stats(
    request: &Request<'_>,
    request_info: &RequestInfo<'_>,
    context: &ConnectionContext,
) -> Result<Response> {
    // allow floats and ints, the backend will truncate
    let scale = if let Some(scale) = request.document().get("scale")? {
        convert_to_scale(scale)?
    } else {
        1.0
    };

    let results = context
        .pg()
        .await?
        .query(
            context.service_context.query_catalog().db_stats(),
            &[Type::TEXT, Type::FLOAT8, Type::BOOL],
            &[&request_info.db()?, &scale, &false],
            Timeout::transaction(request_info.max_time_ms),
        )
        .await?;
    Ok(Response::Pg(PgResponse::new(results)))
}

async fn process_shard_collection(
    request: &Request<'_>,
    request_info: &RequestInfo<'_>,
    context: &ConnectionContext,
    reshard: bool,
) -> Result<Response> {
    let (db, collection) = protocol::extract_namespace(request_info.collection()?)?;
    let key = request
        .document()
        .get_document("key")
        .map_err(DocumentDBError::parse_failure())?;
    let _ = context
        .pg()
        .await?
        .query(
            context.service_context.query_catalog().shard_collection(),
            &[Type::TEXT, Type::TEXT, Type::BYTEA, Type::BOOL],
            &[&db, &collection, &PgDocument(key), &reshard],
            Timeout::transaction(request_info.max_time_ms),
        )
        .await?;
    Ok(Response::ok())
}

async fn process_rename_collection(
    request: &Request<'_>,
    request_info: &RequestInfo<'_>,
    context: &ConnectionContext,
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

    let _ = context
        .pg()
        .await?
        .query(
            context.service_context.query_catalog().rename_collection(),
            &[Type::TEXT, Type::TEXT, Type::TEXT, Type::BOOL],
            &[&source_db, &source_coll, &target_coll, &drop_target],
            Timeout::transaction(request_info.max_time_ms),
        )
        .await?;
    Ok(Response::ok())
}

async fn process_current_op(
    request: &Request<'_>,
    request_info: &RequestInfo<'_>,
    context: &ConnectionContext,
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

    let results = context
        .pg()
        .await?
        .query(
            context.service_context.query_catalog().current_op(),
            &[Type::BYTEA, Type::BOOL, Type::BOOL],
            &[&PgDocument(&filter), &all, &own_ops],
            Timeout::transaction(request_info.max_time_ms),
        )
        .await?;
    Ok(Response::Pg(PgResponse::new(results)))
}

async fn process_coll_mod(
    request: &Request<'_>,
    request_info: &RequestInfo<'_>,
    context: &ConnectionContext,
) -> Result<Response> {
    let results = context
        .pg()
        .await?
        .query(
            context.service_context.query_catalog().coll_mod(),
            &[Type::TEXT, Type::TEXT, Type::BYTEA],
            &[
                &request_info.db()?,
                &request_info.collection()?,
                &PgDocument(request.document()),
            ],
            Timeout::transaction(request_info.max_time_ms),
        )
        .await?;
    Ok(Response::Pg(PgResponse::new(results)))
}

async fn get_parameter(
    context: &ConnectionContext,
    request_info: &RequestInfo<'_>,
    all: bool,
    show_details: bool,
    params: Vec<String>,
) -> Result<Response> {
    let results = context
        .pg()
        .await?
        .query(
            context.service_context.query_catalog().get_parameter(),
            &[Type::BOOL, Type::BOOL, Type::TEXT_ARRAY],
            &[&all, &show_details, &params],
            Timeout::transaction(request_info.max_time_ms),
        )
        .await?;

    Ok(Response::Pg(PgResponse::new(results)))
}

async fn process_get_parameter(
    request: &Request<'_>,
    request_info: &RequestInfo<'_>,
    context: &ConnectionContext,
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
        return get_parameter(context, request_info, true, false, vec![]).await;
    }

    get_parameter(context, request_info, all_parameters, show_details, params).await
}
