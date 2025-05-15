/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/context/connection.rs
 *
 *-------------------------------------------------------------------------
 */

use std::{
    net::SocketAddr,
    sync::{
        atomic::{AtomicI64, Ordering},
        Arc,
    },
    time::Instant,
};

use bson::RawDocumentBuf;

use crate::telemetry::TelemetryProvider;
use crate::{
    auth::AuthState,
    configuration::DynamicConfiguration,
    error::{DocumentDBError, Result},
    postgres::Connection,
};

use super::{Cursor, CursorStoreEntry, ServiceContext};

pub struct ConnectionContext {
    pub start_time: Instant,
    pub connection_id: i64,
    pub service_context: Arc<ServiceContext>,
    pub auth_state: AuthState,
    pub requires_response: bool,
    pub client_information: Option<RawDocumentBuf>,
    pub transaction: Option<(Vec<u8>, i64)>,
    pub telemetry_provider: Option<Box<dyn TelemetryProvider>>,
    pub ip: SocketAddr,
    pub cipher_type: i32,
    pub ssl_protocol: String,
}

static CONNECTION_ID: AtomicI64 = AtomicI64::new(0);

impl ConnectionContext {
    pub async fn pull_connection(&self) -> Result<Arc<Connection>> {
        if let Some((session_id, _)) = self.transaction.as_ref() {
            let transaction_store = self.service_context.transaction_store();

            if let Some(conn) = transaction_store.get_connection(session_id).await {
                return Ok(conn);
            }
        }
        Ok(Arc::new(
            self.pull_connection_without_transaction(false).await?,
        ))
    }

    pub async fn get_cursor(&self, id: i64, user: &str) -> Option<CursorStoreEntry> {
        // If there is a transaction, get the cursor to its store
        if let Some((session_id, _)) = self.transaction.as_ref() {
            let transaction_store = self.service_context.transaction_store();
            if let Some((_, transaction)) =
                transaction_store.transactions.read().await.get(session_id)
            {
                return transaction.cursors.get_cursor((id, user.to_string())).await;
            }
        }

        self.service_context.get_cursor(id, user).await
    }

    pub async fn add_cursor(
        &self,
        conn: Option<Arc<Connection>>,
        cursor: Cursor,
        user: &str,
        db: &str,
        collection: &str,
        session_id: Option<Vec<u8>>,
    ) {
        let key = (cursor.cursor_id, user.to_string());
        let value = CursorStoreEntry {
            conn,
            cursor,
            db: db.to_string(),
            collection: collection.to_string(),
            timestamp: Instant::now(),
            session_id,
        };

        // If there is a transaction, add the cursor to its store
        if let Some((session_id, _)) = self.transaction.as_ref() {
            let transaction_store = self.service_context.transaction_store();
            if let Some((_, transaction)) =
                transaction_store.transactions.read().await.get(session_id)
            {
                transaction.cursors.add_cursor(key, value).await;
                return;
            }
        }

        // Otherwise add it to the service context
        self.service_context.add_cursor(key, value).await
    }

    pub async fn pull_connection_without_transaction(
        &self,
        in_transaction: bool,
    ) -> Result<Connection> {
        let user = self.auth_state.username()?;
        let pass = self
            .auth_state
            .password
            .as_ref()
            .ok_or(DocumentDBError::internal_error(
                "Password is missing on pg connection acquisition".to_string(),
            ))?;

        let mut conn = self.service_context.get_data_conn(user, pass).await?;
        conn.in_transaction = in_transaction;
        Ok(conn)
    }

    pub async fn new(
        sc: ServiceContext,
        telemetry_provider: Option<Box<dyn TelemetryProvider>>,
        ip: SocketAddr,
        ssl_protocol: String,
    ) -> Self {
        let connection_id = CONNECTION_ID.fetch_add(1, Ordering::Relaxed);
        ConnectionContext {
            start_time: Instant::now(),
            connection_id,
            service_context: Arc::new(sc),
            auth_state: AuthState::new(),
            requires_response: true,
            client_information: None,
            transaction: None,
            telemetry_provider,
            ip,
            cipher_type: 0,
            ssl_protocol,
        }
    }

    pub async fn allocate_data_pool(&self, user: &str, pass: &str) -> Result<()> {
        self.service_context.allocate_data_pool(user, pass).await
    }

    pub fn dynamic_configuration(&self) -> Arc<dyn DynamicConfiguration> {
        self.service_context.dynamic_configuration()
    }
}
