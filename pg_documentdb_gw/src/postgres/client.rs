/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/postgres/client.rs
 *
 *-------------------------------------------------------------------------
 */

use std::time::Duration;

use super::{PgDocument, QueryCatalog};
use crate::{
    configuration::SetupConfiguration,
    error::Result,
    requests::{RequestInfo, RequestIntervalKind},
};
use deadpool_postgres::{Hook, HookError, Runtime};
use tokio::task::JoinHandle;
use tokio_postgres::{
    types::{ToSql, Type},
    NoTls, Row,
};

pub type InnerConnection = deadpool_postgres::Object;

pub fn pg_configuration(sc: &dyn SetupConfiguration) -> tokio_postgres::Config {
    let mut config = tokio_postgres::Config::new();
    config
        .host(sc.postgres_host_name())
        .port(sc.postgres_port())
        .dbname(sc.postgres_database());
    config
}

// Ensures search_path is set on all acquired clients
#[derive(Debug)]
pub struct ConnectionPool {
    pool: deadpool_postgres::Pool,
    _reaper: JoinHandle<()>,
}

impl ConnectionPool {
    pub fn new_with_user(
        sc: &dyn SetupConfiguration,
        query_catalog: &QueryCatalog,
        user: &str,
        pass: Option<&str>,
        application_name: String,
        max_size: usize,
    ) -> Result<Self> {
        let mut config = pg_configuration(sc);
        config.application_name(&application_name);

        config.user(user);
        if let Some(pass) = pass {
            config.password(pass);
        }
        let manager = deadpool_postgres::Manager::new(config, NoTls);

        // Clone query_catalog to be used in the post_create hook
        let query_catalog_clone = query_catalog.clone();
        let builder = deadpool_postgres::Pool::builder(manager)
            .post_create(Hook::async_fn(move |m, _| {
                let timeout_ms = Duration::from_secs(120).as_millis().to_string();
                let query = query_catalog_clone.set_search_path_and_timeout(&timeout_ms);
                Box::pin(async move {
                    m.batch_execute(&query).await.map_err(HookError::Backend)?;
                    Ok(())
                })
            }))
            .create_timeout(Some(Duration::from_secs(5)))
            .wait_timeout(Some(Duration::from_secs(5)))
            .recycle_timeout(Some(Duration::from_secs(5)))
            .runtime(Runtime::Tokio1)
            .max_size(max_size);
        let pool = builder.build()?;

        let pool_copy = pool.clone();
        let reaper = tokio::spawn(async move {
            let mut interval = tokio::time::interval(Duration::from_secs(30));
            let max_age = Duration::from_secs(60);
            loop {
                interval.tick().await;
                pool_copy.retain(|_, metrics| metrics.last_used() < max_age);
            }
        });

        Ok(ConnectionPool {
            pool,
            _reaper: reaper,
        })
    }

    pub async fn get_inner_connection(&self) -> Result<InnerConnection> {
        Ok(self.pool.get().await?)
    }
}

// Provides functions which coerce bson to BYTEA. Any statement binding a PgDocument should use query_typed and not query
// WrongType { postgres: Other(Other { name: "bson", oid: 18934, kind: Simple, schema: "schema_name" }), rust: "document_gateway::postgres::document::PgDocument" })
// Will be occur if the wrong one is used.
#[derive(Debug)]
pub struct Connection {
    inner_conn: InnerConnection,
    pub in_transaction: bool,
}

pub enum TimeoutType {
    // Transaction timeout uses SET LOCAL inside a transaction, it cannot be used if cursors need to be persisted
    Transaction,

    // Command timeout uses SET outside of a transaction, should be used when a transaction cannot be started
    Command,
}

pub struct Timeout {
    timeout_type: TimeoutType,
    max_time_ms: i64,
}

impl Timeout {
    pub fn command(max_time_ms: Option<i64>) -> Option<Self> {
        max_time_ms.map(|m| Timeout {
            timeout_type: TimeoutType::Command,
            max_time_ms: m,
        })
    }

    pub fn transaction(max_time_ms: Option<i64>) -> Option<Self> {
        max_time_ms.map(|m| Timeout {
            timeout_type: TimeoutType::Transaction,
            max_time_ms: m,
        })
    }
}

impl Connection {
    async fn query_internal(
        &self,
        query: &str,
        parameter_types: &[Type],
        params: &[&(dyn ToSql + Sync)],
    ) -> Result<Vec<Row>> {
        let statement = self
            .inner_conn
            .prepare_typed_cached(query, parameter_types)
            .await?;
        Ok(self.inner_conn.query(&statement, params).await?)
    }

