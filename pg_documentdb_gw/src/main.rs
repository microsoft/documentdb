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
    postgres::{create_query_catalog, Pool},
    run_server,
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
    let system_pool = Arc::new(
        Pool::new_with_user(
            &setup_configuration,
            &query_catalog,
            &postgres_system_user,
            None,
            format!("{}-SystemRequests", setup_configuration.application_name()),
            5,
        )
        .expect("Failed to create system pool"),
    );
    log::trace!("Pool initialized");

    let dynamic_configuration = PgConfiguration::new(
        &query_catalog,
        &setup_configuration,
        &system_pool,
        "documentdb.",
    )
    .await
    .unwrap();

    let certificate_options = if let Some(co) = setup_configuration.certificate_options() {
        co
    } else {
        populate_ssl_certificates().await.unwrap()
    };

    let service_context = get_service_context(
        Box::new(setup_configuration),
        dynamic_configuration,
        query_catalog,
        system_pool.clone(),
    )
    .await
    .unwrap();

    run_server(service_context, certificate_options, None, token.clone())
        .await
        .unwrap();
}
