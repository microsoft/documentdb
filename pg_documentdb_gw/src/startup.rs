/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/startup.rs
 *
 *-------------------------------------------------------------------------
 */

use std::{sync::Arc, time::Duration};

use log::warn;

use crate::configuration::CertificateOptions;
use crate::{
    configuration::{DynamicConfiguration, SetupConfiguration},
    context::ServiceContext,
    error::Result,
    postgres::ConnectionPool,
    QueryCatalog,
};

pub const SYSTEM_REQUESTS_MAX_CONNECTIONS: usize = 2;
pub const AUTHENTICATION_MAX_CONNECTIONS: usize = 5;

pub fn get_service_context(
    setup_configuration: Box<dyn SetupConfiguration>,
    dynamic: Arc<dyn DynamicConfiguration>,
    query_catalog: QueryCatalog,
    system_requests_pool: Arc<ConnectionPool>,
    authentication_pool: ConnectionPool,
) -> ServiceContext {
    let service_context = ServiceContext::new(
        setup_configuration.clone(),
        Arc::clone(&dynamic),
        query_catalog.clone(),
        Arc::clone(&system_requests_pool),
        authentication_pool,
    );

    let service_context_clone = service_context.clone();

    tokio::spawn(async move {
        let mut cleanup_interval = tokio::time::interval(Duration::from_secs(300));
        let max_age = Duration::from_secs(7200);
        loop {
            cleanup_interval.tick().await;
            service_context_clone.clean_unused_pools(max_age).await;
        }
    });

    service_context
}

pub async fn get_system_connection_pool(
    setup_configuration: &dyn SetupConfiguration,
    query_catalog: &QueryCatalog,
    pool_name: &str,
    max_size: usize,
) -> ConnectionPool {
    // Capture necessary values to avoid lifetime issues
    let postgres_system_user = setup_configuration.postgres_system_user();
    let full_pool_name = format!("{}-{}", setup_configuration.application_name(), pool_name);

    create_postgres_object(
        || async {
            ConnectionPool::new_with_user(
                setup_configuration,
                query_catalog,
                &postgres_system_user,
                None,
                full_pool_name.clone(),
                max_size,
            )
        },
        setup_configuration,
    )
    .await
}

pub async fn create_postgres_object<T, F, Fut>(
    create_func: F,
    setup_configuration: &dyn SetupConfiguration,
) -> T
where
    F: Fn() -> Fut,
    Fut: std::future::Future<Output = Result<T>>,
{
    let max_time = Duration::from_secs(setup_configuration.postgres_startup_wait_time_seconds());
    let wait_time = Duration::from_secs(10);
    let start = tokio::time::Instant::now();

    loop {
        match create_func().await {
            Ok(result) => {
                return result;
            }
            Err(e) => {
                if start.elapsed() < max_time {
                    warn!("Exception when creating postgres object {:?}", e);
                    tokio::time::sleep(wait_time).await;
                    continue;
                } else {
                    panic!(
                        "Failed to create postgres object after {:?}: {}",
                        max_time, e
                    );
                }
            }
        }
    }
}

#[cfg(debug_assertions)]
pub async fn populate_ssl_certificates() -> Result<CertificateOptions> {
    use rcgen::{generate_simple_self_signed, CertifiedKey};

    let subject_alt_names = vec!["localhost".to_string()];
    let CertifiedKey { cert, key_pair } = generate_simple_self_signed(subject_alt_names).unwrap();
    tokio::fs::write("./cert.pem", cert.pem()).await?;
    tokio::fs::write("./key.pem", key_pair.serialize_pem()).await?;
    Ok(CertificateOptions {
        cert_type: "pem_file".to_string(),
        file_path: "./cert.pem".to_string(),
        key_file_path: "./key.pem".to_string(),
        ca_path: None,
    })
}

#[cfg(not(debug_assertions))]
pub async fn populate_ssl_certificates() -> Result<CertificateOptions> {
    panic!("Release builds must provide SSL certificate options.")
}
