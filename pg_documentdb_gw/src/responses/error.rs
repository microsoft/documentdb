use std::error::Error;

use bson::raw::ValueAccessErrorKind;
use bson::RawDocumentBuf;
use deadpool_postgres::PoolError;
use serde::{Deserialize, Serialize};
use tokio_postgres::error::SqlState;

use crate::context::ConnectionContext;
use crate::error::{DocumentDBError, ErrorCode, Result};
use crate::protocol::OK_FAILED;

use super::pg::PgResponse;

/// An error that occurred due to a database command failing.
#[derive(Clone, Debug, Serialize, Deserialize)]
#[non_exhaustive]
pub struct CommandError {
    pub ok: f64,

    /// Identifies the type of error.
    pub code: i32,

    /// The name associated with the error code.
    #[serde(rename = "codeName", default)]
    pub code_name: String,

    /// A description of the error that occurred.
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
        ::bson::to_raw_document_buf(self).map_err(|e| {
            DocumentDBError::internal_error(format!("Failed to serialize error with: {}", e))
        })
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
                CommandError::new(*e, "DocumentDB Error".to_string(), msg.to_string())
            }
            DocumentDBError::RawBsonError(e, _) => {
                CommandError::internal(format!("Operation failed with raw BSON: {}", e))
            }
            DocumentDBError::PoolError(e, _) => {
                CommandError::internal(format!("Pool failed with: {}", e))
            }
            DocumentDBError::CreatePoolError(e, _) => {
                CommandError::internal(format!("Create pool failed with: {}", e))
            }
            DocumentDBError::BuildPoolError(e, _) => {
                CommandError::internal(format!("Build pool failed with: {}", e))
            }
            DocumentDBError::DocumentDBError(error_code, msg, _) => {
                CommandError::new(*error_code as i32, error_code.to_string(), msg.to_string())
            }
            DocumentDBError::UntypedDocumentDBError(error_code, msg, code, _) => {
                CommandError::new(*error_code, code.clone(), msg.to_string())
            }
            DocumentDBError::SSLErrorStack(error_stack, _) => {
                CommandError::internal(format!("SSL failed with: {}", error_stack))
            }
            DocumentDBError::SSLError(error, _) => {
                CommandError::internal(format!("SSL failed with: {}", error))
            }
            DocumentDBError::ValueAccessError(error, _) => match &error.kind {
                ValueAccessErrorKind::UnexpectedType {
                    actual, expected, ..
                } => CommandError::new(
                    ErrorCode::TypeMismatch as i32,
                    "Value Access Error".to_string(),
                    format!(
                        "Expected {:?} but got {:?}, at key {}",
                        expected,
                        actual,
                        error.key()
                    ),
                ),
                ValueAccessErrorKind::InvalidBson(_) => CommandError::new(
                    ErrorCode::BadValue as i32,
                    "Value Access Error".to_string(),
                    "Value is Invalid Bson".to_string(),
                ),
                ValueAccessErrorKind::NotPresent => CommandError::new(
                    ErrorCode::BadValue as i32,
                    "Value Access Error".to_string(),
                    "Value is not present".to_string(),
                ),
                _ => CommandError::new(
                    ErrorCode::BadValue as i32,
                    "Value Access Error".to_string(),
                    "Value Error".to_string(),
                ),
            },
        }
    }

    pub async fn from_pg_error(context: &ConnectionContext, e: &tokio_postgres::Error) -> Self {
        if let Some(state) = e.code() {
            // Check if the code has a known conversion to gateway code
            if let Some(known_error) =
                Self::known_pg_error(context, state, e.as_db_error().map_or("", |e| e.message()))
                    .await
            {
                return known_error;
            }
            CommandError::new(
                PgResponse::postgres_sqlstate_to_i32(state),
                "Error".to_string(),
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
        if let Some((known, known_msg, code)) =
            PgResponse::known_pg_error(context, state, msg).await
        {
            Some(CommandError::new(
                known,
                code.unwrap_or("Backend error".to_string()),
                known_msg.unwrap_or(msg.to_string()),
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
