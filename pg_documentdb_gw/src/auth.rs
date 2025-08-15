/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/auth.rs
 *
 *-------------------------------------------------------------------------
 */

use std::{str::from_utf8, sync::Arc};

use base64::{engine::general_purpose, Engine as _};
use bson::{rawdoc, spec::BinarySubtype};
use rand::Rng;
use serde_json::Value;
use tokio_postgres::types::Type;

use crate::{
    context::ConnectionContext,
    error::{DocumentDBError, ErrorCode, Result},
    postgres::{PgDataClient, PgDocument},
    processor,
    protocol::OK_SUCCEEDED,
    requests::{Request, RequestInfo, RequestType},
    responses::{RawResponse, Response},
};

const NONCE_LENGTH: usize = 2;

pub struct ScramFirstState {
    nonce: String,
    first_message_bare: String,
    first_message: String,
}

pub struct AuthState {
    pub authorized: bool,
    first_state: Option<ScramFirstState>,
    username: Option<String>,
    pub password: Option<String>,
    user_oid: Option<u32>,
}

impl Default for AuthState {
    fn default() -> Self {
        Self::new()
    }
}

impl AuthState {
    pub fn new() -> Self {
        AuthState {
            authorized: false,
            first_state: None,
            username: None,
            password: None,
            user_oid: None,
        }
    }

    pub fn username(&self) -> Result<&str> {
        self.username
            .as_deref()
            .ok_or(DocumentDBError::internal_error(
                "Username missing".to_string(),
            ))
    }

    pub fn user_oid(&self) -> Result<u32> {
        self.user_oid.ok_or(DocumentDBError::internal_error(
            "User OID missing".to_string(),
        ))
    }

    pub fn set_username(&mut self, user: &str) {
        self.username = Some(user.to_string());
    }

    pub fn set_user_oid(&mut self, user_oid: u32) {
        self.user_oid = Some(user_oid);
    }
}

pub async fn process<T>(
    connection_context: &mut ConnectionContext,
    request: &Request<'_>,
) -> Result<Response>
where
    T: PgDataClient,
{
    if let Some(response) = handle_auth_request(connection_context, request).await? {
        return Ok(response);
    }

    let request_info = request.extract_common();
    if request.request_type().allowed_unauthorized() {
        let service_context = Arc::clone(&connection_context.service_context);
        let data_client = T::new_unauthorized(&service_context).await?;
        return processor::process_request(
            request,
            &mut request_info?,
            connection_context,
            data_client,
        )
        .await;
    }

    Err(DocumentDBError::unauthorized(format!(
        "Command {} not supported prior to authentication.",
        request.request_type().to_string().to_lowercase()
    )))
}

async fn handle_auth_request(
    connection_context: &mut ConnectionContext,
    request: &Request<'_>,
) -> Result<Option<Response>> {
    match request.request_type() {
        RequestType::SaslStart => Ok(Some(handle_sasl_start(connection_context, request).await?)),
        RequestType::SaslContinue => Ok(Some(
            handle_sasl_continue(connection_context, request).await?,
        )),
        RequestType::Logout => {
            connection_context.auth_state = AuthState::new();
            Ok(Some(Response::Raw(RawResponse(rawdoc! {
                "ok": OK_SUCCEEDED,
            }))))
        }
        _ => Ok(None),
    }
}

fn generate_server_nonce(client_nonce: &str) -> String {
    const CHARSET: &[u8] = b"!\"#$%&'()*+-./0123456789:;<>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~";
    let mut rng = rand::thread_rng();

    let mut result = String::with_capacity(NONCE_LENGTH);
    for _ in 0..NONCE_LENGTH {
        let idx = rng.gen_range(0..CHARSET.len());
        result.push(CHARSET[idx] as char);
    }

    format!("{}{}", client_nonce, result)
}

async fn handle_sasl_start(
    connection_context: &mut ConnectionContext,
    request: &Request<'_>,
) -> Result<Response> {
    let mechanism = request
        .document()
        .get_str("mechanism")
        .map_err(DocumentDBError::parse_failure())?;

    if mechanism != "SCRAM-SHA-256" && mechanism != "MONGODB-OIDC" {
        return Err(DocumentDBError::unauthorized(format!(
            "Only SCRAM-SHA-256 and MONGODB-OIDC are supported, got: {}",
            mechanism
        )));
    }

    if mechanism == "MONGODB-OIDC" {
        return handle_oidc(connection_context, request).await;
    } else {
        return handle_scram(connection_context, request).await;
    }
}

