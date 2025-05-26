/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/configuration/pg_configuration.rs
 *
 *-------------------------------------------------------------------------
 */

use std::collections::HashMap;
use std::sync::Arc;
use std::time::Duration;

use async_trait::async_trait;
use bson::{rawbson, RawBson};
use serde::Deserialize;
use tokio::sync::RwLock;

use super::dynamic::{DynamicConfiguration, POSTGRES_RECOVERY_KEY};
use super::SetupConfiguration;
use crate::error::{DocumentDBError, Result};
use crate::postgres::{Connection, ConnectionPool};
use crate::requests::RequestInfo;
use crate::QueryCatalog;

#[derive(Debug, Deserialize, Default, Clone)]
#[serde(rename_all = "PascalCase")]
pub struct HostConfig {
    #[serde(default)]
    is_primary: String,
    #[serde(default)]
    send_shutdown_responses: String,
}

#[derive(Debug)]
pub struct PgConfiguration {
    values: RwLock<HashMap<String, String>>,
}

impl PgConfiguration {
    pub fn start_dynamic_configuration_refresh_thread(
        configuration: Arc<PgConfiguration>,
        system_requests_pool: Arc<ConnectionPool>,
        query_catalog: &QueryCatalog,
        dynamic_config_file: String,
        refresh_interval: u32,
        settings_prefix: &str,
    ) {
        let query_catalog_clone = query_catalog.clone();
        let dynamic_config_file_clone = dynamic_config_file.clone();
        let settings_prefix = settings_prefix.to_string();
        tokio::spawn(async move {
            let mut interval = tokio::time::interval(Duration::from_secs(refresh_interval as u64));
            interval.tick().await;

            loop {
                interval.tick().await;

                match system_requests_pool.get_inner_connection().await {
                    Ok(inner_conn) => {
                        match PgConfiguration::load_configurations(
                            &query_catalog_clone,
                            &dynamic_config_file_clone,
                            &Connection::new(inner_conn, false),
                            &settings_prefix,
                        )
                        .await
                        {
                            Ok(new_config) => {
                                let mut config_self_writable = configuration.values.write().await;
                                *config_self_writable = new_config;
                            }
                            Err(e) => log::error!("Failed to refresh configuration: {}", e),
                        }
                    }
                    Err(e) => {
                        log::error!(
                            "Failed to acquire postgres connection to refresh configuration: {}",
                            e
                        )
                    }
                }
            }
        });
    }

    pub async fn new(
        query_catalog: &QueryCatalog,
        setup_configuration: &dyn SetupConfiguration,
        system_requests_pool: &Arc<ConnectionPool>,
        settings_prefix: &str,
    ) -> Result<Arc<Self>> {
        let conn = Connection::new(system_requests_pool.get_inner_connection().await?, false);
        let dynamic_config_file = setup_configuration.dynamic_configuration_file();
        let values = RwLock::new(
            PgConfiguration::load_configurations(
                query_catalog,
                &dynamic_config_file,
                &conn,
                settings_prefix,
            )
            .await?,
        );

        let configuration = Arc::new(PgConfiguration { values });

        let refresh_interval = setup_configuration.dynamic_configuration_refresh_interval_secs();
        Self::start_dynamic_configuration_refresh_thread(
            configuration.clone(),
            system_requests_pool.clone(),
            query_catalog,
            dynamic_config_file,
            refresh_interval,
            settings_prefix,
        );

        Ok(configuration)
    }

    async fn load_host_config(dynamic_config_file: &str) -> Result<HostConfig> {
        let config: HostConfig =
            serde_json::from_str(&tokio::fs::read_to_string(dynamic_config_file).await?).map_err(
                |e| DocumentDBError::internal_error(format!("Failed to read config file: {}", e)),
            )?;
        Ok(config)
    }

    async fn load_configurations(
        query_catalog: &QueryCatalog,
        dynamic_config_file: &str,
        conn: &Connection,
        settings_prefix: &str,
    ) -> Result<HashMap<String, String>> {
        let mut configs = HashMap::new();

        match Self::load_host_config(dynamic_config_file).await {
            Ok(host_config) => {
                configs.insert(
                    "IsPrimary".to_owned(),
                    host_config.is_primary.to_lowercase(),
                );
                configs.insert(
                    "SendShutdownResponses".to_owned(),
                    host_config.send_shutdown_responses.to_lowercase(),
                );
            }
            Err(e) => log::warn!("Host Config file not able to be loaded: {}", e),
        }

        let mut request_info = RequestInfo::new();
        let pg_config_rows = conn
            .query(
                query_catalog.pg_settings(),
                &[],
                &[],
                None,
                &mut request_info,
            )
            .await?;
        for pg_config in pg_config_rows {
            let key: &str = pg_config.get(0);
            let key = key.strip_prefix(settings_prefix).unwrap_or(key);

            let mut value: String = pg_config.get(1);
            if value == "on" {
                value = "true".to_string();
            } else if value == "off" {
                value = "false".to_string();
            }
            configs.insert(key.to_owned(), value);
        }

        let pg_is_in_recovery_row = conn
            .query(
                query_catalog.pg_is_in_recovery(),
                &[],
                &[],
                None,
                &mut request_info,
            )
            .await?;
        let in_recovery: bool = pg_is_in_recovery_row.first().is_some_and(|row| row.get(0));
        configs.insert(POSTGRES_RECOVERY_KEY.to_string(), in_recovery.to_string());

        log::info!("Dynamic configurations loaded: {:?}", configs);
        Ok(configs)
    }
}

#[async_trait]
impl DynamicConfiguration for PgConfiguration {
    async fn get_str(&self, key: &str) -> Option<String> {
        self.values.read().await.get(key).cloned()
    }

    async fn get_bool(&self, key: &str, default: bool) -> bool {
        let ret = self
            .values
            .read()
            .await
            .get(key)
            .map(|v| v.parse::<bool>().unwrap_or(default))
            .unwrap_or(default);
        ret
    }

    async fn get_i32(&self, key: &str, default: i32) -> i32 {
        let ret = self
            .values
            .read()
            .await
            .get(key)
            .map(|v| v.parse::<i32>().unwrap_or(default))
            .unwrap_or(default);
        ret
    }

    async fn equals_value(&self, key: &str, value: &str) -> bool {
        let ret = self
            .values
            .read()
            .await
            .get(key)
            .map(|v| v == value)
            .unwrap_or(false);
        ret
    }

    fn topology(&self) -> RawBson {
        let empty_doc: RawBson = rawbson!({});
        empty_doc
    }

    async fn enable_developer_explain(&self) -> bool {
        self.get_bool("enableDeveloperExplain", false).await
    }

    async fn max_connections(&self) -> usize {
        let ret = self.get_i32("max_connections", 300).await;
        ret as usize
    }

    fn as_any(&self) -> &dyn std::any::Any {
        self
    }
}
