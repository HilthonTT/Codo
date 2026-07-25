#define _GNU_SOURCE

#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "http_types.h"
#include "metrics.h"
#include "util.h"

// Latency histogram bucket upper bounds, in seconds (the classic Prometheus
// default ladder). A request falls in the first bucket whose bound it does not
// exceed; the implicit +Inf bucket is the total count. Kept as a plain array so
// the format code can pair each bound with its atomic counter.
static const double BUCKET_BOUNDS[] = {
    0.0005, 0.001, 0.0025, 0.005, 0.01, 0.025, 0.05,
    0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0};
#define NUM_BUCKETS (int)(sizeof(BUCKET_BOUNDS) / sizeof(BUCKET_BOUNDS[0]))

// requests[method][class] where class is status/100 (1..5), index 0 unused.
// HTTP_UNKNOWN + 1 covers every value the method enum can take.
#define NUM_METHODS (HTTP_UNKNOWN + 1)
#define NUM_CLASSES 6

static _Atomic(uint64_t) g_requests[NUM_METHODS][NUM_CLASSES];
static _Atomic(uint64_t) g_bucket[NUM_BUCKETS];
static _Atomic(uint64_t) g_request_count;
// Latency sum accumulated in microseconds (an integer) so the hot path uses an
// integer atomic add; it is divided back to seconds only when formatting.
static _Atomic(uint64_t) g_duration_us;
static time_t g_start_time;

void metrics_init(void)
{
  g_start_time = time(NULL);
}

void metrics_record_request(http_method_t method, int status, double duration_seconds)
{
  int m = (method >= 0 && method < NUM_METHODS) ? (int)method : HTTP_UNKNOWN;
  int cls = status / 100;
  if (cls < 0 || cls >= NUM_CLASSES)
  {
    cls = 0; // unexpected status code -> the "0xx" bucket rather than a stray write
  }
  atomic_fetch_add(&g_requests[m][cls], 1);

  if (duration_seconds < 0.0)
  {
    duration_seconds = 0.0;
  }
  for (int i = 0; i < NUM_BUCKETS; i++)
  {
    if (duration_seconds <= BUCKET_BOUNDS[i])
    {
      atomic_fetch_add(&g_bucket[i], 1);
      break;
    }
  }
  atomic_fetch_add(&g_request_count, 1);
  atomic_fetch_add(&g_duration_us, (uint64_t)(duration_seconds * 1e6));
}

// Append with bounds checking; advances *off and returns -1 on overflow.
static int append(char *buf, size_t size, size_t *off, const char *fmt, ...)
{
  if (*off >= size)
  {
    return -1;
  }
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf + *off, size - *off, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= size - *off)
  {
    return -1;
  }
  *off += (size_t)n;
  return 0;
}

int metrics_format_prometheus(char *buf, size_t size)
{
  size_t off = 0;

  if (append(buf, size, &off,
             "# HELP codo_requests_total Total HTTP requests by method and status class.\n"
             "# TYPE codo_requests_total counter\n") != 0)
  {
    return -1;
  }
  static const char *class_label[NUM_CLASSES] = {"0xx", "1xx", "2xx", "3xx", "4xx", "5xx"};
  for (int m = 0; m < NUM_METHODS; m++)
  {
    for (int c = 0; c < NUM_CLASSES; c++)
    {
      uint64_t v = atomic_load(&g_requests[m][c]);
      if (v == 0)
      {
        continue; // skip empty series to keep the payload compact
      }
      if (append(buf, size, &off,
                 "codo_requests_total{method=\"%s\",status=\"%s\"} %llu\n",
                 http_method_to_string((http_method_t)m), class_label[c],
                 (unsigned long long)v) != 0)
      {
        return -1;
      }
    }
  }

  if (append(buf, size, &off,
             "# HELP codo_request_duration_seconds Handler chain latency.\n"
             "# TYPE codo_request_duration_seconds histogram\n") != 0)
  {
    return -1;
  }
  // Buckets are cumulative in the exposition format: each le="x" reports the
  // count of observations <= x, so we carry a running total.
  uint64_t cumulative = 0;
  for (int i = 0; i < NUM_BUCKETS; i++)
  {
    cumulative += atomic_load(&g_bucket[i]);
    if (append(buf, size, &off,
               "codo_request_duration_seconds_bucket{le=\"%g\"} %llu\n",
               BUCKET_BOUNDS[i], (unsigned long long)cumulative) != 0)
    {
      return -1;
    }
  }
  uint64_t total = atomic_load(&g_request_count);
  double sum = (double)atomic_load(&g_duration_us) / 1e6;
  if (append(buf, size, &off,
             "codo_request_duration_seconds_bucket{le=\"+Inf\"} %llu\n"
             "codo_request_duration_seconds_sum %.6f\n"
             "codo_request_duration_seconds_count %llu\n",
             (unsigned long long)total, sum, (unsigned long long)total) != 0)
  {
    return -1;
  }

  time_t now = time(NULL);
  long uptime = g_start_time ? (long)(now - g_start_time) : 0;
  if (append(buf, size, &off,
             "# HELP codo_uptime_seconds Seconds since the server started.\n"
             "# TYPE codo_uptime_seconds gauge\n"
             "codo_uptime_seconds %ld\n",
             uptime) != 0)
  {
    return -1;
  }

  return (int)off;
}
