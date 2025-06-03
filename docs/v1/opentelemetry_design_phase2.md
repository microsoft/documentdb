# OpenTelemetry Comprehensive Instrumentation for DocumentDB Gateway
## Phase 2 Implementation Design

## Executive Summary

Building on the foundational metrics established in Phase 1, this document outlines the comprehensive instrumentation plan for the DocumentDB gateway (`pg_documentdb_gw`) using OpenTelemetry (OTel). Phase 2 will extend the existing metrics-based telemetry to include distributed tracing and structured logging, providing end-to-end visibility into request flows and system behavior. The implementation will maintain the vendor-neutral approach using the OpenTelemetry Protocol (OTLP) for all telemetry signals.

## Background

Phase 1 successfully implemented basic metrics for monitoring cluster availability and traffic shifting during failover events. While these metrics provide valuable operational insights, they represent only one pillar of observability.

## Proposed Solution

We propose a comprehensive OpenTelemetry instrumentation that extends our foundation with:

1. **Distributed Tracing**: Trace the lifecycle of requests as they flow through the gateway and to backend clusters
2. **Context Propagation**: Link metrics, traces, and logs using consistent correlation identifiers
3. **Structured Logging**: Emit detailed, context-rich logs that align with trace data
4. **Enhanced Metrics**: Extend existing metrics with additional operational indicators
5. **Unified Export**: Send all telemetry via OTLP to support diverse backend systems

### Why OpenTelemetry for All Signals?

Using OpenTelemetry's unified approach offers several advantages:

- **Correlation**: Native support for linking metrics, traces, and logs
- **Consistency**: Similar APIs and patterns across all telemetry types
- **Vendor Independence**: Compatible with numerous backends (Jaeger, Prometheus, Elasticsearch, etc.)
- **Future-Proof**: Evolving standard with strong industry adoption
- **Performance**: Optimized for minimal overhead with sampling capabilities

### OTLP-Compatible Backends

The OpenTelemetry Protocol (OTLP) is supported by numerous observability platforms:

| Backend | Traces | Metrics | Logs | Notes |
|---------|--------|---------|------|-------|
| Jaeger | ✓ | | | De facto standard for distributed tracing |
| Prometheus | | ✓ | | Industry-standard metrics collection |
| Elastic APM | ✓ | ✓ | ✓ | Full-stack observability platform |
| New Relic | ✓ | ✓ | ✓ | Commercial observability platform |
| Datadog | ✓ | ✓ | ✓ | Commercial observability platform |
| Azure Monitor | ✓ | ✓ | ✓ | Microsoft's cloud monitoring solution |
| Google Cloud Operations | ✓ | ✓ | ✓ | Google's monitoring stack |
| AWS X-Ray | ✓ | | | AWS tracing solution (via OTel collector) |
| Grafana Tempo | ✓ | | | Grafana's tracing backend |
| OpenSearch | ✓ | | ✓ | Open-source analytics & visualization |

This vendor-neutral approach allows us to switch backends without code changes, simply by reconfiguring the OpenTelemetry collector.

## Implementation Details

### Dependencies to Add

```toml
[dependencies]
# Existing dependencies remain...

# Updated OpenTelemetry dependencies for complete instrumentation
opentelemetry = { version = "0.31", features = ["metrics", "trace"] }
opentelemetry_sdk = { version = "0.31", features = ["metrics", "trace", "rt-tokio"] }
opentelemetry-otlp = { version = "0.31", features = ["metrics", "trace"] }
tracing = "0.1"
tracing-opentelemetry = "0.21"
tracing-subscriber = { version = "0.3", features = ["env-filter", "json"] }
```

### Architecture Overview

The implementation will follow a layered approach:

1. **Core Instrumentation Layer**: OpenTelemetry tracer, meter, and logger providers
2. **Integration Layer**: Middleware, interceptors, and adapters for existing code
3. **Context Propagation Layer**: Tools for maintaining context across async boundaries
4. **Configuration Layer**: Settings for sampling, export, and backend connectivity

### Distributed Tracing

Tracing will focus on capturing the complete lifecycle of MongoDB protocol requests through the gateway, including:

- Initial request reception
- Authentication and authorization
- Request parsing and validation
- Backend selection (primary vs secondary)
- Query transformation
- Backend interaction
- Response processing
- Error handling

#### Trace Implementation

