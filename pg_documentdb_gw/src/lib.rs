/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/lib.rs
 *
 *-------------------------------------------------------------------------
 */

#![allow(clippy::too_many_arguments)]

use configuration::{CertificateOptions, DynamicConfiguration, SetupConfiguration};
use either::Either::{Left, Right};
use error::ErrorCode;
use log::{error, log_enabled, warn};
use openssl::ssl::{Ssl, SslContextBuilder, SslMethod, SslOptions};
use protocol::header::Header;
use requests::compute_request_tracker::ComputeRequestTracker;
use requests::{ComputeRequestInterval, Request, RequestInfo, RequestMessage};
use responses::{CommandError, Response};
use socket2::TcpKeepalive;
use std::sync::Arc;
use std::{net::SocketAddr, pin::Pin, time::Duration};
use telemetry::TelemetryProvider;
use tokio::io::{AsyncRead, AsyncWrite, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};
use tokio_openssl::SslStream;
use tokio_util::sync::CancellationToken;

use crate::context::{ConnectionContext, ServiceContext};
use crate::error::{DocumentDBError, Result};
use crate::postgres::Pool;
use crate::requests::RequestType;

pub use crate::postgres::QueryCatalog;

pub mod auth;
pub mod bson;
pub mod configuration;
pub mod context;
pub mod error;
pub mod explain;
pub mod postgres;
pub mod processor;
pub mod protocol;
pub mod requests;
pub mod responses;
pub mod telemetry;

pub async fn run_server(
    sc: ServiceContext,
    certificate_options: CertificateOptions,
    telemetry: Option<Box<dyn TelemetryProvider>>,
    token: CancellationToken,
) -> Result<()> {
    let listener = TcpListener::bind(format!(
        "{}:{}",
        if sc.setup_configuration().use_local_host() {
            "127.0.0.1"
        } else {
            "[::]"
        },
        sc.setup_configuration().gateway_listen_port(),
    ))
    .await?;

    let enforce_ssl_tcp = sc.setup_configuration().enforce_ssl_tcp();
    // Listen for new tcp connections until cancelled
    loop {
        match listen_for_connections(
            sc.clone(),
            &certificate_options,
            telemetry.clone(),
            &listener,
            token.clone(),
            enforce_ssl_tcp,
        )
        .await
        {
            Err(e) => error!("[], Failed to accept a connection: {:?}", e),
            Ok(true) => continue,
            Ok(false) => break Ok(()),
        }
    }
}

