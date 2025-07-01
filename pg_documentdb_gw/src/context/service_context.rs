/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/context/service_context.rs
 *
 *-------------------------------------------------------------------------
 */

use std::{borrow::Cow, collections::HashMap, sync::Arc, time::Duration};

use tokio::sync::RwLock;

use crate::{
    configuration::{DynamicConfiguration, SetupConfiguration},
    error::{DocumentDBError, Result},
    postgres::{Connection, ConnectionPool},
    QueryCatalog,
};

use super::{CursorStore, CursorStoreEntry, TransactionStore};

type ClientKey = (Cow<'static, str>, Cow<'static, str>, usize);

pub struct ServiceContextInner {
    pub setup_configuration: Box<dyn SetupConfiguration>,
    pub dynamic_configuration: Arc<dyn DynamicConfiguration>,

    // Connection pool for system requests that is shared between ServiceContext and DynamicConfiguration
    pub system_requests_pool: Arc<ConnectionPool>,
    pub system_auth_pool: ConnectionPool,

    // Maps user credentials to their respective connection pools
    // We need Arc on the ConnectionPool to allow sharing across threads from different connections
    // TODO: need to add excessive testing when the user is changing password or pool size changed
    pub user_data_pools: RwLock<HashMap<ClientKey, Arc<ConnectionPool>>>,
    pub system_shared_pools: RwLock<HashMap<usize, Arc<ConnectionPool>>>,
    pub cursor_store: CursorStore,
    pub transaction_store: TransactionStore,
    pub query_catalog: QueryCatalog,
}

#[derive(Clone)]
pub struct ServiceContext(Arc<ServiceContextInner>);

impl ServiceContext {
    pub fn new(
        setup_configuration: Box<dyn SetupConfiguration>,
        dynamic_configuration: Arc<dyn DynamicConfiguration>,
        query_catalog: QueryCatalog,
        system_requests_pool: Arc<ConnectionPool>,
        system_auth_pool: ConnectionPool,
    ) -> Self {
        log::trace!("Initial dynamic configuration: {:?}", dynamic_configuration);

        let timeout_secs = setup_configuration.transaction_timeout_secs();
        let inner = ServiceContextInner {
            setup_configuration: setup_configuration.clone(),
            dynamic_configuration,
            system_requests_pool,
            system_auth_pool,
            user_data_pools: RwLock::new(HashMap::new()),
            system_shared_pools: RwLock::new(HashMap::new()),
            cursor_store: CursorStore::new(setup_configuration.as_ref(), true),
            transaction_store: TransactionStore::new(Duration::from_secs(timeout_secs)),
            query_catalog,
        };
        ServiceContext(Arc::new(inner))
    }

    pub async fn get_data_pool(&self, user: &str, pass: &str) -> Result<Arc<ConnectionPool>> {
        let max_connections = self.dynamic_configuration().max_connections().await;
        let read_lock = self.0.user_data_pools.read().await;

        match read_lock.get(&(Cow::Borrowed(user), Cow::Borrowed(pass), max_connections)) {
            None => Err(DocumentDBError::internal_error(
                "Connection pool missing for user.".to_string(),
            )),
            Some(pool) => Ok(Arc::clone(pool)),
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

    pub async fn system_requests_connection(&self) -> Result<Connection> {
        Ok(Connection::new(
            self.0.system_requests_pool.get_inner_connection().await?,
            false,
        ))
    }

    pub async fn authentication_connection(&self) -> Result<Connection> {
        Ok(Connection::new(
            self.0.system_auth_pool.get_inner_connection().await?,
            false,
        ))
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

    pub async fn allocate_data_pool(&self, user: &str, pass: &str) -> Result<()> {
        let max_connections = self.dynamic_configuration().max_connections().await;

        if self.0.user_data_pools.read().await.contains_key(&(
            Cow::Borrowed(user),
            Cow::Borrowed(pass),
            max_connections,
        )) {
            return Ok(());
        }

        let mut write_lock = self.0.user_data_pools.write().await;
        let _ = write_lock.insert(
            (
                Cow::Owned(user.to_owned()),
                Cow::Owned(pass.to_owned()),
                max_connections,
            ),
            Arc::new(ConnectionPool::new_with_user(
                self.setup_configuration(),
                self.query_catalog(),
                user,
                Some(pass),
                format!("{}-Data", self.setup_configuration().application_name()),
                self.get_real_max_connections(max_connections).await,
            )?),
        );
        Ok(())
    }

    pub async fn get_system_shared_pool(&self) -> Result<Arc<ConnectionPool>> {
        let max_connections = self.dynamic_configuration().max_connections().await;

        if let Some(conn_pool) = self
            .0
            .system_shared_pools
            .read()
            .await
            .get(&max_connections)
        {
            return Ok(Arc::clone(conn_pool));
        }

        let mut write_lock = self.0.system_shared_pools.write().await;

        let system_shared_pool = Arc::new(ConnectionPool::new_with_user(
            self.setup_configuration(),
            self.query_catalog(),
            &self.setup_configuration().postgres_system_user(),
            None,
            format!("{}-Data", self.setup_configuration().application_name()),
            self.get_real_max_connections(max_connections).await,
        )?);

        write_lock.insert(max_connections, Arc::clone(&system_shared_pool));

        Ok(Arc::clone(&system_shared_pool))
    }

    async fn get_real_max_connections(&self, max_connections: usize) -> usize {
        let system_connection_budget = self
            .dynamic_configuration()
            .system_connection_budget()
            .await;
        let mut real_max_connections = max_connections - system_connection_budget;
        if real_max_connections < system_connection_budget {
            real_max_connections = system_connection_budget;
        }
        real_max_connections
    }

    pub async fn clean_unused_pools(&self, max_age: Duration) {
        {
            let mut user_pools_write_lock = self.0.user_data_pools.write().await;
            let mut keys_to_remove = Vec::new();
            for (key, pool) in user_pools_write_lock.iter() {
                if pool.last_used().await.elapsed() > max_age {
                    keys_to_remove.push(key.clone());
                }
            }
            for key in keys_to_remove {
                user_pools_write_lock.remove(&key);
            }
        }

        {
            let mut system_shared_write_lock = self.0.system_shared_pools.write().await;
            let mut keys_to_remove = Vec::new();
            for (key, pool) in system_shared_write_lock.iter() {
                if pool.last_used().await.elapsed() > max_age {
                    keys_to_remove.push(*key);
                }
            }
            for key in keys_to_remove {
                system_shared_write_lock.remove(&key);
            }
        }
    }
}