```rust
pub struct TracingProvider {
    tracer: sdktrace::Tracer,
}

impl TracingProvider {
    pub fn new() -> Self {
        // Configure OTLP exporter for traces
        let exporter = opentelemetry_otlp::new_exporter().tonic();
        
        // Create a trace provider with batch export
        let tracer_provider = sdktrace::TracerProvider::builder()
            .with_batch_exporter(exporter)
            .with_sampler(sdktrace::Sampler::AlwaysOn)
            .build();
        
        // Create a tracer
        let tracer = global::tracer("documentdb_gateway");
        
        Self { tracer }
    }
    
    pub fn start_request_span(&self, req_type: &str, header: &Header) -> Context {
        // Create span with request attributes
        let span = self.tracer.span_builder(format!("handle_{}", req_type))
            .with_kind(SpanKind::Server)
            .with_attributes([
                KeyValue::new("request.type", req_type.to_string()),
                KeyValue::new("request.id", header.request_id.to_string()),
            ])
            .start_with_context(&self.tracer, &Context::current());
        
        Context::current().with_span(span)
    }
}
```

#### Usage Example

```rust
async fn handle_message<R>(
    connection_context: &mut ConnectionContext,
    header: &Header,
    stream: &mut R,
) -> Result<()>
where
    R: AsyncRead + AsyncWrite + Unpin + Send,
{
    // Start a server span for the incoming request
    let ctx = connection_context.telemetry.start_request_span(
        header.op_code.to_string().as_str(),
        header
    );
    let _guard = ctx.span().enter();
    
    // Process the request with traced context
    ctx.span().add_event("reading_request_body", vec![]);
    
    // Processing logic with tracing annotations
    match process_request(stream, header, &ctx).await {
        Ok(_) => ctx.span().set_status(Status::Ok),
        Err(e) => ctx.span().set_status(Status::Error(e.to_string()))
    }
    
    Ok(())
}
```

### Context Propagation

