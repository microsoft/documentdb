use crate::error::{DocumentDBError, Result};

pub mod header;
pub mod message;
pub mod opcode;
pub mod reader;
pub mod util;

pub const MAX_BSON_OBJECT_SIZE: i32 = 16 * 1024 * 1024;
pub const MAX_MESSAGE_SIZE_BYTES: i32 = 48000000;
pub const LOGICAL_SESSION_TIMEOUT_MINUTES: u8 = 30;

pub const OK_SUCCEEDED: f64 = 1.0;
pub const OK_FAILED: f64 = 0.0;

pub fn extract_namespace(ns: &str) -> Result<(&str, &str)> {
    let pos = ns.find('.').ok_or(DocumentDBError::bad_value(
        "Source namespace not valid".to_string(),
    ))?;
    let db = &ns[0..pos];
    let coll = &ns[pos + 1..];
    Ok((db, coll))
}
