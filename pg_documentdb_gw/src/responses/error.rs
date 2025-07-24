/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/responses/error.rs
 *
 *-------------------------------------------------------------------------
 */

use std::error::Error;

use bson::raw::ValueAccessErrorKind;
use bson::RawDocumentBuf;
use deadpool_postgres::PoolError;
use serde::{Deserialize, Serialize};
use tokio_postgres::error::SqlState;

use crate::context::ConnectionContext;
use crate::error::{DocumentDBError, ErrorCode, Result};
use crate::protocol::OK_FAILED;
use crate::responses::constant::{
    bson_serialize_error_message, documentdb_error_message, value_access_error_message,
};

use super::pg::PgResponse;

/// Represents an error returned by a command execution.
#[derive(Clone, Debug, Serialize, Deserialize)]
#[non_exhaustive]
pub struct CommandError {
    pub ok: f64,

    /// The error code in i32, e.g. InternalError has error code 1.
    pub code: i32,

    /// The error string, e.g. Internal Error.
    #[serde(rename = "codeName", default)]
    pub code_name: String,

    /// A human-readable description of the error.
    #[serde(rename = "errmsg", default = "String::new")]
    pub message: String,
}

impl CommandError {
    pub fn new(code: i32, code_name: String, msg: String) -> Self {
        CommandError {
            ok: OK_FAILED,
            code,
            code_name,
            message: msg,
        }
    }

    pub fn to_raw_document_buf(&self) -> Result<RawDocumentBuf> {
        bson::to_raw_document_buf(self)
            .map_err(|e| DocumentDBError::internal_error(bson_serialize_error_message(e)))
    }

    fn internal(msg: String) -> Self {
        CommandError::new(
            ErrorCode::InternalError as i32,
            "Internal Error".to_string(),
            msg,
        )
    }

    pub async fn from_error(connection_context: &ConnectionContext, err: &DocumentDBError) -> Self {
        match err {
            DocumentDBError::IoError(e, _) => CommandError::internal(e.to_string()),
            DocumentDBError::PostgresError(e, _) => {
                Self::from_pg_error(connection_context, e).await
            }
            DocumentDBError::PoolError(PoolError::Backend(e), _) => {
                Self::from_pg_error(connection_context, e).await
            }
            DocumentDBError::PostgresDocumentDBError(e, msg, _) => {
                if let Ok(state) = PgResponse::i32_to_postgres_sqlstate(e) {
                    if let Some(known_error) =
                        Self::known_pg_error(connection_context, &state, msg).await
                    {
                        return known_error;
                    }
                }
                CommandError::new(*e, documentdb_error_message(), msg.to_string())
            }
            DocumentDBError::RawBsonError(e, _) => {
                CommandError::internal(format!("Raw BSON error: {}", e))
            }
            DocumentDBError::PoolError(e, _) => {
                CommandError::internal(format!("Pool error: {}", e))
            }
            DocumentDBError::CreatePoolError(e, _) => {
                CommandError::internal(format!("Create pool error: {}", e))
            }
            DocumentDBError::BuildPoolError(e, _) => {
                CommandError::internal(format!("Build pool error: {}", e))
            }
            DocumentDBError::DocumentDBError(error_code, msg, _) => {
                CommandError::new(*error_code as i32, error_code.to_string(), msg.to_string())
            }
            DocumentDBError::UntypedDocumentDBError(error_code, msg, code, _) => {
                CommandError::new(*error_code, code.clone(), msg.to_string())
            }
            DocumentDBError::SSLErrorStack(error_stack, _) => {
                CommandError::internal(format!("SSL error stack: {}", error_stack))
            }
            DocumentDBError::SSLError(error, _) => {
                CommandError::internal(format!("SSL error: {}", error))
            }
            DocumentDBError::ValueAccessError(error, _) => match &error.kind {
                ValueAccessErrorKind::UnexpectedType {
                    actual, expected, ..
                } => CommandError::new(
                    ErrorCode::TypeMismatch as i32,
                    value_access_error_message(),
                    format!(
                        "Expected {:?} but got {:?}, at key {}",
                        expected,
                        actual,
                        error.key()
                    ),
                ),
                ValueAccessErrorKind::InvalidBson(_) => CommandError::new(
                    ErrorCode::BadValue as i32,
                    value_access_error_message(),
                    "Value is not a valid BSON".to_string(),
                ),
                ValueAccessErrorKind::NotPresent => CommandError::new(
                    ErrorCode::BadValue as i32,
                    value_access_error_message(),
                    "Value is not present".to_string(),
                ),
                _ => CommandError::new(
                    ErrorCode::BadValue as i32,
                    value_access_error_message(),
                    "Unexpected value".to_string(),
                ),
            },
        }
    }

    pub async fn from_pg_error(context: &ConnectionContext, e: &tokio_postgres::Error) -> Self {
        if let Some(state) = e.code() {
            if let Some(known_error) =
                Self::known_pg_error(context, state, e.as_db_error().map_or("", |e| e.message()))
                    .await
            {
                return known_error;
            }
            CommandError::new(
                PgResponse::postgres_sqlstate_to_i32(state),
                documentdb_error_message(),
                Self::pg_error_to_msg(e),
            )
        } else {
            CommandError::internal(e.to_string())
        }
    }

    async fn known_pg_error(
        context: &ConnectionContext,
        state: &SqlState,
        msg: &str,
    ) -> Option<Self> {
        if let Some((code, code_name, error_msg)) =
            PgResponse::known_pg_error(context, state, msg).await
        {
            Some(CommandError::new(
                code,
                code_name.unwrap_or(documentdb_error_message()),
                error_msg.to_string(),
            ))
        } else {
            None
        }
    }

    fn pg_error_to_msg(e: &tokio_postgres::Error) -> String {
        if let Some(db_error) = e.as_db_error() {
            db_error.message().to_owned()
        } else {
            e.source().map_or("No Cause.".to_owned(), |s| s.to_string())
        }
    }
}
