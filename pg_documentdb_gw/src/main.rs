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
    postgres::{create_query_catalog, DocumentDBDataClient},
    run_server,
    shutdown_controller::SHUTDOWN_CONTROLLER,
    startup::{
        create_postgres_object, get_service_context, get_system_connection_pool,
        populate_ssl_certificates, AUTHENTICATION_MAX_CONNECTIONS, SYSTEM_REQUESTS_MAX_CONNECTIONS,
    },
};

use tokio::signal;

#[ntex::main]
async fn main() {
    // Takes the configuration file as an argument
    let cfg_file = if let Some(arg1) = env::args().nth(1) {
        PathBuf::from(arg1)
    } else {
        // Defaults to the source directory for local runs
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("SetupConfiguration.json")
    };

    let shutdown_token = SHUTDOWN_CONTROLLER.token();

    tokio::spawn(async move {
        signal::ctrl_c().await.expect("Failed to listen for Ctrl+C");
        info!("Ctrl+C received. Shutting down Rust gateway.");
        SHUTDOWN_CONTROLLER.shutdown();
    });

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

    let system_requests_pool = Arc::new(
        get_system_connection_pool(
            &setup_configuration,
            &query_catalog,
            "SystemRequests",
            SYSTEM_REQUESTS_MAX_CONNECTIONS,
        )
        .await,
    );
    log::trace!("System requests pool initialized");

    let dynamic_configuration = create_postgres_object(
        || async {
            PgConfiguration::new(
                &query_catalog,
                &setup_configuration,
                &system_requests_pool,
                vec!["documentdb.".to_string()],
            )
            .await
        },
        &setup_configuration,
    )
    .await;

    let certificate_options = if let Some(co) = setup_configuration.certificate_options() {
        co
    } else {
        populate_ssl_certificates().await.unwrap()
    };

    let authentication_pool = get_system_connection_pool(
        &setup_configuration,
        &query_catalog,
        "PreAuthRequests",
        AUTHENTICATION_MAX_CONNECTIONS,
    )
    .await;
    log::trace!("Authentication pool initialized");

    let service_context = get_service_context(
        Box::new(setup_configuration),
        dynamic_configuration,
        query_catalog,
        system_requests_pool,
        authentication_pool,
    );

    run_server::<DocumentDBDataClient>(
        service_context,
        certificate_options,
        None,
        shutdown_token,
        None,
    )
    .await
    .unwrap();
}
