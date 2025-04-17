use bson::{RawDocument, RawDocumentBuf};

use crate::error::Result;

/// A response constructed by the gateway
#[derive(Debug)]
pub struct RawResponse(pub RawDocumentBuf);

impl RawResponse {
    pub fn as_raw_document(&self) -> Result<&RawDocument> {
        Ok(&self.0)
    }
}
