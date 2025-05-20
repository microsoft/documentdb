/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/telemetry.rs
 *
 *-------------------------------------------------------------------------
 */

use crate::context::ConnectionContext;
use crate::protocol::header::Header;
use crate::requests::compute_request_tracker::ComputeRequestTracker;
use crate::requests::Request;
use crate::responses::{CommandError, Response};
use async_trait::async_trait;
use dyn_clone::{clone_trait_object, DynClone};
use either::Either;

// Re-export the OpenTelemetry provider
pub use crate::open_telemetry_provider::OpenTelemetryProvider;

// TelemetryProvider takes care of emitting events and metrics
// for tracking the gateway.
#[async_trait]
pub trait TelemetryProvider: Send + Sync + DynClone {
    // Emits an event for every CRUD request dispached to backend
    async fn emit_request_event(
        &self,
        _: &ConnectionContext,
        _: &Header,
        _: Option<&Request<'_>>,
        _: Either<&Response, (&CommandError, usize)>,
        _: String,
        _: &mut ComputeRequestTracker,
    );

    // Check and emit the perceived availability of a cluster
    fn update_cluster_availability(&self, cluster_id: &str, is_available: bool);
    
    // Track a request being routed to a specific region
    fn track_request_routing(&self, target_region: &str, status: &str);
}

clone_trait_object!(TelemetryProvider);
