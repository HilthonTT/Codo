#ifndef METRICS_H
#define METRICS_H

#include <stddef.h>

#include "http_types.h"

// Prometheus-style request metrics, updated concurrently from every worker and
// storage-pool thread, so the counters are atomics rather than mutex-guarded --
// same rationale as the network stats in stats.c. Metrics are process-wide.

// Record the server's start time so codo_uptime_seconds can be reported. Call
// once at startup; safe to call again (it just re-stamps the epoch).
void metrics_init(void);

// Record one completed request: `method` and the final response `status`, plus
// the wall-clock `duration_seconds` spent in the handler chain. Bumps the
// request counter (by method + status class) and the latency histogram.
void metrics_record_request(http_method_t method, int status, double duration_seconds);

// Serialize every metric in the Prometheus text exposition format into buf.
// Returns the number of bytes written (excluding the NUL), or -1 on overflow.
int metrics_format_prometheus(char *buf, size_t size);

#endif
