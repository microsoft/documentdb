/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/monitoring/mod.rs
 *
 *-------------------------------------------------------------------------
 */

use std::sync::Arc;
use std::time::Duration;
use tokio::time::interval;
use log::{info, warn, error};

use crate::postgres::Pool;
use crate::telemetry::TelemetryProvider;

/// ClusterMonitor periodically checks the availability of primary and secondary clusters
/// and emits metrics via the TelemetryProvider.
pub struct ClusterMonitor {
    primary_pool: Arc<Pool>,
    secondary_pool: Option<Arc<Pool>>,
    telemetry: Arc<dyn TelemetryProvider>,
    check_interval: Duration,
}

impl ClusterMonitor {
    pub fn new(
        primary_pool: Arc<Pool>,
        secondary_pool: Option<Arc<Pool>>,
        telemetry: Arc<dyn TelemetryProvider>,
        check_interval_secs: u64,
    ) -> Self {
        ClusterMonitor {
            primary_pool,
            secondary_pool,
            telemetry,
            check_interval: Duration::from_secs(check_interval_secs),
        }
    }

    /// Start the cluster monitoring background task
    pub async fn start(self) -> tokio::task::JoinHandle<()> {
        tokio::spawn(async move {
            let mut interval = interval(self.check_interval);
            
            loop {
                interval.tick().await;
                self.check_cluster_availability().await;
            }
        })
    }

    /// Check the availability of all clusters and emit metrics
    async fn check_cluster_availability(&self) {
        // Check primary cluster availability
        let primary_available = self.check_pool_availability(&self.primary_pool, "primary").await;
        self.telemetry.update_cluster_availability("primary", primary_available);

        // Check secondary cluster availability if configured
        if let Some(secondary_pool) = &self.secondary_pool {
            let secondary_available = self.check_pool_availability(secondary_pool, "secondary").await;
            self.telemetry.update_cluster_availability("secondary", secondary_available);
        }
    }

    /// Check if a specific connection pool is available by attempting to get a connection
    async fn check_pool_availability(&self, pool: &Pool, cluster_name: &str) -> bool {
        match pool.get().await {
            Ok(mut client) => {
                // Try to execute a simple query to confirm the connection works
                match client.query("SELECT 1", &[]).await {
                    Ok(_) => {
                        info!("{} cluster is available", cluster_name);
                        true
                    }
                    Err(e) => {
                        warn!("{} cluster query failed: {}", cluster_name, e);
                        false
                    }
                }
            }
            Err(e) => {
                error!("Failed to connect to {} cluster: {}", cluster_name, e);
                false
            }
        }
    }
}
