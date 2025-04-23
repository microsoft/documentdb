/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/context/service.rs
 *
 *-------------------------------------------------------------------------
 */

use std::{borrow::Cow, collections::HashMap, sync::Arc, time::Duration};

use tokio::sync::RwLock;

use crate::{
    configuration::{DynamicConfiguration, SetupConfiguration},
    error::{DocumentDBError, Result},
    postgres::{Client, Pool},
    QueryCatalog,
};

use super::{CursorStore, CursorStoreEntry, TransactionStore};

type ClientKey = (Cow<'static, str>, Cow<'static, str>);

pub struct ServiceContextInner {
    pub setup_configuration: Box<dyn SetupConfiguration>,
    pub dynamic_configuration: Arc<dyn DynamicConfiguration>,
    pub system_pool: Arc<Pool>,
    pub pg_clients: RwLock<HashMap<ClientKey, Pool>>,
    pub cursor_store: CursorStore,
    pub transaction_store: TransactionStore,
    pub query_catalog: QueryCatalog,
}

#[derive(Clone)]
pub struct ServiceContext(Arc<ServiceContextInner>);

impl ServiceContext {
    pub async fn new(
        setup_configuration: Box<dyn SetupConfiguration>,
        dynamic_configuration: Arc<dyn DynamicConfiguration>,
        query_catalog: QueryCatalog,
        system_pool: Arc<Pool>,
    ) -> Result<Self> {
        log::trace!("Initial dynamic configuration: {:?}", dynamic_configuration);

        let timeout_secs = setup_configuration.transaction_timeout_secs();
        let inner = ServiceContextInner {
            setup_configuration: setup_configuration.clone(),
            dynamic_configuration,
            system_pool,
            pg_clients: RwLock::new(HashMap::new()),
            cursor_store: CursorStore::new(setup_configuration.as_ref(), true),
            transaction_store: TransactionStore::new(Duration::from_secs(timeout_secs)),
            query_catalog,
        };
        Ok(ServiceContext(Arc::new(inner)))
    }

    pub async fn pg(&'_ self, user: &str, pass: &str) -> Result<Client> {
        let map = self.0.pg_clients.read().await;

        match map.get(&(Cow::Borrowed(user), Cow::Borrowed(pass))) {
            None => Err(DocumentDBError::internal_error(
                "Connection pool missing for user.".to_string(),
            )),
            Some(pool) => {
                let client = pool.get().await?;
                Ok(Client::new(client, false))
            }
        }
    }

    pub async fn add_cursor(&self, key: (i64, String), entry: CursorStoreEntry) {
        self.0.cursor_store.add_cursor(key, entry).await
    }

    pub async fn get_cursor(&self, id: i64, user: &str) -> Option<CursorStoreEntry> {
        self.0.cursor_store.get_cursor((id, user.to_string())).await
    }

    pub async fn invalidate_cursors_by_collection(&self, db: &str, collection: &str) {
        self.0
            .cursor_store
            .invalidate_cursors_by_collection(db, collection)
            .await
    }

    pub async fn invalidate_cursors_by_database(&self, db: &str) {
        self.0.cursor_store.invalidate_cursors_by_database(db).await
    }

    pub async fn invalidate_cursors_by_session(&self, session: &[u8]) {
        self.0
            .cursor_store
            .invalidate_cursors_by_session(session)
            .await
    }

    pub async fn kill_cursors(&self, user: &str, cursors: &[i64]) -> (Vec<i64>, Vec<i64>) {
        self.0
            .cursor_store
            .kill_cursors(user.to_string(), cursors)
            .await
    }

    pub async fn system_client(&self) -> Result<Client> {
        Ok(Client::new(self.0.system_pool.get().await?, false))
    }

    pub fn setup_configuration(&self) -> &dyn SetupConfiguration {
        self.0.setup_configuration.as_ref()
    }

    pub fn dynamic_configuration(&self) -> Arc<dyn DynamicConfiguration> {
        self.0.dynamic_configuration.clone()
    }

    pub fn transaction_store(&self) -> &TransactionStore {
        &self.0.transaction_store
    }

    pub fn query_catalog(&self) -> &QueryCatalog {
        &self.0.query_catalog
    }

    pub async fn ensure_client_pool(&self, user: &str, pass: &str) -> Result<()> {
        if self
            .0
            .pg_clients
            .read()
            .await
            .contains_key(&(Cow::Borrowed(user), Cow::Borrowed(pass)))
        {
            return Ok(());
        }

        let mut map = self.0.pg_clients.write().await;
        let _ = map.insert(
            (Cow::Owned(user.to_owned()), Cow::Owned(pass.to_owned())),
            Pool::new_with_user(
                self.setup_configuration(),
                self.query_catalog(),
                user,
                Some(pass),
                format!("{}-Data", self.setup_configuration().application_name()),
                self.dynamic_configuration().max_connections().await,
            )?,
        );
        Ok(())
    }
}
