/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/lib.rs
 *
 *-------------------------------------------------------------------------
 */

#![allow(clippy::too_many_arguments)]

use configuration::CertificateOptions;
use either::Either::{Left, Right};
use error::ErrorCode;
use log::{error, log_enabled, warn};
use openssl::ssl::{Ssl, SslContextBuilder, SslMethod, SslOptions};
use protocol::header::Header;
use requests::request_tracker::RequestTracker;
use requests::{Request, RequestInfo, RequestIntervalKind, RequestMessage};
use responses::{CommandError, Response};
use socket2::TcpKeepalive;
use std::sync::Arc;
use std::{net::SocketAddr, pin::Pin, time::Duration};
use telemetry::TelemetryProvider;
use tokio::io::{AsyncRead, AsyncWrite, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};
use tokio_openssl::SslStream;
use tokio_util::sync::CancellationToken;

use crate::context::{ConnectionContext, RequestContext, ServiceContext};
use crate::error::{DocumentDBError, Result};
use crate::postgres::PgDataClient;
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
pub mod shutdown_controller;
pub mod startup;
pub mod telemetry;

pub async fn run_server<T>(
    sc: ServiceContext,
    certificate_options: CertificateOptions,
    telemetry: Option<Box<dyn TelemetryProvider>>,
    token: CancellationToken,
    cipher_map: Option<fn(Option<&str>) -> i32>,
) -> Result<()>
where
    T: PgDataClient,
{
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
        match listen_for_connections::<T>(
            sc.clone(),
            &certificate_options,
            telemetry.clone(),
            &listener,
            token.clone(),
            enforce_ssl_tcp,
            cipher_map,
        )
        .await
        {
            Err(e) => error!("[], Failed to accept a connection: {:?}", e),
            Ok(true) => continue,
            Ok(false) => break Ok(()),
        }
    }
}