pub async fn get_service_context(
    setup_configuration: Box<dyn SetupConfiguration>,
    dynamic: Arc<dyn DynamicConfiguration>,
    query_catalog: QueryCatalog,
    system_pool: Arc<Pool>,
) -> Result<ServiceContext> {
    let mut i = 0;
    let mut interval = tokio::time::interval(Duration::from_secs(30));
    loop {
        interval.tick().await;
        match ServiceContext::new(
            setup_configuration.clone(),
            dynamic.clone(),
            query_catalog.clone(),
            system_pool.clone(),
        )
        .await
        {
            Ok(sc) => return Ok(sc),
            Err(e) if i >= 10 => return Err(e),
            Err(e) => log::debug!("Failed to initialize service context: {}", e),
        }
        i += 1;
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

async fn listen_for_connections(
    sc: ServiceContext,
    certificate_options: &CertificateOptions,
    telemetry: Option<Box<dyn TelemetryProvider>>,
    listener: &TcpListener,
    token: CancellationToken,
    enforce_ssl_tcp: bool,
) -> Result<bool> {
    let token_clone = token.clone();

    let mut ctx = SslContextBuilder::new(SslMethod::tls()).unwrap();
    ctx.set_private_key_file(
        &certificate_options.key_file_path,
        openssl::ssl::SslFiletype::PEM,
    )?;
    ctx.set_certificate_chain_file(&certificate_options.file_path)?;
    if let Some(ca_path) = certificate_options.ca_path.as_deref() {
        ctx.set_ca_file(ca_path)?;
    }
    ctx.set_options(SslOptions::NO_TLSV1 | SslOptions::NO_TLSV1_1);

    let ssl = Ssl::new(&ctx.build())?;
    // Wait for either a new connection or cancellation
    tokio::select! {
        res = listener.accept() => {
            // Recieve the connection and spawn a thread which handles it until cancelled
            let (stream, ip) = res?;
            log::trace!("New TCP connection established.");

            stream.set_nodelay(true)?;
            socket2::SockRef::from(&stream).set_tcp_keepalive(&TcpKeepalive::new().with_interval(Duration::from_secs(60)).with_time(Duration::from_secs(180)))?;
            tokio::spawn(async move {
                tokio::select! {
                    _ = handle_connection(ssl, sc, telemetry, ip, stream, enforce_ssl_tcp) => {}
                    _ = token_clone.cancelled() => {}
                }
            });
            Ok(true)
        }
        _ = token_clone.cancelled() => {
            Ok(false)
        }
    }
}

async fn get_stream(ssl: Ssl, stream: TcpStream) -> Result<SslStream<TcpStream>> {
    let mut ssl_stream = SslStream::new(ssl, stream)?;
    Pin::new(&mut ssl_stream).accept().await?;
    Ok(ssl_stream)
}

async fn handle_connection(
    ssl: Ssl,
    sc: ServiceContext,
    telemetry: Option<Box<dyn TelemetryProvider>>,
    ip: SocketAddr,
    stream: TcpStream,
    enforce_ssl_tcp: bool,
) {
    let mut connection_context =
        ConnectionContext::new(sc, telemetry, ip, ssl.version_str().to_string()).await;

    match enforce_ssl_tcp {
        true => match get_stream(ssl, stream).await {
            Ok(stream) => {
                connection_context.cipher_type =
                    get_cipher_type(stream.ssl().current_cipher().map(|cipher| cipher.name()));
                handle_stream(stream, connection_context).await
            }
            Err(e) => log::error!("Failed to create SslStream: {}", e),
        },
        false => handle_stream(stream, connection_context).await,
    }
}

async fn handle_stream<R>(mut stream: R, mut connection_context: ConnectionContext)
where
    R: AsyncRead + AsyncWrite + Unpin + Send,
{
    loop {
        match protocol::reader::read_header(&mut stream).await {
            Ok(Some(header)) => {
                if let Err(e) = handle_message(&mut connection_context, &header, &mut stream).await
                {
                    if let Err(e) = log_and_write_error(
                        &connection_context,
                        &header,
                        &e,
                        None,
                        &mut stream,
                        None,
                        &mut ComputeRequestTracker::default(),
                    )
                    .await
                    {
                        error!("[{}] Couldn't reply with error {:?}", header.request_id, e);
                        break;
                    }
                }
            }

            Ok(None) => {
                log::debug!("[{}] Connection closed", connection_context.connection_id);
                break;
            }

            // Failed to read a header, can't provide request id in the error so use connection id instead.
            Err(e) => {
                if let Err(e) = responses::writer::write_error_without_header(
                    &connection_context,
                    e,
                    &mut stream,
                )
                .await
                {
                    warn!(
                        "[C:{}] Couldn't reply with error {:?}",
                        connection_context.connection_id, e
                    );
                    break;
                }
            }
        }
    }
}

async fn get_response(
    ctx: &mut ConnectionContext,
    message: &RequestMessage,
    request: &Request<'_>,
    request_info: &RequestInfo<'_>,
    header: &Header,
) -> Result<Response> {
    // Parse the request bytes into a type and the contained document(s)
    if request.request_type() != &RequestType::IsMaster {
        log::debug!(
            activity_id = header.activity_id.as_str();
            "Request: {:?} - {:?}",
            message.op_code,
            request.request_type(),
        );

        // request.to_json() can be expensive on the order of the size of the request
        if log::log_enabled!(log::Level::Trace) {
            log::trace!(activity_id = header.activity_id.as_str(); "Request: {:?} - {}", message.op_code, request.to_json()?);
        }
    }

    if !ctx.auth_state.authorized || request.request_type().handle_with_auth() {
        let response = auth::process(ctx, request).await?;
        return Ok(response);
    }

    // Once authorized, make sure that there is a pool of pg clients for the user/password.
    ctx.ensure_client_pool(
        ctx.auth_state.username()?,
        ctx.auth_state
            .password
            .as_ref()
            .ok_or(DocumentDBError::internal_error(
                "Password missing for connection pool.".to_string(),
            ))?,
    )
    .await?;

    // Process the actual request
    let response = processor::process_request(request, request_info, ctx).await?;

    Ok(response)
}

async fn handle_message<R>(
    connection_context: &mut ConnectionContext,
    header: &Header,
    stream: &mut R,
) -> Result<()>
where
    R: AsyncRead + AsyncWrite + Unpin + Send,
{
    // Read the request message off the stream

    let mut compute_request_tracker = ComputeRequestTracker::new();

    let message = protocol::reader::read_request(header, stream, connection_context).await?;

    log::trace!(activity_id = header.activity_id.as_str(); "[{}] Request parsed.", header.request_id);

    if connection_context
        .dynamic_configuration()
        .send_shutdown_responses()
        .await
    {
        return Err(DocumentDBError::documentdb_error(
            ErrorCode::ShutdownInProgress,
            "Graceful shutdown requested".to_string(),
        ));
    }

    let start = compute_request_tracker.start_timer();
    let request = protocol::reader::parse_request(&message, connection_context).await?;
    compute_request_tracker.record_duration(ComputeRequestInterval::FormatRequest, start);

    if log_enabled!(log::Level::Trace) {
        log::trace!(activity_id = header.activity_id.as_str(); "Request: {}", request.to_json()?)
    }

    let mut collection = String::new();
    let start = compute_request_tracker.start_timer();
    let result = handle_request(
        connection_context,
        header,
        &request,
        &message,
        stream,
        &mut collection,
        &mut compute_request_tracker,
    )
    .await;
    compute_request_tracker.record_duration(ComputeRequestInterval::RequestDuration, start);

    // Errors in request handling are handled explicitly so that telemetry can have access to the request
    // Returns Ok afterwards so that higher level error telemetry is not invoked.
    if let Err(e) = result {
        if let Err(e) = log_and_write_error(
            connection_context,
            header,
            &e,
            Some(&request),
            stream,
            Some(collection),
            &mut compute_request_tracker,
        )
        .await
        {
            error!("[{}] Couldn't reply with error {:?}", header.request_id, e);
        }
    }

    Ok(())
}

async fn handle_request<R>(
    ctx: &mut ConnectionContext,
    header: &Header,
    request: &Request<'_>,
    message: &RequestMessage,
    stream: &mut R,
    collection: &mut String,
    compute_request_tracker: &mut ComputeRequestTracker,
) -> Result<()>
where
    R: AsyncRead + AsyncWrite + Unpin + Send,
{
    let request_info = request.extract_common()?;
    *collection = request_info.collection().unwrap_or("").to_string();

    // Process the response for the message
    let start = compute_request_tracker.start_timer();
    let response = get_response(ctx, message, request, &request_info, header).await?;

    // Write the response back to the stream
    if ctx.requires_response {
        responses::writer::write(header, &response, stream).await?;
        stream.flush().await?;
    }

    if let Some(telemetry) = ctx.telemetry_provider.as_ref() {
        telemetry
            .emit_request_event(
                ctx,
                header,
                Some(request),
                Left(&response),
                collection.to_string(),
                compute_request_tracker,
            )
            .await;
    }

    compute_request_tracker.record_duration(ComputeRequestInterval::HandleResponse, start);

    Ok(())
}

async fn log_and_write_error<R>(
    connection_context: &ConnectionContext,
    header: &Header,
    e: &DocumentDBError,
    request: Option<&Request<'_>>,
    stream: &mut R,
    collection: Option<String>,
    compute_request_tracker: &mut ComputeRequestTracker,
) -> Result<()>
where
    R: AsyncRead + AsyncWrite + Unpin + Send,
{
    let error_response = CommandError::from_error(connection_context, e).await;
    let response = error_response.to_raw_document_buf()?;

    responses::writer::write_response(header, &response, stream).await?;

    log::error!(activity_id = header.activity_id.as_str(); "Request failure: {e}");

    if let Some(telemetry) = connection_context.telemetry_provider.as_ref() {
        telemetry
            .emit_request_event(
                connection_context,
                header,
                request,
                Right((&error_response, response.as_bytes().len())),
                collection.unwrap_or_default(),
                compute_request_tracker,
            )
            .await;
    }

    Ok(())
}

fn get_cipher_type(cipher_name: Option<&str>) -> i32 {
    match cipher_name {
        Some("TLS_AES_256_GCM_SHA384") => 1,
        Some("TLS_CHACHA20_POLY1305_SHA256") => 2,
        Some("TLS_AES_128_GCM_SHA256") => 3,
        Some("ECDHE_ECDSA_AES256_GCM_SHA384") => 4,
        Some("ECDHE_RSA_AES256_GCM_SHA384") => 5,
        Some("DHE_RSA_AES256_GCM_SHA384") => 6,
        Some("ECDHE_ECDSA_CHACHA20_POLY1305") => 7,
        Some("ECDHE_RSA_CHACHA20_POLY1305") => 8,
        Some("DHE_RSA_CHACHA20_POLY1305") => 9,
        Some("ECDHE_ECDSA_AES128_GCM_SHA256") => 10,
        Some("ECDHE_RSA_AES128_GCM_SHA256") => 11,
        Some("DHE_RSA_AES128_GCM_SHA256") => 12,
        Some("ECDHE_ECDSA_AES256_SHA384") => 13,
        Some("ECDHE_RSA_AES256_SHA384") => 14,
        Some("DHE_RSA_AES256_SHA256") => 15,
        Some("ECDHE_ECDSA_AES128_SHA256") => 16,
        Some("ECDHE_RSA_AES128_SHA256") => 17,
        Some("DHE_RSA_AES128_SHA256") => 18,
        Some("ECDHE_ECDSA_AES256_SHA") => 19,
        Some("ECDHE_RSA_AES256_SHA") => 20,
        Some("DHE_RSA_AES256_SHA") => 21,
        Some("ECDHE_ECDSA_AES128_SHA") => 22,
        Some("ECDHE_RSA_AES128_SHA") => 23,
        Some("DHE_RSA_AES128_SHA") => 24,
        Some("RSA_PSK_AES256_GCM_SHA384") => 25,
        Some("DHE_PSK_AES256_GCM_SHA384") => 26,
        Some("RSA_PSK_CHACHA20_POLY1305") => 27,
        Some("DHE_PSK_CHACHA20_POLY1305") => 28,
        Some("ECDHE_PSK_CHACHA20_POLY1305") => 29,
        Some("AES256_GCM_SHA384") => 30,
        Some("PSK_AES256_GCM_SHA384") => 31,
        Some("PSK_CHACHA20_POLY1305") => 32,
        Some("RSA_PSK_AES128_GCM_SHA256") => 33,
        Some("DHE_PSK_AES128_GCM_SHA256") => 34,
        Some("AES128_GCM_SHA256") => 35,
        Some("PSK_AES128_GCM_SHA256") => 36,
        Some("AES256_SHA256") => 37,
        Some("AES128_SHA256") => 38,
        Some("ECDHE_PSK_AES256_CBC_SHA384") => 39,
        Some("ECDHE_PSK_AES256_CBC_SHA") => 40,
        Some("SRP_RSA_AES_256_CBC_SHA") => 41,
        Some("SRP_AES_256_CBC_SHA") => 42,
        Some("RSA_PSK_AES256_CBC_SHA384") => 43,
        Some("DHE_PSK_AES256_CBC_SHA384") => 44,
        Some("RSA_PSK_AES256_CBC_SHA") => 45,
        Some("DHE_PSK_AES256_CBC_SHA") => 46,
        Some("AES256_SHA") => 47,
        Some("PSK_AES256_CBC_SHA384") => 48,
        Some("PSK_AES256_CBC_SHA") => 49,
        Some("ECDHE_PSK_AES128_CBC_SHA256") => 50,
        Some("ECDHE_PSK_AES128_CBC_SHA") => 51,
        Some("SRP_RSA_AES_128_CBC_SHA") => 52,
        Some("SRP_AES_128_CBC_SHA") => 53,
        Some("RSA_PSK_AES128_CBC_SHA256") => 54,
        Some("DHE_PSK_AES128_CBC_SHA256") => 55,
        Some("RSA_PSK_AES128_CBC_SHA") => 56,
        Some("DHE_PSK_AES128_CBC_SHA") => 57,
        Some("AES128_SHA") => 58,
        Some("PSK_AES128_CBC_SHA256") => 59,
        Some("PSK_AES128_CBC_SHA") => 60,
        Some(_) => -1, // Unknown cipher
        None => 0,     // No cipher (possibly not using SSL/TLS)
    }
}
