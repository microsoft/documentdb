use std::{backtrace::Backtrace, fmt::Display, io};

use bson::raw::ValueAccessError;
use deadpool_postgres::{BuildError, CreatePoolError, PoolError};
use num_traits::FromPrimitive;
use openssl::error::ErrorStack;

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
        DocumentDBError::internal_error(format!("PG returned invalid response: {}", e))
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
            DocumentDBError::UntypedDocumentDBError(code, _, _, _) => {
                ErrorCode::from_i64(*code as i64)
            }
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

#[derive(Debug, Clone, Copy)]
pub enum ErrorCode {
    Ok = 0,
    InternalError = 1,
    BadValue = 2,
    Unauthorized = 13,
    TypeMismatch = 14,
    AuthenticationFailed = 18,
    IllegalOperation = 20,
    LockTimeout = 24,
    NamespaceNotFound = 26,
    CursorNotFound = 43,
    ExceededTimeLimit = 50,
    InvalidOptions = 72,
    InvalidNamespace = 73,
    ShutdownInProgress = 91,
    WriteConflict = 112,
    CommandNotSupported = 115,
    ConflictingOperationInProgress = 117,
    ExceededMemoryLimit = 146,
    ClientMetadataCannotBeMutated = 186,
    TransactionTooOld = 225,
    NoSuchTransaction = 251,
    TransactionCommitted = 256,
    OperationNotSupportedInTransaction = 263,
    DuplicateKey = 11000,
    OutOfDiskSpace = 14031,
    UnknownBsonField = 40415,
}

impl FromPrimitive for ErrorCode {
    fn from_i64(n: i64) -> Option<Self> {
        match n {
            0 => Some(ErrorCode::Ok),
            1 => Some(ErrorCode::InternalError),
            2 => Some(ErrorCode::BadValue),
            13 => Some(ErrorCode::Unauthorized),
            14 => Some(ErrorCode::TypeMismatch),
            18 => Some(ErrorCode::AuthenticationFailed),
            20 => Some(ErrorCode::IllegalOperation),
            24 => Some(ErrorCode::LockTimeout),
            26 => Some(ErrorCode::NamespaceNotFound),
            43 => Some(ErrorCode::CursorNotFound),
            50 => Some(ErrorCode::ExceededTimeLimit),
            72 => Some(ErrorCode::InvalidOptions),
            73 => Some(ErrorCode::InvalidNamespace),
            91 => Some(ErrorCode::ShutdownInProgress),
            112 => Some(ErrorCode::WriteConflict),
            115 => Some(ErrorCode::CommandNotSupported),
            117 => Some(ErrorCode::ConflictingOperationInProgress),
            146 => Some(ErrorCode::ExceededMemoryLimit),
            186 => Some(ErrorCode::ClientMetadataCannotBeMutated),
            225 => Some(ErrorCode::TransactionTooOld),
            251 => Some(ErrorCode::NoSuchTransaction),
            256 => Some(ErrorCode::TransactionCommitted),
            263 => Some(ErrorCode::OperationNotSupportedInTransaction),
            11000 => Some(ErrorCode::DuplicateKey),
            14031 => Some(ErrorCode::OutOfDiskSpace),
            40415 => Some(ErrorCode::UnknownBsonField),
            _ => None,
        }
    }

    fn from_u64(n: u64) -> Option<Self> {
        Self::from_i64(n as i64)
    }
}

impl Display for ErrorCode {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{:?}", self)
    }
}
