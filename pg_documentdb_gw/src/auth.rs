/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/auth.rs
 *
 *-------------------------------------------------------------------------
 */

use std::str::from_utf8;

use bson::{rawdoc, spec::BinarySubtype};
use rand::{distributions::Uniform, prelude::Distribution, rngs::OsRng};
use tokio_postgres::types::Type;

use crate::{
    context::ConnectionContext,
    error::{DocumentDBError, ErrorCode, Result},
    postgres::PgDocument,
    processor,
    protocol::OK_SUCCEEDED,
    requests::{Request, RequestType},
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
        }
    }

    pub fn username(&self) -> Result<&str> {
        self.username
            .as_deref()
            .ok_or(DocumentDBError::internal_error(
                "Username missing".to_string(),
            ))
    }

    pub fn set_username(&mut self, user: &str) {
        self.username = Some(user.to_string());
    }
}

pub async fn process(ctx: &mut ConnectionContext, request: &Request<'_>) -> Result<Response> {
    if let Some(response) = handle_auth_request(ctx, request).await? {
        return Ok(response);
    }

    if request.request_type().allowed_unauthorized() {
        return processor::process_request(request, &request.extract_common()?, ctx).await;
    }

    Err(DocumentDBError::unauthorized(format!(
        "Command {} not supported prior to authentication.",
        request.request_type().to_string().to_lowercase()
    )))
}

async fn handle_auth_request(
    ctx: &mut ConnectionContext,
    request: &Request<'_>,
) -> Result<Option<Response>> {
    match request.request_type() {
        RequestType::SaslStart => Ok(Some(handle_sasl_start(ctx, request).await?)),
        RequestType::SaslContinue => Ok(Some(handle_sasl_continue(ctx, request).await?)),
        RequestType::Logout => {
            ctx.auth_state = AuthState::new();
            Ok(Some(Response::Raw(RawResponse(rawdoc! {
                "ok": OK_SUCCEEDED,
            }))))
        }
        _ => Ok(None),
    }
}

async fn handle_sasl_start(ctx: &mut ConnectionContext, request: &Request<'_>) -> Result<Response> {
    let mechanism = request
        .document()
        .get_str("mechanism")
        .map_err(DocumentDBError::parse_failure())?;

    if mechanism != "SCRAM-SHA-256" {
        return Err(DocumentDBError::unauthorized(
            "Only SCRAM-SHA-256 is supported".to_string(),
        ));
    }

    let payload = parse_sasl_payload(request, true)?;

    let username = payload.username.ok_or(DocumentDBError::unauthorized(
        "Username missing from SaslStart.".to_string(),
    ))?;

    let client_nonce = payload.nonce.ok_or(DocumentDBError::unauthorized(
        "Nonce missing from SaslStart.".to_string(),
    ))?;

    let mut nonce = String::with_capacity(client_nonce.len() + NONCE_LENGTH);
    nonce.push_str(client_nonce);
    nonce.extend(
        Uniform::from(33..125)
            .sample_iter(OsRng)
            .map(|x: u8| if x > 43 { (x + 1) as char } else { x as char })
            .take(NONCE_LENGTH),
    );

    let (salt, iterations) = get_salt_and_iteration(ctx, username).await?;
    let response = format!("r={},s={},i={}", nonce, salt, iterations);

    ctx.auth_state.first_state = Some(ScramFirstState {
        nonce,
        first_message_bare: format!("n={},r={}", username, client_nonce),
        first_message: response.clone(),
    });

    ctx.auth_state.username = Some(username.to_string());

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

async fn handle_sasl_continue(
    ctx: &mut ConnectionContext,
    request: &Request<'_>,
) -> Result<Response> {
    let payload = parse_sasl_payload(request, false)?;

    if let Some(first_state) = ctx.auth_state.first_state.as_ref() {
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
            .or(ctx.auth_state.username.as_deref())
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

        let scram_sha256_row = ctx
            .service_context
            .authentication_connection()
            .await?
            .query(
                ctx.service_context
                    .query_catalog()
                    .authenticate_with_scram_sha256(),
                &[Type::TEXT, Type::TEXT, Type::TEXT],
                &[&username, &auth_message, &proof],
                None,
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

        ctx.auth_state.password = Some("".to_string());
        ctx.auth_state.authorized = true;

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

async fn get_salt_and_iteration(ctx: &ConnectionContext, username: &str) -> Result<(String, i32)> {
    for blocked_prefix in ctx
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

    let results = ctx
        .service_context
        .authentication_connection()
        .await?
        .query(
            ctx.service_context.query_catalog().salt_and_iterations(),
            &[Type::TEXT],
            &[&username],
            None,
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