async fn handle_scram(
    connection_context: &mut ConnectionContext,
    request: &Request<'_>,
) -> Result<Response> {
    let payload = parse_sasl_payload(request, true)?;

    let username = payload.username.ok_or(DocumentDBError::unauthorized(
        "Username missing from SaslStart.".to_string(),
    ))?;

    let client_nonce = payload.nonce.ok_or(DocumentDBError::unauthorized(
        "Nonce missing from SaslStart.".to_string(),
    ))?;

    let server_nonce = generate_server_nonce(client_nonce);

    let (salt, iterations) = get_salt_and_iteration(connection_context, username).await?;
    let response = format!("r={},s={},i={}", server_nonce, salt, iterations);

    connection_context.auth_state.first_state = Some(ScramFirstState {
        nonce: server_nonce,
        first_message_bare: format!("n={},r={}", username, client_nonce),
        first_message: response.clone(),
    });

    connection_context.auth_state.username = Some(username.to_string());

    let binary_response = bson::Binary {
        subtype: BinarySubtype::Generic,
        bytes: response.as_bytes().to_vec(),
    };

    Ok(Response::Raw(RawResponse(rawdoc! {
        "payload": binary_response,
        "ok": OK_SUCCEEDED,
        "conversationId": 1,
        "done": false
    })))
}

async fn handle_oidc(
    connection_context: &mut ConnectionContext,
    request: &Request<'_>,
) -> Result<Response> {
    let payload = request
        .document()
        .get_binary("payload")
        .map_err(DocumentDBError::parse_failure())?;

    let payload_doc = bson::Document::from_reader(&mut std::io::Cursor::new(payload.bytes))
        .map_err(|e| {
            DocumentDBError::bad_value(format!("Failed to parse OIDC payload as BSON: {}", e))
        })?;

    let jwt_token = payload_doc.get_str("jwt").map_err(|_| {
        DocumentDBError::unauthorized("JWT token missing from OIDC payload".to_string())
    })?;

    handle_oidc_token_authentication(connection_context, jwt_token).await
}

async fn handle_oidc_token_authentication(
    connection_context: &mut ConnectionContext,
    token_string: &str,
) -> Result<Response> {
    let oid = parse_and_validate_jwt_token(token_string)?;

    let authentication_token_row = connection_context
        .service_context
        .authentication_connection()
        .await?
        .query(
            connection_context
                .service_context
                .query_catalog()
                .authenticate_with_token(),
            &[Type::TEXT, Type::TEXT],
            &[&oid, &token_string],
            None,
            &mut RequestInfo::new(),
        )
        .await?;

    let authentication_result: String = authentication_token_row
        .first()
        .ok_or(DocumentDBError::pg_response_empty())?
        .try_get(0)?;

    if authentication_result.trim() != oid {
        return Err(DocumentDBError::unauthorized(
            "Token validation failed".to_string(),
        ));
    }

    let server_signature = "";
    let payload = bson::Binary {
        subtype: BinarySubtype::Generic,
        bytes: server_signature.as_bytes().to_vec(),
    };

    connection_context.auth_state.set_username(&oid);
    connection_context.auth_state.password = Some(token_string.to_string());
    connection_context.auth_state.user_oid = Some(get_user_oid(connection_context, &oid).await?);
    connection_context.auth_state.authorized = true;

    Ok(Response::Raw(RawResponse(rawdoc! {
        "payload": payload,
        "ok": OK_SUCCEEDED,
        "conversationId": 1,
        "done": true
    })))
}

