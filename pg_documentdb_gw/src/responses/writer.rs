use crate::{
    context::ConnectionContext,
    error::DocumentDBError,
    protocol::{header::Header, opcode::OpCode},
};
use bson::{to_raw_document_buf, RawDocument};
use tokio::{
    io::{AsyncRead, AsyncWrite, AsyncWriteExt},
    net::TcpStream,
};
use tokio_openssl::SslStream;
use uuid::Uuid;

use super::{CommandError, Response};

/// Write a server response to the client stream
pub async fn write<R>(
    header: &Header,
    response: &Response,
    stream: &mut R,
) -> Result<(), DocumentDBError>
where
    R: AsyncRead + AsyncWrite + Unpin + Send,
{
    write_response(header, response.as_raw_document()?, stream).await
}

/// Write a raw BSON object to the client stream
pub async fn write_response<R>(
    header: &Header,
    response: &RawDocument,
    stream: &mut R,
) -> Result<(), DocumentDBError>
where
    R: AsyncRead + AsyncWrite + Unpin + Send,
{
    // The format of the response will depend on the OP which the client sent
    match header.op_code {
        OpCode::Command => unimplemented!(),

        // Messages are always responded to with messages
        OpCode::Msg => write_message(header, response, stream).await,

        // Query is responded to with Reply
        OpCode::Query => {
            // Write the header
            let header = Header {
                // Total size of the response is the bytes + standard header + reply header
                length: (response.as_bytes().len() + Header::LENGTH + 20) as i32,
                request_id: header.request_id,
                response_to: header.request_id,
                op_code: OpCode::Reply,
                activity_id: header.activity_id.clone(),
            };
            header.write_to(stream).await?;

            stream.write_i32_le(0).await?; // Response flags
            stream.write_i64_le(0).await?; // Cursor Id
            stream.write_i32_le(0).await?; // startingFrom
            stream.write_i32_le(1).await?; // numberReturned

            stream.write_all(response.as_bytes()).await?;
            Ok(())
        }

        // Insert has no response
        OpCode::Insert => Ok(()),
        _ => Err(DocumentDBError::internal_error(format!(
            "Unexpected response opcode: {:?}",
            header.op_code
        ))),
    }?;
    stream.flush().await?;
    Ok(())
}

/// Serializes the Message to bytes and writes them to `writer`.
pub async fn write_message<R>(
    header: &Header,
    response: &RawDocument,
    writer: &mut R,
) -> Result<(), DocumentDBError>
where
    R: AsyncRead + AsyncWrite + Unpin + Send,
{
    let total_length = Header::LENGTH
        + std::mem::size_of::<u32>()
        + std::mem::size_of::<u8>()
        + response.as_bytes().len(); // Message payload + status code

    let header = Header {
        length: total_length as i32,
        request_id: header.request_id,
        response_to: header.request_id,
        op_code: OpCode::Msg,
        activity_id: header.activity_id.clone(),
    };
    header.write_to(writer).await?;

    // Write Flags
    writer.write_u32_le(0).await?;

    // Write payload type + section
    writer.write_u8(0).await?;

    writer.write_all(response.as_bytes()).await?;

    Ok(())
}

pub async fn write_error(
    connection_context: &ConnectionContext,
    header: &Header,
    err: DocumentDBError,
    stream: &mut SslStream<TcpStream>,
) -> Result<(), DocumentDBError> {
    let response = to_raw_document_buf(&CommandError::from_error(connection_context, &err).await)
        .map_err(|e| {
        DocumentDBError::internal_error(format!("Failed to serialize error with: {}", e))
    })?;
    write_response(header, &response, stream).await?;
    stream.flush().await?;
    Ok(())
}

pub async fn write_error_without_header<R>(
    connection_context: &ConnectionContext,
    err: DocumentDBError,
    stream: &mut R,
) -> Result<(), DocumentDBError>
where
    R: AsyncRead + AsyncWrite + Unpin + Send,
{
    let response = to_raw_document_buf(&CommandError::from_error(connection_context, &err).await)
        .map_err(|e| {
        DocumentDBError::internal_error(format!("Failed to serialize error with: {}", e))
    })?;

    let header = Header {
        length: (response.as_bytes().len() + 1) as i32,
        request_id: 0,
        response_to: 0,
        op_code: OpCode::Msg,
        activity_id: Uuid::default().to_string(),
    };
    write_response(&header, &response, stream).await?;
    stream.flush().await?;
    Ok(())
}
