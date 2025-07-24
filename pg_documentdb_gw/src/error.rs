/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/error.rs
 *
 *-------------------------------------------------------------------------
 */

use std::{backtrace::Backtrace, fmt::Display, io};

use bson::raw::ValueAccessError;
use deadpool_postgres::{BuildError, CreatePoolError, PoolError};
use openssl::error::ErrorStack;

use documentdb_macros::documentdb_error_code_enum;

use crate::responses::constant::pg_returned_invalid_response_message;

documentdb_error_code_enum!();

#[derive(Debug)]
pub enum DocumentDBError {
    IoError(io::Error, Backtrace),
    DocumentDBError(ErrorCode, String, Backtrace),
    UntypedDocumentDBError(i32, String, String, Backtrace),
    PostgresError(tokio_postgres::Error, Backtrace),
    PostgresDocumentDBError(i32, String, Backtrace),
    PoolError(PoolError, Backtrace),
    CreatePoolError(CreatePoolError, Backtrace),
    BuildPoolError(BuildError, Backtrace),
    RawBsonError(bson::raw::Error, Backtrace),
    SSLError(openssl::ssl::Error, Backtrace),
    SSLErrorStack(ErrorStack, Backtrace),
    ValueAccessError(ValueAccessError, Backtrace),
}

impl DocumentDBError {
    pub fn parse_failure<'a, E: std::fmt::Display>() -> impl Fn(E) -> Self + 'a {
        move |e| DocumentDBError::bad_value(format!("Failed to parse: {}", e))
    }

    pub fn pg_response_empty() -> Self {
        DocumentDBError::internal_error("PG returned no rows in response".to_string())
    }

    pub fn pg_response_invalid(e: ValueAccessError) -> Self {
        DocumentDBError::internal_error(pg_returned_invalid_response_message(e))
    }

    pub fn sasl_payload_invalid() -> Self {
        DocumentDBError::unauthorized("Sasl payload invalid.".to_string())
    }

    pub fn unauthorized(msg: String) -> Self {
        DocumentDBError::DocumentDBError(ErrorCode::Unauthorized, msg, Backtrace::capture())
    }

    pub fn bad_value(msg: String) -> Self {
        DocumentDBError::DocumentDBError(ErrorCode::BadValue, msg, Backtrace::capture())
    }

    pub fn internal_error(msg: String) -> Self {
        DocumentDBError::DocumentDBError(ErrorCode::InternalError, msg, Backtrace::capture())
    }

    pub fn type_mismatch(msg: String) -> Self {
        DocumentDBError::DocumentDBError(ErrorCode::TypeMismatch, msg, Backtrace::capture())
    }

    #[allow(clippy::self_named_constructors)]
    pub fn documentdb_error(e: ErrorCode, msg: String) -> Self {
        DocumentDBError::DocumentDBError(e, msg, Backtrace::capture())
    }

    pub fn error_code_enum(&self) -> Option<ErrorCode> {
        match self {
            DocumentDBError::DocumentDBError(code, _, _) => Some(*code),
            DocumentDBError::UntypedDocumentDBError(code, _, _, _) => ErrorCode::from_i32(*code),
            _ => None,
        }
    }
}

impl Display for DocumentDBError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            DocumentDBError::PostgresError(e, _) => {
                if let Some(dbe) = e.as_db_error() {
                    write!(f, "PostgresError({:?}, {:?})", dbe.code(), dbe.hint())
                } else {
                    write!(f, "{:?}", e)
                }
            }
            _ => write!(f, "{:?}", self),
        }
    }
}

/// The result type for all methods that can return an error
pub type Result<T> = std::result::Result<T, DocumentDBError>;

impl From<io::Error> for DocumentDBError {
    fn from(error: io::Error) -> Self {
        DocumentDBError::IoError(error, Backtrace::capture())
    }
}

impl From<tokio_postgres::Error> for DocumentDBError {
    fn from(error: tokio_postgres::Error) -> Self {
        DocumentDBError::PostgresError(error, Backtrace::capture())
    }
}

impl From<bson::raw::Error> for DocumentDBError {
    fn from(error: bson::raw::Error) -> Self {
        DocumentDBError::RawBsonError(error, Backtrace::capture())
    }
}

impl From<PoolError> for DocumentDBError {
    fn from(error: PoolError) -> Self {
        DocumentDBError::PoolError(error, Backtrace::capture())
    }
}

impl From<CreatePoolError> for DocumentDBError {
    fn from(error: CreatePoolError) -> Self {
        DocumentDBError::CreatePoolError(error, Backtrace::capture())
    }
}

impl From<BuildError> for DocumentDBError {
    fn from(error: BuildError) -> Self {
        DocumentDBError::BuildPoolError(error, Backtrace::capture())
    }
}

impl From<ErrorStack> for DocumentDBError {
    fn from(error: ErrorStack) -> Self {
        DocumentDBError::SSLErrorStack(error, Backtrace::capture())
    }
}

impl From<openssl::ssl::Error> for DocumentDBError {
    fn from(error: openssl::ssl::Error) -> Self {
        DocumentDBError::SSLError(error, Backtrace::capture())
    }
}

impl From<ValueAccessError> for DocumentDBError {
    fn from(error: ValueAccessError) -> Self {
        DocumentDBError::ValueAccessError(error, Backtrace::capture())
    }
}

impl Display for ErrorCode {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{:?}", self)
    }
}
