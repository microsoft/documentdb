/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/configuration/setup.rs
 *
 *-------------------------------------------------------------------------
 */

use std::path::Path;

use serde::Deserialize;
use tokio::fs::File;

use super::SetupConfiguration;
use crate::{
    configuration::certs::CertificateOptions,
    error::{DocumentDBError, Result},
};

// Configurations which are populated statically on process start
#[derive(Debug, Deserialize, Default, Clone)]
#[serde(rename_all = "PascalCase")]
pub struct DocumentDBSetupConfiguration {
    pub application_name: Option<String>,
    pub node_host_name: String,
    pub blocked_role_prefixes: Vec<String>,

    // Gateway listener configuration
    pub use_local_host: Option<bool>,
    pub gateway_listen_port: Option<u16>,

    // Postgres configuration
    pub postgres_system_user: Option<String>,
    pub postgres_host_name: Option<String>,
    pub postgres_port: Option<u16>,
    pub postgres_database: Option<String>,

    #[serde(default)]
    pub allow_transaction_snapshot: Option<bool>,
    pub transaction_timeout_secs: Option<u64>,
    pub cursor_timeout_secs: Option<u64>,
    pub enforce_ssl_tcp: Option<bool>,
    pub certificate_options: Option<CertificateOptions>,

    #[serde(default)]
    pub dynamic_configuration_file: String,
    pub dynamic_configuration_refresh_interval_secs: Option<u32>,
    pub postgres_command_timeout_secs: Option<u64>,
    pub postgres_startup_wait_time_seconds: Option<u64>,
}

impl DocumentDBSetupConfiguration {
    pub async fn new(config_path: &Path) -> Result<Self> {
        let config_file = File::open(config_path).await?;
        serde_json::from_reader(config_file.into_std().await).map_err(|e| {
            DocumentDBError::internal_error(format!("Failed to parse configuration file: {}", e))
        })
    }
}

impl SetupConfiguration for DocumentDBSetupConfiguration {
    // Needed to downcast to concrete type
    fn as_any(&self) -> &dyn std::any::Any {
        self
    }

    fn postgres_host_name(&self) -> &str {
        self.postgres_host_name.as_deref().unwrap_or("localhost")
    }

    fn postgres_port(&self) -> u16 {
        self.postgres_port.unwrap_or(9712)
    }

    fn postgres_database(&self) -> &str {
        self.postgres_database.as_deref().unwrap_or("postgres")
    }

    fn postgres_system_user(&self) -> String {
        self.postgres_system_user
            .clone()
            .unwrap_or(whoami::username())
    }

    fn dynamic_configuration_file(&self) -> String {
        self.dynamic_configuration_file.clone()
    }

    fn dynamic_configuration_refresh_interval_secs(&self) -> u32 {
        self.dynamic_configuration_refresh_interval_secs
            .unwrap_or(60 * 5)
    }

    fn cursor_timeout_secs(&self) -> u64 {
        self.cursor_timeout_secs.unwrap_or(600)
    }

    fn transaction_timeout_secs(&self) -> u64 {
        self.transaction_timeout_secs.unwrap_or(30)
    }

    fn use_local_host(&self) -> bool {
        self.use_local_host.unwrap_or(false)
    }

    fn gateway_listen_port(&self) -> u16 {
        self.gateway_listen_port.unwrap_or(10260)
    }

    fn enforce_ssl_tcp(&self) -> bool {
        self.enforce_ssl_tcp.unwrap_or(true)
    }

    fn blocked_role_prefixes(&self) -> &[String] {
        &self.blocked_role_prefixes
    }

    fn postgres_command_timeout_secs(&self) -> u64 {
        self.postgres_command_timeout_secs.unwrap_or(120)
    }

    fn certificate_options(&self) -> Option<CertificateOptions> {
        self.certificate_options.clone()
    }

    fn node_host_name(&self) -> &str {
        &self.node_host_name
    }

    fn application_name(&self) -> &str {
        self.application_name
            .as_deref()
            .unwrap_or("DocumentDBGateway")
    }

    fn postgres_startup_wait_time_seconds(&self) -> u64 {
        self.postgres_startup_wait_time_seconds.unwrap_or(60)
    }
}