fn parse_and_validate_jwt_token(token_string: &str) -> Result<String> {
    let token_parts: Vec<&str> = token_string.split('.').collect();
    if token_parts.len() != 3 {
        return Err(DocumentDBError::unauthorized(
            "Invalid JWT token format.".to_string(),
        ));
    }

    let payload_part = token_parts[1];
    let payload_bytes = general_purpose::URL_SAFE_NO_PAD
        .decode(payload_part)
        .map_err(|_| DocumentDBError::unauthorized("Invalid JWT token encoding.".to_string()))?;

    let payload_json: Value = serde_json::from_slice(&payload_bytes)
        .map_err(|_| DocumentDBError::unauthorized("Invalid JWT token payload.".to_string()))?;

    let oid = payload_json
        .get("oid")
        .and_then(|v| v.as_str())
        .ok_or_else(|| DocumentDBError::unauthorized("Token does not contain OID.".to_string()))?
        .to_string();

    let aud = payload_json
        .get("aud")
        .and_then(|v| v.as_str())
        .ok_or_else(|| {
            DocumentDBError::unauthorized("Token does not contain audience claim.".to_string())
        })?
        .to_string();

    let exp = payload_json
        .get("exp")
        .and_then(|v| v.as_i64())
        .ok_or_else(|| {
            DocumentDBError::unauthorized("Token does not contain expiry time.".to_string())
        })?;

    let valid_audiences = ["https://ossrdbms-aad.database.windows.net"];
    if !valid_audiences.contains(&aud.as_str()) {
        return Err(DocumentDBError::unauthorized(
            "Invalid audience claim.".to_string(),
        ));
    }

    let exp_datetime = std::time::UNIX_EPOCH + std::time::Duration::from_secs(exp as u64);
    let now = std::time::SystemTime::now();

    if exp_datetime < now {
        return Err(DocumentDBError::reauthentication_required(
            "Token has expired.".to_string(),
        ));
    }

    Ok(oid)
}

async fn handle_sasl_continue(
    connection_context: &mut ConnectionContext,
    request: &Request<'_>,
) -> Result<Response> {
    let payload = parse_sasl_payload(request, false)?;

    if let Some(first_state) = connection_context.auth_state.first_state.as_ref() {
        // Username is not always provided by saslcontinue

        let client_nonce = payload.nonce.ok_or(DocumentDBError::unauthorized(
            "Nonce missing from SaslContinue.".to_string(),
        ))?;
        let proof = payload.proof.ok_or(DocumentDBError::unauthorized(
            "Proof missing from SaslContinue.".to_string(),
        ))?;
        let channel_binding = payload
            .channel_binding
            .ok_or(DocumentDBError::unauthorized(
                "Channel binding missing from SaslContinue.".to_string(),
            ))?;
        let username = payload
            .username
            .or(connection_context.auth_state.username.as_deref())
            .ok_or(DocumentDBError::internal_error(
                "Username missing from sasl continue".to_string(),
            ))?;

        if client_nonce != first_state.nonce {
            return Err(DocumentDBError::unauthorized(
                "Nonce did not match expected nonce.".to_string(),
            ));
        }

        let auth_message = format!(
            "{},{},c={},r={}",
            first_state.first_message_bare,
            first_state.first_message,
            channel_binding,
            client_nonce
        );

        let scram_sha256_row = connection_context
            .service_context
            .authentication_connection()
            .await?
            .query(
                connection_context
                    .service_context
                    .query_catalog()
                    .authenticate_with_scram_sha256(),
                &[Type::TEXT, Type::TEXT, Type::TEXT],
                &[&username, &auth_message, &proof],
                None,
                &mut RequestInfo::new(),
            )
            .await?;

        let scram_sha256_doc: PgDocument = scram_sha256_row
            .first()
            .ok_or(DocumentDBError::pg_response_empty())?
            .try_get(0)?;

        if scram_sha256_doc
            .0
            .get_i32("ok")
            .map_err(DocumentDBError::pg_response_invalid)?
            != 1
        {
            return Err(DocumentDBError::unauthorized("Invalid key".to_string()));
        }

        let server_signature = scram_sha256_doc
            .0
            .get_str("ServerSignature")
            .map_err(DocumentDBError::pg_response_invalid)?;

        let payload = bson::Binary {
            subtype: BinarySubtype::Generic,
            bytes: format!("v={}", server_signature).as_bytes().to_vec(),
        };

        connection_context.auth_state.password = Some("".to_string());
        connection_context.auth_state.user_oid =
            Some(get_user_oid(connection_context, username).await?);
        connection_context.auth_state.authorized = true;

        Ok(Response::Raw(RawResponse(rawdoc! {
            "payload": payload,
            "ok": OK_SUCCEEDED,
            "conversationId": 1,
            "done": true
        })))
    } else {
        Err(DocumentDBError::unauthorized(
            "Sasl Continue called without SaslStart state.".to_string(),
        ))
    }
}

