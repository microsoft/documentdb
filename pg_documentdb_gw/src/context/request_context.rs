/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/context/request_context.rs
 *
 *-------------------------------------------------------------------------
 */

use protocol::header::Header;
use uuid::Uuid;

use crate::{context::ConnectionContext, protocol, requests::RequestInfo};

pub struct RequestContext<'a> {
    pub activity_id: String,
    pub connection_context: &'a mut ConnectionContext,
    pub header: &'a Header,
    pub request_info: &'a mut RequestInfo<'a>,
}

impl<'a> RequestContext<'a> {
    pub fn new(
        activity_id: Option<String>,
        header: &'a Header,
        connection_context: &'a mut ConnectionContext,
        request_info: &'a mut RequestInfo<'a>,
    ) -> Self {
        let activity_id = activity_id.unwrap_or_else(|| Uuid::new_v4().to_string());
        Self {
            activity_id,
            connection_context,
            header,
            request_info,
        }
    }

    pub fn generate_activity_id() -> String {
        Uuid::new_v4().to_string()
    }
}