To maintain consistent context across service boundaries, we'll implement a multi-layered approach inspired by [FerretDB's OpenTelemetry context propagation strategy](https://blog.ferretdb.io/otel-context-propagation-in-ferretdb/):

1. **W3C Trace Context**: Standard headers for HTTP/gRPC communications where applicable
2. **MongoDB Comment Field Propagation**: Leverage MongoDB's `comment` field to propagate trace context through the wire protocol
3. **PostgreSQL Query Comments**: Extend context propagation to the backend using SQL comments (SQLCommenter-style)
4. **Local Context**: In-process context propagation for async tasks

#### MongoDB Wire Protocol Context Propagation

Since the MongoDB wire protocol doesn't support HTTP-style trace headers, we'll follow FerretDB's approach of using the `comment` field in MongoDB operations to carry trace context.

#### Implementation Strategy

Our gateway will implement a sophisticated context propagation strategy that bridges MongoDB wire protocol limitations:

**1. Client-to-Gateway Propagation:**
- Parse MongoDB `comment` fields for embedded trace context
- Support both string and JSON comment formats for broader client compatibility
- Use a standardized JSON structure: `{"documentdb": {"traceID": "...", "spanID": "..."}}`

**2. Gateway-to-Backend Propagation:**
- Inject trace context into PostgreSQL queries using SQL comments (SQLCommenter approach)
- Maintain trace correlation across the MongoDB-to-SQL translation layer
- Enable PostgreSQL query log analysis with trace correlation

**3. Bi-directional Context Flow:**
- Extract context from incoming MongoDB requests
- Propagate context through internal gateway operations
- Inject context into outgoing PostgreSQL queries
- Correlate response processing back to original trace

#### Limitations and Considerations

Following FerretDB's analysis, our implementation acknowledges several limitations:

1. **Comment Field Support**: Not all MongoDB operations support comment fields (e.g., `insert`, `listCollections`)
2. **Driver Compatibility**: Some MongoDB drivers handle comments as strings only, requiring careful JSON encoding/decoding
3. **Protocol Extensions**: No standardized approach exists for database context propagation (W3C Trace Context Protocols Registry gap)

#### Future-Proofing

To address these limitations, our implementation will:

- **Graceful Degradation**: Continue functioning when comment-based context is unavailable
- **Multiple Propagation Methods**: Support various context injection strategies as they become available
- **Standards Compliance**: Ready to adopt future W3C standards for database context propagation
- **Extensible Design**: Architecture allows for additional propagation mechanisms

This approach ensures comprehensive trace correlation while acknowledging the practical constraints of MongoDB wire protocol context propagation.

### Logging Integration

The logging system will be integrated with tracing to provide context-aware logs that include trace identifiers and other span attributes.

```rust
pub fn configure_logging() {
    // Create a JSON formatter for structured logs
    let formatting_layer = fmt::layer()
        .json()
        .with_current_span(true);
    
    // OpenTelemetry layer to add trace context to logs
    let otel_layer = tracing_opentelemetry::layer()
        .with_tracer(opentelemetry_otlp::new_pipeline().tracing().install_batch(...));
    
    // Register all layers
    tracing_subscriber::registry()
        .with(EnvFilter::from_default_env())
        .with(formatting_layer)
        .with(otel_layer)
        .init();
}
```

#### Logging Example

```rust
async fn check_cluster_availability(&self) {
    // Create a span for the cluster check
    let span = info_span!(
        "cluster_availability_check",
        component = "monitoring"
    );
    let _guard = span.enter();

    // Check primary cluster availability
    debug!(cluster = "primary", "Checking availability");
    let primary_available = self.check_pool_availability(&self.primary_pool, "primary").await;
    
    // Log result with structured fields
    if !primary_available {
        warn!(cluster = "primary", region = self.primary_pool.region(), "Cluster unavailable");
    }
    
    self.telemetry.update_cluster_availability("primary", primary_available);
}
```

### Enhanced Metrics

We will enhance the metrics implementation by adding trace context to metrics, enabling correlation between traces and metrics. This will allow operators to relate performance metrics directly to specific request traces for improved root cause analysis.

New metrics to be added in Phase 2:

1. **Request Duration Histogram** (`docdb_gateway_request_duration`): Measures the duration of requests processed by the gateway
2. **Error Counter** (`docdb_gateway_errors_total`): Tracks errors by error type and operation
3. **Connection Pool Metrics** (`docdb_connection_pool_size`, `docdb_connection_wait_time`): Monitor connection pool health
4. **Resource Usage Metrics** (`docdb_gateway_memory_usage`, `docdb_gateway_cpu_usage`): Track resource utilization

Example of recording metrics with trace context:

```rust
// Record with current trace context to enable correlation
let ctx = Context::current();
self.request_duration.record(
    &ctx,
    duration_ms,
    &[KeyValue::new("operation", op_type.to_string())]
);
```

### Unified OpenTelemetryProvider

We will enhance the existing `OpenTelemetryProvider` to integrate all three telemetry signals. The updated provider will:

1. **Maintain Tracer Instance**: For creating and managing trace spans
2. **Hold Meter Provider**: For registering and recording metrics
3. **Context Management**: Extract and propagate context across boundaries
4. **Trace-Log Correlation**: Inject trace IDs into log records
5. **Resource Attribution**: Add consistent service and environment information

Our implementation will ensure that the `TelemetryProvider` trait methods are enhanced to handle context propagation while maintaining backward compatibility with existing code.

Example of the enhanced `emit_request_event` method:

```rust
async fn emit_request_event(&self, ctx: &ConnectionContext, header: &Header, request: Option<&Request<'_>>, ...) {
    // Extract trace context from request
    let context = extract_context_from_request(header);
    
    // Record metrics with context
    self.record_request_metrics(&context, duration_ms, ctx.current_region());
    
    // Log with trace correlation
    info!(trace_id = context.span().span_context().trace_id().to_string(),
          operation = request.map(|r| r.request_type().as_str()).unwrap_or("unknown"),
          "Request completed");
}
```

## Integration Points

The comprehensive telemetry will be integrated at these key points:

1. **Gateway Entry Points**:
   - TcpListener connection acceptance
   - SSL/TLS handshake
   - MongoDB protocol message parsing

2. **Request Processing**:
   - Authentication & authorization
   - Request parsing and validation
   - Request routing decisions

3. **Backend Interaction**:
   - Connection pool management
   - Query transformation
   - Query execution

4. **Response Handling**:
   - Response generation
   - Error processing
   - Protocol encoding

5. **Monitoring Subsystems**:
   - Cluster health checks
   - Failover detection
   - Resource usage tracking

## Configuration

Key configurable options for the OpenTelemetry implementation:

- **Service Identification**
  - `service_name`: Name of the service for resource attribution (default: "documentdb-gateway")
  - `service_version`: Version of the service (default: from package version)
  - `environment`: Deployment environment (e.g., "production", "staging")

- **Tracing Configuration**
  - `trace_sample_ratio`: Percentage of traces to sample (default: 0.1 = 10%)
  - `max_attributes_per_span`: Limit for span attributes to control cardinality

- **Export Configuration**
  - `otlp_endpoint`: OTLP endpoint for telemetry export (default: "http://localhost:4317")
  - `otlp_protocol`: Protocol to use (gRPC or HTTP)
  - `metrics_export_interval_secs`: How often to export metrics (default: 15s)

- **Performance Tuning**
  - `max_queue_size`: Size of the telemetry export buffer
  - `max_export_batch_size`: Maximum batch size for export
  - `scheduled_delay_secs`: Batching interval for telemetry export


## Example Docker Compose for OpenTelemetry Collector

```yaml

services:
  # OpenTelemetry Collector
  otel-collector:
    image: otel/opentelemetry-collector-contrib:latest
    command: ["--config=/etc/otel-collector-config.yaml"]
    volumes:
      - ./otel-collector-config.yaml:/etc/otel-collector-config.yaml
    ports:
      - "4317:4317"   # OTLP gRPC
      - "4318:4318"   # OTLP HTTP
      - "8888:8888"   # Metrics endpoint
    depends_on:
      - jaeger
      - prometheus
    networks:
      - otel-network

  # Jaeger for tracing
  jaeger:
    image: jaegertracing/all-in-one:latest
    ports:
      - "16686:16686"   # UI
      - "14250:14250"   # Model used by collector
    environment:
      - COLLECTOR_OTLP_ENABLED=true
    networks:
      - otel-network

  # Prometheus for metrics
  prometheus:
    image: prom/prometheus:latest
    command:
      - --config.file=/etc/prometheus/prometheus.yml
      - --storage.tsdb.path=/prometheus
      - --web.console.libraries=/usr/share/prometheus/console_libraries
      - --web.console.templates=/usr/share/prometheus/consoles
    volumes:
      - ./prometheus.yml:/etc/prometheus/prometheus.yml
    ports:
      - "9090:9090"
    networks:
      - otel-network
      
  # Grafana for visualization
  grafana:
    image: grafana/grafana:latest
    ports:
      - "3000:3000"
    environment:
      - GF_SECURITY_ADMIN_USER=admin
      - GF_SECURITY_ADMIN_PASSWORD=admin
    volumes:
      - ./grafana-provisioning:/etc/grafana/provisioning
    networks:
      - otel-network
    depends_on:
      - prometheus
      - jaeger

networks:
  otel-network:
    driver: bridge
```

### OpenTelemetry Collector Configuration

```yaml
# otel-collector-config.yaml
receivers:
  otlp:
    protocols:
      grpc:
        endpoint: 0.0.0.0:4317
      http:
        endpoint: 0.0.0.0:4318

processors:
  batch:
    send_batch_size: 1000
    timeout: 10s
  
  # Add resource attributes
  resource:
    attributes:
      - key: deployment.environment
        value: ${DEPLOYMENT_ENVIRONMENT}
        action: upsert

exporters:
  prometheus:
    endpoint: 0.0.0.0:8889
    namespace: documentdb_gateway
    send_timestamps: true
    metric_expiration: 60m

  jaeger:
    endpoint: jaeger:14250
    tls:
      insecure: true

  logging:
    verbosity: detailed

service:
  pipelines:
    traces:
      receivers: [otlp]
      processors: [batch, resource]
      exporters: [jaeger, logging]
    metrics:
      receivers: [otlp]
      processors: [batch, resource]
      exporters: [prometheus, logging]
    logs:
      receivers: [otlp]
      processors: [batch, resource] 
      exporters: [logging]
```

### Prometheus Configuration

```yaml
# prometheus.yml
global:
  scrape_interval: 15s
  evaluation_interval: 15s

scrape_configs:
  - job_name: 'otel-collector'
    scrape_interval: 15s
    static_configs:
      - targets: ['otel-collector:8889']
```

## Benefits of Comprehensive Instrumentation

1. **Complete Request Visibility**: Trace the entire lifecycle of requests through the system
2. **Correlation Across Signals**: Link metrics, logs, and traces for unified analysis
3. **Root Cause Analysis**: Quickly identify the source of errors or performance issues
4. **Enhanced Debugging**: Access detailed context for troubleshooting
5. **Performance Optimization**: Identify bottlenecks and opportunities for improvement
6. **Operational Awareness**: Real-time visibility into system health and behavior
7. **Capacity Planning**: Data-driven insights for resource allocation
8. **Cross-Team Collaboration**: Shared observability data for development and operations

## Implementation Plan

1. **Phase 2a**: Add distributed tracing infrastructure and basic spans
2. **Phase 2b**: Implement context propagation within the gateway
3. **Phase 2c**: Add structured logging with trace correlation
4. **Phase 2d**: Extend metrics with additional operational indicators
5. **Phase 2e**: Implement backend context propagation (MongoDB to PostgreSQL)
6. **Phase 2f**: Set up sample dashboards and alerting rules

## Conclusion

The comprehensive OpenTelemetry instrumentation plan builds on the foundation established in Phase 1, adding distributed tracing and structured logging to complete the observability triad. By exporting all telemetry via OTLP, we ensure compatibility with a wide range of backend systems while maintaining a unified approach to observability.

This implementation will provide operators and developers with unprecedented visibility into the DocumentDB gateway's behavior, enabling faster troubleshooting, more informed decision-making, and ultimately a more reliable service for end users.
