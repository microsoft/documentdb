use core::str;

use bson::{Bson, Document, RawDocument, RawDocumentBuf};

use documentdb_macros::documentdb_int_error_mapping;
use regex::Regex;
use tokio_postgres::{error::SqlState, Row};

use crate::{
    context::{ConnectionContext, Cursor},
    error::{DocumentDBError, ErrorCode, Result},
    postgres::PgDocument,
};

use super::{raw::RawResponse, Response};
use once_cell::sync::Lazy;

/// A server response from PG - holds ownership of the whole response from the backend
#[derive(Debug)]
pub struct PgResponse {
    rows: Vec<Row>,
}

static VECTOR_INDEX_LENGTH_CONTRAINT_REGEX: Lazy<Regex> = Lazy::new(|| {
    Regex::new(r"column cannot have more than (?<dimension>\d+) dimensions for")
        .expect("Static input")
});

static VECTOR_DIMENSIONS_EXCEEDED_REGEX: Lazy<Regex> = Lazy::new(|| {
    Regex::new(r"vector cannot have more than (?<dimension>\d+) dimensions").expect("Static input")
});

static VECTOR_DISKANN_INDEX_LENGTH_CONTRAINT_REGEX: Lazy<Regex> = Lazy::new(|| {
    Regex::new(
        r"vector dimension cannot be larger than (?<dimension>\d+) dimensions for diskann index",
    )
    .expect("Static input")
});

impl PgResponse {
    pub fn new(rows: Vec<Row>) -> Self {
        PgResponse { rows }
    }

    pub fn first(&self) -> Result<&Row> {
        self.rows
            .first()
            .ok_or(DocumentDBError::pg_response_empty())
    }

    pub fn as_raw_document(&self) -> Result<&RawDocument> {
        match self.rows.first() {
            Some(row) => {
                let content: PgDocument = row.try_get(0)?;
                Ok(content.0)
            }
            None => Err(DocumentDBError::pg_response_empty()),
        }
    }

    pub fn get_cursor(&self) -> Result<Option<(bool, Cursor)>> {
        match self.rows.first() {
            Some(row) => {
                let cols = row.columns();
                if cols.len() != 4 {
                    Ok(None)
                } else {
                    let continuation: Option<PgDocument> = row.try_get(1)?;
                    match continuation {
                        Some(continuation) => {
                            let persist: bool = row.try_get(2)?;
                            let cursor_id: i64 = row.try_get(3)?;
                            Ok(Some((
                                persist,
                                Cursor {
                                    continuation: continuation.0.to_raw_document_buf(),
                                    cursor_id,
                                },
                            )))
                        }
                        None => Ok(None),
                    }
                }
            }
            None => Err(DocumentDBError::pg_response_empty()),
        }
    }

    /// In certain cases, write errors need to be walked through to convert PG error codes to Gatway ones.
    pub async fn transform_write_errors(self, context: &ConnectionContext) -> Result<Response> {
        if let Ok(Some(_)) = self.as_raw_document()?.get("writeErrors") {
            // TODO: Conceivably faster without conversion to document
            let mut response = Document::try_from(self.as_raw_document()?)?;
            let write_errors = response.get_array_mut("writeErrors").map_err(|e| {
                DocumentDBError::internal_error(format!("PG returned invalid response: {}", e))
            })?;

            for value in write_errors {
                self.transform_error(context, value).await?;
            }
            let raw = RawDocumentBuf::from_document(&response)?;
            return Ok(Response::Raw(RawResponse(raw)));
        }
        Ok(Response::Pg(self))
    }

