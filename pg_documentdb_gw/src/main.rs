/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/main.rs
 *
 *-------------------------------------------------------------------------
 */

use log::info;
use simple_logger::SimpleLogger;
use std::{env, path::PathBuf, sync::Arc};

use documentdb_gateway::{
    configuration::{DocumentDBSetupConfiguration, PgConfiguration, SetupConfiguration},
    get_service_context, populate_ssl_certificates,
    postgres::{create_query_catalog, ConnectionPool},
    run_server, AUTHENTICATION_MAX_CONNECTIONS, SYSTEM_REQUESTS_MAX_CONNECTIONS,
};
use tokio_util::sync::CancellationToken;

#[ntex::main]
async fn main() {
    // Takes the configuration file as an argument
    let cfg_file = if let Some(arg1) = env::args().nth(1) {
        PathBuf::from(arg1)
    } else {
        // Defaults to the source directory for local runs
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("SetupConfiguration.json")
    };

    let token = CancellationToken::new();

    let setup_configuration = DocumentDBSetupConfiguration::new(&cfg_file)
        .await
        .expect("Failed to load configuration.");

    info!(
        "Starting server with configuration: {:?}",
        setup_configuration
    );

    SimpleLogger::new()
        .with_level(log::LevelFilter::Info)
        .with_module_level("tokio_postgres", log::LevelFilter::Info)
        .init()
        .expect("Failed to start logger");

    let query_catalog = create_query_catalog();

    let postgres_system_user = setup_configuration.postgres_system_user();
    let system_requests_pool = Arc::new(
        ConnectionPool::new_with_user(
            &setup_configuration,
            &query_catalog,
            &postgres_system_user,
            None,
            format!("{}-SystemRequests", setup_configuration.application_name()),
            SYSTEM_REQUESTS_MAX_CONNECTIONS,
        )
        .expect("Failed to create system requests pool"),
    );
    log::trace!("System requests pool initialized");

    let dynamic_configuration = PgConfiguration::new(
        &query_catalog,
        &setup_configuration,
        &system_requests_pool,
        "documentdb.",
    )
    .await
    .unwrap();

    let certificate_options = if let Some(co) = setup_configuration.certificate_options() {
        co
    } else {
        populate_ssl_certificates().await.unwrap()
    };

    let authentication_pool = Arc::new(
        ConnectionPool::new_with_user(
            &setup_configuration,
            &query_catalog,
            &postgres_system_user,
            None,
            format!("{}-PreAuthRequests", setup_configuration.application_name()),
            AUTHENTICATION_MAX_CONNECTIONS,
        )
        .expect("Failed to create authentication pool"),
    );
    log::trace!("Authentication pool initialized");

    let service_context = get_service_context(
        Box::new(setup_configuration),
        dynamic_configuration,
        query_catalog,
        system_requests_pool.clone(),
        authentication_pool.clone(),
    )
    .await
    .unwrap();

    run_server(
        service_context,
        certificate_options,
        None,
        token.clone(),
        None,
    )
    .await
    .unwrap();
}
