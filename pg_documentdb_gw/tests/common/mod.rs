use std::sync::Arc;
use std::{backtrace::Backtrace, env, sync::Once, thread, time::Duration};

use documentdb_gateway::configuration::{
    DocumentDBSetupConfiguration, PgConfiguration, SetupConfiguration,
};
use documentdb_gateway::error::Result;
use documentdb_gateway::postgres::{create_query_catalog, ConnectionPool, DocumentDBDataClient};
use documentdb_gateway::{
    run_server,
    startup::{get_service_context, populate_ssl_certificates, AUTHENTICATION_MAX_CONNECTIONS},
    QueryCatalog,
};

use mongodb::options::{Tls, TlsOptions};
use mongodb::{
    options::{AuthMechanism, ClientOptions, Credential, ServerAddress},
    Client, Database,
};
use simple_logger::SimpleLogger;
use tokio_postgres::{error::SqlState, NoTls};
use tokio_util::sync::CancellationToken;

static INIT: Once = Once::new();

// Starts the server and returns an authenticated client
async fn initialize_full(config: DocumentDBSetupConfiguration) {
    env::set_var("RUST_LIB_BACKTRACE", "1");

    INIT.call_once(|| {
        SimpleLogger::new()
            .with_level(log::LevelFilter::Trace)
            .with_module_level("rustls", log::LevelFilter::Info)
            .with_module_level("ntex_rt", log::LevelFilter::Info)
            .with_module_level("ntex_server", log::LevelFilter::Info)
            .with_module_level("tokio_postgres", log::LevelFilter::Info)
            .with_module_level("hyper", log::LevelFilter::Info)
            .init()
            .unwrap();
        thread::spawn(move || run(config));
        thread::sleep(Duration::from_millis(100));
    });

    create_user("test", "test", &create_query_catalog())
        .await
        .unwrap();
}

#[ntex::main]
async fn run(config: DocumentDBSetupConfiguration) {
    let query_catalog = create_query_catalog();
    let postgres_system_user = config.postgres_system_user();
    let system_pool = Arc::new(
        ConnectionPool::new_with_user(
            &config,
            &query_catalog,
            &postgres_system_user,
            None,
            format!("{}-SystemRequests", config.application_name()),
            5,
        )
        .expect("Failed to create system pool"),
    );

    let certificate_options = if let Some(co) = config.certificate_options.clone() {
        co
    } else {
        populate_ssl_certificates().await.unwrap()
    };

    let dynamic_configuration =
        PgConfiguration::new(&query_catalog, &config, &system_pool, "documentdb.")
            .await
            .unwrap();

    let authentication_pool = ConnectionPool::new_with_user(
        &config,
        &query_catalog,
        &postgres_system_user,
        None,
        format!("{}-PreAuthRequests", config.application_name()),
        AUTHENTICATION_MAX_CONNECTIONS,
    )
    .expect("Failed to create authentication pool");

    let service_context = get_service_context(
        Box::new(config),
        dynamic_configuration,
        query_catalog,
        system_pool,
        authentication_pool,
    );

    run_server::<DocumentDBDataClient>(
        service_context,
        certificate_options,
        None,
        CancellationToken::new(),
        None,
    )
    .await
    .unwrap()
}

pub fn configuration() -> DocumentDBSetupConfiguration {
    DocumentDBSetupConfiguration {
        node_host_name: "localhost".to_string(),
        blocked_role_prefixes: Vec::new(),
        gateway_listen_port: Some(10260),
        allow_transaction_snapshot: Some(false),
        enforce_ssl_tcp: Some(true),
        postgres_system_user: Some(
            std::env::var("PostgresSystemUser").unwrap_or("cosmosdev".to_string()),
        ),
        ..Default::default()
    }
}

pub fn get_client() -> Client {
    let credential = Credential::builder()
        .username("test".to_string())
        .password("test".to_string())
        .mechanism(AuthMechanism::ScramSha256)
        .build();

    let client_options = ClientOptions::builder()
        .credential(credential)
        .tls(Tls::Enabled(
            TlsOptions::builder()
                .allow_invalid_certificates(true)
                .build(),
        ))
        .hosts(vec![ServerAddress::parse("127.0.0.1:10260").unwrap()])
        .build();
    Client::with_options(client_options).unwrap()
}

#[allow(dead_code)]
pub fn get_client_insecure() -> Client {
    let credential = Credential::builder()
        .username("test".to_string())
        .password("test".to_string())
        .mechanism(AuthMechanism::ScramSha256)
        .build();

    let client_options = ClientOptions::builder()
        .credential(credential)
        .hosts(vec![ServerAddress::parse("127.0.0.1:10260").unwrap()])
        .build();
    Client::with_options(client_options).unwrap()
}

#[allow(dead_code)]
pub async fn initialize_with_logger() -> Client {
    initialize_full(configuration()).await;
    get_client()
}

#[allow(dead_code)]
pub async fn initialize_with_config(config: DocumentDBSetupConfiguration) -> Client {
    initialize_full(config).await;
    get_client()
}

#[allow(dead_code)]
pub async fn initialize() -> Client {
    initialize_full(configuration()).await;
    get_client()
}

pub async fn create_user(user: &str, pass: &str, query_catalog: &QueryCatalog) -> Result<()> {
    let (client, connection) = tokio_postgres::Config::new()
        .host("localhost")
        .port(9712)
        .dbname("postgres")
        .connect(NoTls)
        .await
        .unwrap();
    tokio::spawn(connection);

    let statement = query_catalog.create_db_user(user, pass);
    match client.batch_execute(&statement).await {
        Err(e) => {
            if e.code()
                .is_some_and(|code| code == &SqlState::DUPLICATE_OBJECT)
            {
                Ok(())
            } else {
                Err(documentdb_gateway::error::DocumentDBError::PostgresError(
                    e,
                    Backtrace::capture(),
                ))
            }
        }
        Ok(_) => Ok(()),
    }?;

    client
        .batch_execute(&format!("ALTER ROLE {} SUPERUSER", user))
        .await
        .unwrap();

    if let tokio_postgres::SimpleQueryMessage::Row(result) = client
        .simple_query(&format!(
            "SELECT * FROM pg_roles WHERE rolname = '{}'",
            user
        ))
        .await
        .unwrap()
        .get(0)
        .unwrap()
    {
        log::info!("Test can create: {:?}", result.get("rolcreaterole"));
    }
    Ok(())
}

// Initialize the server and also clear a database for use
#[allow(dead_code)]
pub async fn initialize_with_db(db: &str) -> Database {
    let client = initialize().await;
    setup_db(&client, db).await
}

#[allow(dead_code)]
pub async fn setup_db(client: &Client, db: &str) -> Database {
    let db = client.database(db);

    // Make sure the DB is clean
    db.drop().await.unwrap();
    db
}