    async fn transform_error(&self, context: &ConnectionContext, bson: &mut Bson) -> Result<()> {
        let doc = bson
            .as_document_mut()
            .ok_or(DocumentDBError::internal_error(
                "Write error was not a document".to_string(),
            ))?;
        let msg = doc.get_str("errmsg").unwrap_or("").to_owned();
        let code = doc.get_i32_mut("code").map_err(|e| {
            DocumentDBError::internal_error(format!("PG returned invalid response: {}", e))
        })?;

        if let Some((known, opt, error_code)) =
            PgResponse::known_pg_error(context, &PgResponse::i32_to_postgres_sqlstate(code)?, &msg)
                .await
        {
            if known == ErrorCode::WriteConflict as i32
                || known == ErrorCode::InternalError as i32
                || known == ErrorCode::LockTimeout as i32
                || known == ErrorCode::Unauthorized as i32
            {
                return Err(DocumentDBError::UntypedDocumentDBError(
                    known,
                    opt.unwrap_or("".to_string()),
                    error_code.unwrap_or("DocumentDB Error".to_string()),
                    std::backtrace::Backtrace::capture(),
                ));
            }

            *code = known;
            if let Some(m) = opt {
                doc.insert("errmsg", m);
            }
        }
        Ok(())
    }

    pub fn i32_to_postgres_sqlstate(code: &i32) -> Result<SqlState> {
        let mut code = *code;
        let mut chars = [0_u8; 5];
        for char in &mut chars {
            *char = (code & 0x3F) as u8 + b'0';
            code >>= 6;
        }

        Ok(SqlState::from_code(str::from_utf8(&chars).map_err(
            |_| {
                DocumentDBError::internal_error(
                    "Failed to convert error code to state string".to_string(),
                )
            },
        )?))
    }

    pub fn postgres_sqlstate_to_i32(sql_state: &SqlState) -> i32 {
        let mut i = 0;
        let mut res = 0;
        for byte in sql_state.code().as_bytes() {
            res += (((byte - b'0') & 0x3F) as i32) << i;
            i += 6;
        }
        res
    }

    documentdb_int_error_mapping!();

