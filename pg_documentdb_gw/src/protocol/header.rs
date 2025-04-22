/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/protocol/header.rs
 *
 *-------------------------------------------------------------------------
 */

use tokio::io::{AsyncReadExt, AsyncWrite, AsyncWriteExt};
use uuid::Uuid;

use crate::{error::DocumentDBError, protocol::opcode::OpCode};

// This represents the message header (first 16 bytes of a message).
// Uniqueness is not guaranteed since request_id and/or response_to can wrap, so we
// use a UUID to uniquely identify each message and use it across the entire lifecycle of the message.
#[derive(Debug)]
pub struct Header {
    pub length: i32,
    pub request_id: i32,
    pub response_to: i32,
    pub op_code: OpCode,
    pub activity_id: String, // won't be written to the stream
}

impl Header {
    pub const LENGTH: usize = 4 * std::mem::size_of::<i32>();

    pub async fn write_to<W: AsyncWrite + Unpin>(
        &self,
        stream: &mut W,
    ) -> Result<(), DocumentDBError> {
        stream.write_all(&self.length.to_le_bytes()).await?;
        stream.write_all(&self.request_id.to_le_bytes()).await?;
        stream.write_all(&self.response_to.to_le_bytes()).await?;
        stream
            .write_all(&(self.op_code as i32).to_le_bytes())
            .await?;

        Ok(())
    }

    pub(crate) async fn read_from<R: tokio::io::AsyncRead + Unpin + Send>(
        reader: &mut R,
    ) -> Result<Self, DocumentDBError> {
        let length = reader.read_i32_le().await?;
        let request_id = reader.read_i32_le().await?;
        let response_to = reader.read_i32_le().await?;
        let op_code = OpCode::from_value(reader.read_i32_le().await?);
        Ok(Self {
            length,
            request_id,
            response_to,
            op_code,
            activity_id: Uuid::new_v4().to_string(),
        })
    }
}
