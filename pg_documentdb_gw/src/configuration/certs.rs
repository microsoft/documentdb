/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/configuration/certs.rs
 *
 *-------------------------------------------------------------------------
 */

use std::sync::Arc;

use crate::error::Result;
use arc_swap::ArcSwap;
use openssl::ssl::{SslContext, SslContextBuilder, SslMethod, SslOptions};
use serde::Deserialize;

const CERT_CHECK_REFRESH_INTERVAL_SECONDS: u64 = 60;

#[derive(Debug, Deserialize, Default, Clone)]
#[serde(rename_all = "PascalCase")]
pub struct CertificateOptions {
    pub cert_type: String,
    pub file_path: String,
    pub key_file_path: String,
    pub ca_path: Option<String>,
}

pub struct CertificateProvider {
    ssl_context: Arc<ArcSwap<SslContext>>,
    _refresh_thread: tokio::task::JoinHandle<()>,
}

impl CertificateProvider {
    fn load_ssl_context(certificate_options: &CertificateOptions) -> Result<SslContext> {
        let mut ssl_context = SslContextBuilder::new(SslMethod::tls()).unwrap();
        ssl_context.set_private_key_file(
            &certificate_options.key_file_path,
            openssl::ssl::SslFiletype::PEM,
        )?;
        ssl_context.set_certificate_chain_file(&certificate_options.file_path)?;
        if let Some(ca_path) = certificate_options.ca_path.as_deref() {
            ssl_context.set_ca_file(ca_path)?;
        }
        ssl_context.set_options(SslOptions::NO_TLSV1 | SslOptions::NO_TLSV1_1);

        Ok(ssl_context.build())
    }

    async fn get_modified_time(path: &str) -> Result<std::time::SystemTime> {
        let metadata = tokio::fs::metadata(path).await?;
        Ok(metadata.modified()?)
    }

    pub async fn new(certificate_options: &CertificateOptions) -> Result<Self> {
        let ssl_context_as_arc = Arc::new(ArcSwap::from_pointee(Self::load_ssl_context(
            certificate_options,
        )?));

        let mut last_cert_modified =
            Self::get_modified_time(&certificate_options.file_path).await?;
        let mut last_key_modified =
            Self::get_modified_time(&certificate_options.key_file_path).await?;
        let ssl_context_arc_clone = Arc::clone(&ssl_context_as_arc);
        let certificate_options = certificate_options.clone();

        let refresh_thread = tokio::spawn(async move {
            let mut ticker = tokio::time::interval(tokio::time::Duration::from_secs(
                CERT_CHECK_REFRESH_INTERVAL_SECONDS,
            ));

            loop {
                ticker.tick().await;
                if let (Ok(cert_m), Ok(key_m)) = (
                    Self::get_modified_time(&certificate_options.file_path).await,
                    Self::get_modified_time(&certificate_options.key_file_path).await,
                ) {
                    if cert_m > last_cert_modified || key_m > last_key_modified {
                        log::info!("Reloading TLS certificate since it has been modified.");
                        match Self::load_ssl_context(&certificate_options) {
                            Ok(new_ssl_context) => {
                                ssl_context_arc_clone.store(Arc::new(new_ssl_context));
                                last_cert_modified = cert_m;
                                last_key_modified = key_m;
                                log::info!("TLS certificate reloaded.");
                            }
                            Err(e) => log::error!("Failed to reload TLS certificate: {:?}", e),
                        }
                    }
                }
            }
        });

        Ok(Self {
            ssl_context: ssl_context_as_arc,
            _refresh_thread: refresh_thread,
        })
    }

    pub fn get_ssl_context(&self) -> Arc<SslContext> {
        self.ssl_context.load().clone()
    }
}
