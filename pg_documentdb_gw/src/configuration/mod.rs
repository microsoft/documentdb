mod dynamic;
mod pg_configuration;
mod setup;
mod version;

pub use dynamic::DynamicConfiguration;
pub use pg_configuration::PgConfiguration;
pub use setup::{CertificateOptions, DocumentDBSetupConfiguration};
pub use version::Version;

use dyn_clone::{clone_trait_object, DynClone};
use std::fmt::Debug;

// These are the required configuration fields.
/// A trait that defines the configuration setup for the application.
/// Implementors of this trait are expected to provide various configuration
/// parameters required for the application to function correctly.
pub trait SetupConfiguration: DynClone + Send + Sync + Debug {
    /// Returns the timeout duration (in seconds) for database cursors.
    fn cursor_timeout_secs(&self) -> u64;

    /// Returns the file path to the dynamic configuration file.
    fn dynamic_configuration_file(&self) -> String;

    /// Returns the refresh interval (in seconds) for reloading the dynamic configuration.
    fn dynamic_configuration_refresh_interval_secs(&self) -> u32;

    /// Returns the name of the backend PostgreSQL database.
    fn postgres_database(&self) -> &str;

    /// Returns the hostname of the backend PostgreSQL server.
    fn postgres_host_name(&self) -> &str;

    /// Returns the port number of the backend PostgreSQL server.
    fn postgres_port(&self) -> u16;

    /// Returns the system user for connecting to the backend PostgreSQL server.
    fn postgres_system_user(&self) -> String;

    /// Returns the timeout duration (in seconds) for transactions.
    fn transaction_timeout_secs(&self) -> u64;

    /// Indicates whether the application should only serve on local host or
    /// be available from all addresses.
    fn use_local_host(&self) -> bool;

    /// Returns the port number on which the gateway listens.
    fn gateway_listen_port(&self) -> u16;

    /// Indicates whether SSL/TCP enforcement is enabled.
    fn enforce_ssl_tcp(&self) -> bool;

    /// Returns a list of role prefixes that are blocked.
    fn blocked_role_prefixes(&self) -> &[String];

    /// Returns the timeout duration (in seconds) for PostgreSQL commands.
    fn postgres_command_timeout_secs(&self) -> u64;

    /// Returns the hostname of the current node for the purposes of the IsDBGrid command.
    fn node_host_name(&self) -> &str;

    /// Returns the certificate options for SSL/TLS connections.
    fn certificate_options(&self) -> Option<CertificateOptions>;

    /// Returns the name of the Gateway application.
    fn application_name(&self) -> &str;

    /// Provides a way to downcast the trait object to a concrete type.
    fn as_any(&self) -> &dyn std::any::Any;
}

clone_trait_object!(SetupConfiguration);