    #[allow(unused_mut)]
    pub async fn query(
        &self,
        query: &str,
        parameter_types: &[Type],
        params: &[&(dyn ToSql + Sync)],
        timeout: Option<Timeout>,
        request_info: &mut RequestInfo<'_>,
    ) -> Result<Vec<Row>> {
        match timeout {
            Some(Timeout {
                timeout_type: _,
                max_time_ms,
            }) if self.in_transaction => {
                let set_timeout_start = request_info.request_tracker.start_timer();
                self.inner_conn
                    .batch_execute(&format!("set local statement_timeout to {}", max_time_ms))
                    .await?;
                request_info.request_tracker.record_duration(
                    RequestIntervalKind::PostgresSetStatementTimeout,
                    set_timeout_start,
                );

                let request_start = request_info.request_tracker.start_timer();
                let results = self.query_internal(query, parameter_types, params).await;
                request_info
                    .request_tracker
                    .record_duration(RequestIntervalKind::ProcessRequest, request_start);

                let set_timeout_start = request_info.request_tracker.start_timer();
                self.inner_conn
                    .batch_execute(&format!(
                        "set local statement_timeout to {}",
                        Duration::from_secs(120).as_millis()
                    ))
                    .await?;
                request_info.request_tracker.record_duration(
                    RequestIntervalKind::PostgresSetStatementTimeout,
                    set_timeout_start,
                );

                Ok(results?)
            }
            Some(Timeout {
                timeout_type: TimeoutType::Transaction,
                max_time_ms,
            }) => {
                let begin_transaction_start = request_info.request_tracker.start_timer();
                self.inner_conn.batch_execute("BEGIN").await?;
                request_info.request_tracker.record_duration(
                    RequestIntervalKind::PostgresBeginTransaction,
                    begin_transaction_start,
                );

                let set_timeout_start = request_info.request_tracker.start_timer();
                self.inner_conn
                    .batch_execute(&format!("set local statement_timeout to {}", max_time_ms))
                    .await?;
                request_info.request_tracker.record_duration(
                    RequestIntervalKind::PostgresSetStatementTimeout,
                    set_timeout_start,
                );

                let request_start = request_info.request_tracker.start_timer();
                let results = match self.query_internal(query, parameter_types, params).await {
                    Ok(results) => Ok(results),
                    Err(e) => {
                        self.inner_conn.batch_execute("ROLLBACK").await?;
                        Err(e)
                    }
                }?;
                request_info
                    .request_tracker
                    .record_duration(RequestIntervalKind::ProcessRequest, request_start);

                let commit_start = request_info.request_tracker.start_timer();
                self.inner_conn.batch_execute("COMMIT").await?;
                request_info
                    .request_tracker
                    .record_duration(RequestIntervalKind::PostgresTransactionCommit, commit_start);

                Ok(results)
            }
            Some(Timeout {
                timeout_type: TimeoutType::Command,
                max_time_ms,
            }) => {
                let set_timeout_start = request_info.request_tracker.start_timer();
                self.inner_conn
                    .batch_execute(&format!("set statement_timeout to {}", max_time_ms))
                    .await?;
                request_info.request_tracker.record_duration(
                    RequestIntervalKind::PostgresSetStatementTimeout,
                    set_timeout_start,
                );

                let request_start = request_info.request_tracker.start_timer();
                let results = self.query_internal(query, parameter_types, params).await;
                request_info
                    .request_tracker
                    .record_duration(RequestIntervalKind::ProcessRequest, request_start);

                let set_timeout_start = request_info.request_tracker.start_timer();
                self.inner_conn
                    .batch_execute(&format!(
                        "set statement_timeout to {}",
                        Duration::from_secs(120).as_millis()
                    ))
                    .await?;
                request_info.request_tracker.record_duration(
                    RequestIntervalKind::PostgresSetStatementTimeout,
                    set_timeout_start,
                );

                Ok(results?)
            }
            None => {
                let request_start = request_info.request_tracker.start_timer();
                let results = self.query_internal(query, parameter_types, params).await;
                request_info
                    .request_tracker
                    .record_duration(RequestIntervalKind::ProcessRequest, request_start);

                results
            }
        }
    }

    pub async fn query_db_bson(
        &self,
        query: &str,
        db: &str,
        bson: &PgDocument<'_>,
        timeout: Option<Timeout>,
        request_info: &mut RequestInfo<'_>,
    ) -> Result<Vec<Row>> {
        self.query(
            query,
            &[Type::TEXT, Type::BYTEA],
            &[&db, bson],
            timeout,
            request_info,
        )
        .await
    }

    pub async fn batch_execute(&self, query: &str) -> Result<()> {
        Ok(self.inner_conn.batch_execute(query).await?)
    }

    pub fn new(conn: InnerConnection, in_transaction: bool) -> Self {
        Connection {
            inner_conn: conn,
            in_transaction,
        }
    }

    // Should avoid using this in general - Used for explain which has special handling
    pub fn get_inner(&mut self) -> &mut InnerConnection {
        &mut self.inner_conn
    }
}
