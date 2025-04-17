use crate::context::ConnectionContext;
use crate::protocol::header::Header;
use crate::requests::compute_request_tracker::ComputeRequestTracker;
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
        _: &ConnectionContext,
        _: &Header,
        _: Option<&Request<'_>>,
        _: Either<&Response, (&CommandError, usize)>,
        _: String,
        _: &mut ComputeRequestTracker,
    );
}

clone_trait_object!(TelemetryProvider);
