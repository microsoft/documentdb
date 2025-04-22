/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/requests/compute_request_tracker.rs
 *
 *-------------------------------------------------------------------------
 */

use std::time::Instant;

#[derive(Debug)]
pub enum ComputeRequestInterval {
    BufferRead,
    InteropAndSdk,
    TransportAndBackend,
    HandleResponse,
    RequestDuration,
    FormatRequest,
    MaxUnused,
}

#[derive(Debug, Default)]
pub struct ComputeRequestTracker {
    pub request_interval_metrics_array: [i64; ComputeRequestInterval::MaxUnused as usize],
}

impl ComputeRequestTracker {
    pub fn new() -> Self {
        ComputeRequestTracker {
            request_interval_metrics_array: [0; ComputeRequestInterval::MaxUnused as usize],
        }
    }

    pub fn start_timer(&mut self) -> Instant {
        Instant::now()
    }

    pub fn record_duration(&mut self, interval: ComputeRequestInterval, start_time: Instant) {
        let elapsed = start_time.elapsed();
        self.request_interval_metrics_array[interval as usize] += elapsed.as_nanos() as i64;
    }

    pub fn get_interval_elapsed_time(&mut self, interval: ComputeRequestInterval) -> i64 {
        self.request_interval_metrics_array[interval as usize]
    }
}