    pub async fn known_pg_error(
        connection_context: &ConnectionContext,
        state: &SqlState,
        msg: &str,
    ) -> Option<(i32, Option<String>, Option<String>)> {
        if let Some(known) = PgResponse::known_error_code(state) {
            return Some((known, None, None));
        }

        let code = PgResponse::postgres_sqlstate_to_i32(state);
        if (PgResponse::API_ERROR_CODE_MIN..PgResponse::API_ERROR_CODE_MAX).contains(&code) {
            return Some((code - PgResponse::API_ERROR_CODE_MIN, None, None));
        }

        match *state {
            SqlState::UNIQUE_VIOLATION
            | SqlState::EXCLUSION_VIOLATION
                if connection_context.transaction.is_some() =>
            {
                Some((ErrorCode::WriteConflict as i32, None, None))
            }
            SqlState::UNIQUE_VIOLATION => Some((
                ErrorCode::DuplicateKey as i32,
                Some("Duplicate key violation on the requested collection".to_string()), None
            )),
            SqlState::EXCLUSION_VIOLATION => Some((
                (ErrorCode::DuplicateKey as i32),
                Some("Duplicate key violation on the requested collection".to_string()), None
            )),
            SqlState::DISK_FULL => Some((ErrorCode::OutOfDiskSpace as i32, Some("The database disk is full".to_string()), None)),
            SqlState::UNDEFINED_TABLE => Some((ErrorCode::NamespaceNotFound as i32, None, None)),
            SqlState::QUERY_CANCELED => Some((ErrorCode::ExceededTimeLimit as i32, None, None)),
            SqlState::LOCK_NOT_AVAILABLE if connection_context.transaction.is_some() => {
                Some((ErrorCode::WriteConflict as i32, None, None))
            }
            SqlState::LOCK_NOT_AVAILABLE => Some((ErrorCode::LockTimeout as i32, None, None)),
            SqlState::FEATURE_NOT_SUPPORTED => {
                Some((ErrorCode::CommandNotSupported as i32, None, None))
            }
            SqlState::DATA_EXCEPTION
                if msg.contains("dimensions, not") || msg.contains("not allowed in vector") =>
            {
                Some((ErrorCode::BadValue as i32, None, None))
            }
            SqlState::DATA_EXCEPTION => Some((
                ErrorCode::InternalError as i32,
                Some("An unexpected internal error has occurred".to_string()), None
            )),
            SqlState::PROGRAM_LIMIT_EXCEEDED =>
            {
                if msg.contains("MB, maintenance_work_mem is")
                {
                    Some((
                        ErrorCode::ExceededMemoryLimit as i32,
                        Some("index creation requires resources too large to fit in the resource memory limit, please try creating index with less number of documents or creating index before inserting documents into collection".to_string()), None
                    ))
                }
                else if let Some(captures) = VECTOR_INDEX_LENGTH_CONTRAINT_REGEX.captures(msg) {
                    let dimension = captures.name("dimension").unwrap().as_str();
                    Some((
                        ErrorCode::BadValue as i32,
                        Some(format!("field cannot have more than {} dimensions for vector index", dimension)), None
                    ))
                }
                else if let Some(captures) = VECTOR_DIMENSIONS_EXCEEDED_REGEX.captures(msg) {
                    let dimension = captures.name("dimension").unwrap().as_str();
                    Some((
                        ErrorCode::BadValue as i32,
                        Some(format!("field cannot have more than {} dimensions for vector index", dimension)), None
                    ))
                }
                else {
                    Some((ErrorCode::InternalError as i32, None, None))
                }
            },
            SqlState::NUMERIC_VALUE_OUT_OF_RANGE
                if msg
                    .contains("is out of range for type halfvec") =>
            {
                Some((
                    ErrorCode::BadValue as i32,
                    Some("Some values in the vector are out of range for half vector index".to_string()), None
                ))
            },
            SqlState::OBJECT_NOT_IN_PREREQUISITE_STATE
                if msg
                    .contains("diskann index needs to be upgraded to version") =>
            {
                Some((
                    ErrorCode::InvalidOptions as i32,
                    Some("The diskann index needs to be upgraded to the latest version, please drop and recreate the index".to_string()), None
                ))
            },
            SqlState::INTERNAL_ERROR =>
            {
                if let Some(captures) = VECTOR_DISKANN_INDEX_LENGTH_CONTRAINT_REGEX.captures(msg)
                {
                    let dimension = captures.name("dimension").unwrap().as_str();
                    Some((
                        ErrorCode::BadValue as i32,
                        Some(format!("field cannot have more than {} dimensions for diskann index", dimension)), None
                    ))
                }
                else {
                    Some((ErrorCode::InternalError as i32, None, None))
                }
            },
            SqlState::INVALID_TEXT_REPRESENTATION
            | SqlState::INVALID_PARAMETER_VALUE
            | SqlState::INVALID_ARGUMENT_FOR_NTH_VALUE => {
                Some((ErrorCode::BadValue as i32, None, None))
            },
            SqlState::READ_ONLY_SQL_TRANSACTION if connection_context.dynamic_configuration().is_replica_cluster().await => {
                Some((ErrorCode::IllegalOperation as i32, Some("Cannot execute the operation on this replica cluster".to_string()), None))
            },
            SqlState::READ_ONLY_SQL_TRANSACTION => {
                Some((ErrorCode::ExceededTimeLimit as i32, Some("Timed out while waiting for new primary to be elected".to_string()), Some("ExceededTimeLimit".to_string())))
            },
            SqlState::INSUFFICIENT_PRIVILEGE => {
                Some((ErrorCode::Unauthorized as i32, Some("User is not authorized to perform this action".to_string()), Some("Unauthorized".to_string())))
            },
            SqlState::T_R_DEADLOCK_DETECTED => {
                Some((ErrorCode::LockTimeout as i32, Some("Could not acquire lock for operation due to deadlock".to_string()), None))
            }
            _ => None,
        }
    }

    /// Corresponds to the PG ErrorCode 0000Y.
    /// Smallest value of the documentdb backend error (DocumentDBError.ok).
    pub const API_ERROR_CODE_MIN: i32 = 687865856;

    /// Corresponds to the PG ErrorCode 000PY.
    /// Largest value of the documentdb backend error.
    pub const API_ERROR_CODE_MAX: i32 = 696254464;
}
