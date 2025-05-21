/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/open_telemetry_provider.rs
 *
 *-------------------------------------------------------------------------
 */

use crate::context::ConnectionContext;
use crate::protocol::header::Header;
use crate::requests::compute_request_tracker::ComputeRequestTracker;
use crate::requests::Request;
use crate::responses::{CommandError, Response};
use crate::telemetry::TelemetryProvider;
use async_trait::async_trait;
use either::Either;
use std::env;
use std::sync::Arc;
use std::time::Duration;
use once_cell::sync::OnceCell;

// OpenTelemetry imports
use opentelemetry::{KeyValue, metrics::MeterProvider};
use opentelemetry_otlp::{WithExportConfig};

// Global meter provider for OpenTelemetry
static METER_PROVIDER: OnceCell<Arc<opentelemetry_sdk::metrics::SdkMeterProvider>> = OnceCell::new();

#[derive(Clone)]
pub struct OpenTelemetryProvider {
    traffic_counter: opentelemetry::metrics::Counter<u64>,
    availability_gauge: opentelemetry::metrics::Gauge<i64>,
}

impl OpenTelemetryProvider {
    pub fn new() -> Self {
        // Initialize the global meter provider if not already done
        let meter_provider = METER_PROVIDER.get_or_init(|| {
            let endpoint = env::var("OTEL_EXPORTER_OTLP_ENDPOINT")
                .unwrap_or_else(|_| "http://otel-collector:4317".to_string());
            
            let exporter = opentelemetry_otlp::MetricExporter::builder()
                .with_tonic()
                .with_endpoint(endpoint)
                .with_timeout(Duration::from_secs(5))
                .build()
                .expect("Failed to build OTLP exporter");
            
            let provider = opentelemetry_sdk::metrics::SdkMeterProvider::builder()
                .with_periodic_exporter(exporter)
                .build();

            Arc::new(provider)
        });

        let meter = meter_provider.meter("documentdb_gateway");

        let traffic_counter = meter
            .u64_counter("docdb_gateway_request_routing")
            .with_description("Counts requests routed to primary or secondary regions")
            .build();

        let availability_gauge = meter
            .i64_gauge("docdb_cluster_availability")
            .with_description("Reports perceived availability of database clusters")
            .build();
            
        OpenTelemetryProvider {
            traffic_counter,
            availability_gauge,
        }
    }
}

#[async_trait]
impl TelemetryProvider for OpenTelemetryProvider {
    async fn emit_request_event(
        &self,
        _context: &ConnectionContext,
        _header: &Header,
        _request: Option<&Request<'_>>,
        response: Either<&Response, (&CommandError, usize)>,
        target_region: String,
        _request_tracker: &mut ComputeRequestTracker,
    ) {
        // Determine if the request succeeded
        let status = match &response {
            Either::Left(_) => "success",
            Either::Right(_) => "failure",
        };
        
        // Track this request being routed to the target region
        self.track_request_routing(&target_region, status);
        
        // Additional metrics could be added here in future phases
    }
    
    fn update_cluster_availability(&self, cluster_id: &str, is_available: bool) {
        let value = if is_available { 1 } else { 0 };
        self.availability_gauge.record(
            value, 
            &[KeyValue::new("cluster_id", cluster_id.to_string())]
        );
    }
    
    fn track_request_routing(&self, target_region: &str, status: &str) {
        // Increment the traffic counter for routing decisions
        self.traffic_counter.add(
            1,
            &[
                KeyValue::new("target_region", target_region.to_string()),
                KeyValue::new("status", status.to_string())
            ]
        );
    }
}