async fn listen_for_connections<T>(
    sc: ServiceContext,
    certificate_options: &CertificateOptions,
    telemetry: Option<Box<dyn TelemetryProvider>>,
    listener: &TcpListener,
    token: CancellationToken,
    enforce_ssl_tcp: bool,
    cipher_map: Option<fn(Option<&str>) -> i32>,
) -> Result<bool>
where
    T: PgDataClient,
{
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
            // Receive the connection and spawn a thread which handles it until cancelled
            let (stream, ip) = res?;
            log::trace!("New TCP connection established.");

            stream.set_nodelay(true)?;
            socket2::SockRef::from(&stream).set_tcp_keepalive(&TcpKeepalive::new().with_interval(Duration::from_secs(60)).with_time(Duration::from_secs(180)))?;
            tokio::spawn(async move {
                tokio::select! {
                    _ = handle_connection::<T>(ssl, sc, telemetry, ip, stream, enforce_ssl_tcp, cipher_map) => {}
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

async fn handle_connection<T>(
    ssl: Ssl,
    sc: ServiceContext,
    telemetry: Option<Box<dyn TelemetryProvider>>,
    ip: SocketAddr,
    stream: TcpStream,
    enforce_ssl_tcp: bool,
    cipher_map: Option<fn(Option<&str>) -> i32>,
) where
    T: PgDataClient,
{
    let mut connection_context =
        ConnectionContext::new(sc, telemetry, ip, ssl.version_str().to_string()).await;

    match enforce_ssl_tcp {
        true => match get_stream(ssl, stream).await {
            Ok(stream) => {
                connection_context.cipher_type = match cipher_map {
                    Some(map) => map(stream.ssl().current_cipher().map(|cipher| cipher.name())),
                    None => 0,
                };
                handle_stream::<SslStream<TcpStream>, T>(stream, connection_context).await
            }
            Err(e) => log::error!("Failed to create SslStream: {}", e),
        },
        false => handle_stream::<TcpStream, T>(stream, connection_context).await,
    }
}

async fn handle_stream<R, T>(mut stream: R, mut connection_context: ConnectionContext)
where
    R: AsyncRead + AsyncWrite + Unpin + Send,
    T: PgDataClient,
{
    loop {
        match protocol::reader::read_header(&mut stream).await {
            Ok(Some(header)) => {
                if let Err(e) =
                    handle_message::<R, T>(&mut connection_context, &header, &mut stream).await
                {
                    let mut request_info = RequestInfo::new();
                    let mut request_context = RequestContext::new(
                        None,
                        &header,
                        &mut connection_context,
                        &mut request_info,
                    );
                    if let Err(e) =
                        log_and_write_error(&mut request_context, &e, None, &mut stream, None).await
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

async fn get_response<T>(
    request_context: &mut RequestContext<'_>,
    message: &RequestMessage,
    request: &Request<'_>,
) -> Result<Response>
where
    T: PgDataClient,
{
    // Parse the request bytes into a type and the contained document(s)
    if request.request_type() != &RequestType::IsMaster {
        log::debug!(
            activity_id = request_context.activity_id.as_str();
            "Request: {:?} - {:?}",
            message.op_code,
            request.request_type(),
        );

        // request.to_json() can be expensive on the order of the size of the request
        if log::log_enabled!(log::Level::Trace) {
            log::trace!(activity_id = request_context.activity_id.as_str(); "Request: {:?} - {}", message.op_code, request.to_json()?);
        }
    }

    if !request_context.connection_context.auth_state.authorized
        || request.request_type().handle_with_auth()
    {
        let response = auth::process::<T>(
            request_context.connection_context,
            request,
            request_context.header,
        )
        .await?;
        return Ok(response);
    }

    // Once authorized, make sure that there is a pool of pg clients for the user/password.
    request_context
        .connection_context
        .allocate_data_pool()
        .await?;

    let service_context = Arc::clone(&request_context.connection_context.service_context);
    let data_client = T::new_authorized(
        &service_context,
        &request_context.connection_context.auth_state,
    )
    .await?;

    // Process the actual request
    let response = processor::process_request(request, request_context, data_client).await?;

    Ok(response)
}

async fn handle_message<R, T>(
    connection_context: &mut ConnectionContext,
    header: &Header,
    stream: &mut R,
) -> Result<()>
where
    R: AsyncRead + AsyncWrite + Unpin + Send,
    T: PgDataClient,
{
    // Check if the connection is in a shutdown state
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

    // Read the request message off the stream
    let mut request_tracker = RequestTracker::new();

    // TODO: this is a unique activity id for read_request and parse_request to use for logging and telemetry.
    // Currently, it is not used but will be when logging is added.
    let activity_id = RequestContext::generate_activity_id();

    let buffer_read_start = request_tracker.start_timer();
    let message = protocol::reader::read_request(header, stream).await?;
    request_tracker.record_duration(RequestIntervalKind::BufferRead, buffer_read_start);

    let format_request_start = request_tracker.start_timer();
    let request = protocol::reader::parse_request(&message, connection_context).await?;
    request_tracker.record_duration(RequestIntervalKind::FormatRequest, format_request_start);

    let mut request_info = request.extract_common()?;
    request_info.request_tracker = request_tracker;

    let mut request_context = RequestContext::new(
        Some(activity_id),
        header,
        connection_context,
        &mut request_info,
    );

    log::trace!(activity_id = request_context.activity_id.as_str(); "[{}] Request parsed.", header.request_id);

    let handle_request_start = request_context.request_info.request_tracker.start_timer();

    if log_enabled!(log::Level::Trace) {
        log::trace!(activity_id = request_context.activity_id.as_str(); "Request: {}", request.to_json()?)
    }

    let mut collection = String::new();
    let result = handle_request::<R, T>(
        &mut request_context,
        &request,
        &message,
        stream,
        &mut collection,
    )
    .await;
    request_context
        .request_info
        .request_tracker
        .record_duration(RequestIntervalKind::HandleRequest, handle_request_start);

    if request_context
        .connection_context
        .dynamic_configuration()
        .enable_verbose_logging_gateway()
        .await
    {
        log::info!(
            activity_id = request_context.activity_id.as_str();
            "Latency for Mongo Request with ActivityId: {}, BufferRead={}ms, HandleRequest={}ms, FormatRequest={}ms, ProcessRequest={}ms, PostgresBeginTransaction={}ms, PostgresSetStatementTimeout={}ms, PostgresTransactionCommit={}ms, FormatResponse={}ms",
            request_context.activity_id,
            request_context.request_info.request_tracker.get_interval_elapsed_time(RequestIntervalKind::BufferRead),
            request_context.request_info.request_tracker.get_interval_elapsed_time(RequestIntervalKind::HandleRequest),
            request_context.request_info.request_tracker.get_interval_elapsed_time(RequestIntervalKind::FormatRequest),
            request_context.request_info.request_tracker.get_interval_elapsed_time(RequestIntervalKind::ProcessRequest),
            request_context.request_info.request_tracker.get_interval_elapsed_time(RequestIntervalKind::PostgresBeginTransaction),
            request_context.request_info.request_tracker.get_interval_elapsed_time(RequestIntervalKind::PostgresSetStatementTimeout),
            request_context.request_info.request_tracker.get_interval_elapsed_time(RequestIntervalKind::PostgresTransactionCommit),
            request_context.request_info.request_tracker.get_interval_elapsed_time(RequestIntervalKind::FormatResponse)
        );
    }

    // Errors in request handling are handled explicitly so that telemetry can have access to the request
    // Returns Ok afterwards so that higher level error telemetry is not invoked.
    if let Err(e) = result {
        if let Err(e) = log_and_write_error(
            &mut request_context,
            &e,
            Some(&request),
            stream,
            Some(collection),
        )
        .await
        {
            error!("[{}] Couldn't reply with error {:?}", header.request_id, e);
        }
    }

    Ok(())
}

async fn handle_request<R, T>(
    request_context: &mut RequestContext<'_>,
    request: &Request<'_>,
    message: &RequestMessage,
    stream: &mut R,
    collection: &mut String,
) -> Result<()>
where
    R: AsyncRead + AsyncWrite + Unpin + Send,
    T: PgDataClient,
{
    // Process the response for the message
    let response = get_response::<T>(request_context, message, request).await?;

    *collection = request_context
        .request_info
        .collection()
        .unwrap_or("")
        .to_string();

    let format_response_start = request_context.request_info.request_tracker.start_timer();

    // Write the response back to the stream
    if request_context.connection_context.requires_response {
        responses::writer::write(request_context.header, &response, stream).await?;
        stream.flush().await?;
    }

    request_context
        .request_info
        .request_tracker
        .record_duration(RequestIntervalKind::FormatResponse, format_response_start);

    if let Some(telemetry) = request_context
        .connection_context
        .telemetry_provider
        .clone()
    {
        telemetry
            .emit_request_event(
                request_context,
                Some(request),
                Left(&response),
                collection.to_string(),
            )
            .await;
    }

    Ok(())
}

async fn log_and_write_error<R>(
    request_context: &mut RequestContext<'_>,
    e: &DocumentDBError,
    request: Option<&Request<'_>>,
    stream: &mut R,
    collection: Option<String>,
) -> Result<()>
where
    R: AsyncRead + AsyncWrite + Unpin + Send,
{
    let error_response = CommandError::from_error(request_context.connection_context, e).await;
    let response = error_response.to_raw_document_buf()?;

    responses::writer::write_response(request_context.header, &response, stream).await?;

    log::error!(activity_id = request_context.activity_id.as_str(); "Request failure: {e}");

    if let Some(telemetry) = request_context
        .connection_context
        .telemetry_provider
        .clone()
    {
        telemetry
            .emit_request_event(
                request_context,
                request,
                Right((&error_response, response.as_bytes().len())),
                collection.unwrap_or_default(),
            )
            .await;
    }

    Ok(())
}
