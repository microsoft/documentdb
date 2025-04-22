/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/postgres/transaction.rs
 *
 *-------------------------------------------------------------------------
 */

use std::sync::Arc;

use tokio_postgres::IsolationLevel;

use super::{Client, QueryCatalog};
use crate::error::{DocumentDBError, Result};

pub struct Transaction {
    client: Arc<Client>,
    pub committed: bool,
}

impl Transaction {
    pub async fn start(client: Arc<Client>, isolation_level: IsolationLevel) -> Result<Self> {
        let isolation = match isolation_level {
            IsolationLevel::RepeatableRead => "REPEATABLE READ",
            IsolationLevel::ReadCommitted => "READ COMMITTED",
            other => {
                return Err(DocumentDBError::bad_value(format!(
                    "Isolation level not supported: {:?}",
                    other
                )))
            }
        };

        client
            .batch_execute(&format!(
                "START TRANSACTION ISOLATION LEVEL {}; SET LOCAL lock_timeout='20ms'; SET LOCAL citus.max_adaptive_executor_pool_size=1;",
                isolation
            ))
            .await?;

        Ok(Transaction {
            client,
            committed: false,
        })
    }

    pub fn get_client(&self) -> Arc<Client> {
        self.client.clone()
    }

    pub async fn commit(&mut self) -> Result<()> {
        self.client.batch_execute("COMMIT").await?;
        self.committed = true;
        Ok(())
    }

    pub async fn abort(&mut self) -> Result<()> {
        self.client.batch_execute("ROLLBACK").await?;
        self.committed = true;
        Ok(())
    }

    pub async fn allow_writes_in_readonly(&self, query_catalog: &QueryCatalog) -> Result<()> {
        self.client
            .batch_execute(query_catalog.set_allow_write())
            .await
    }
}