struct ScramPayload<'a> {
    username: Option<&'a str>,
    nonce: Option<&'a str>,
    proof: Option<&'a str>,
    channel_binding: Option<&'a str>,
}

fn parse_sasl_payload<'a, 'b: 'a>(
    request: &'b Request<'a>,
    with_header: bool,
) -> Result<ScramPayload<'a>> {
    let payload = request
        .document()
        .get_binary("payload")
        .map_err(DocumentDBError::parse_failure())?;
    let mut payload = from_utf8(payload.bytes).map_err(|e| {
        DocumentDBError::bad_value(format!(
            "Sasl payload couldn't be converted to utf-8: {}",
            e
        ))
    })?;

    if with_header {
        if payload.len() < 3 {
            return Err(DocumentDBError::sasl_payload_invalid());
        }
        match &payload[0..=2] {
            "n,," => (),
            "p,," => (),
            "y,," => (),
            _ => return Err(DocumentDBError::sasl_payload_invalid()),
        }
        payload = &payload[3..];
    }

    let mut username: Option<&str> = None;
    let mut nonce: Option<&str> = None;
    let mut proof: Option<&str> = None;
    let mut channel_binding: Option<&str> = None;

    for value in payload.split(',') {
        let idx = value
            .find('=')
            .ok_or(DocumentDBError::sasl_payload_invalid())?;

        let k = &value[..idx];
        let v = &value[idx + 1..];
        match k {
            "n" => username = Some(v),
            "r" => nonce = Some(v),
            "p" => proof = Some(v),
            "c" => channel_binding = Some(v),
            _ => {
                return Err(DocumentDBError::unauthorized(
                    "Sasl payload was invalid.".to_string(),
                ))
            }
        }
    }

    Ok(ScramPayload {
        username,
        nonce,
        proof,
        channel_binding,
    })
}

async fn get_salt_and_iteration(
    connection_context: &ConnectionContext,
    username: &str,
) -> Result<(String, i32)> {
    for blocked_prefix in connection_context
        .service_context
        .setup_configuration()
        .blocked_role_prefixes()
    {
        if username
            .to_lowercase()
            .starts_with(&blocked_prefix.to_lowercase())
        {
            return Err(DocumentDBError::unauthorized(
                "Username is invalid.".to_string(),
            ));
        }
    }

    let results = connection_context
        .service_context
        .authentication_connection()
        .await?
        .query(
            connection_context
                .service_context
                .query_catalog()
                .salt_and_iterations(),
            &[Type::TEXT],
            &[&username],
            None,
            &mut RequestInfo::new(),
        )
        .await?;

    let doc: PgDocument = results
        .first()
        .ok_or(DocumentDBError::pg_response_empty())?
        .try_get(0)?;
    if doc
        .0
        .get_i32("ok")
        .map_err(|e| DocumentDBError::internal_error(e.to_string()))?
        != 1
    {
        return Err(DocumentDBError::documentdb_error(
            ErrorCode::AuthenticationFailed,
            "Invalid account: User details not found in the database".to_string(),
        ));
    }

    let iterations = doc
        .0
        .get_i32("iterations")
        .map_err(DocumentDBError::pg_response_invalid)?;
    let salt = doc
        .0
        .get_str("salt")
        .map_err(DocumentDBError::pg_response_invalid)?;

    Ok((salt.to_string(), iterations))
}

pub async fn get_user_oid(connection_context: &ConnectionContext, username: &str) -> Result<u32> {
    let user_oid_rows = connection_context
        .service_context
        .authentication_connection()
        .await?
        .query(
            "SELECT oid from pg_roles WHERE rolname = $1",
            &[Type::TEXT],
            &[&username],
            None,
            &mut RequestInfo::new(),
        )
        .await?;

    let user_oid = user_oid_rows
        .first()
        .ok_or(DocumentDBError::pg_response_empty())?
        .try_get::<_, tokio_postgres::types::Oid>(0)?;

    Ok(user_oid as u32)
}
