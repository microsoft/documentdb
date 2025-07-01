/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/telemetry.rs
 *
 *-------------------------------------------------------------------------
 */

use crate::context::RequestContext;
use crate::requests::Request;
use crate::responses::{CommandError, Response};
use async_trait::async_trait;
use dyn_clone::{clone_trait_object, DynClone};
use either::Either;

// TelemetryProvider takes care of emitting events and metrics
// for tracking the gateway.
#[async_trait]
pub trait TelemetryProvider: Send + Sync + DynClone {
    // Emits an event for every CRUD request dispached to backend
    async fn emit_request_event(
        &self,
        _: &mut RequestContext<'_>,
        _: Option<&Request<'_>>,
        _: Either<&Response, (&CommandError, usize)>,
        _: String,
    );
}

clone_trait_object!(TelemetryProvider);
