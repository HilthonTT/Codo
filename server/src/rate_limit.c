#define _GNU_SOURCE

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "env.h"
#include "http_protocol.h"
#include "rate_limit.h"
#include "server.h"

// Per-client token bucket. `tokens` is refilled at `g_rps` tokens/second up to
// `g_burst`, and one token is spent per request; a request with no token left is
// rejected. Buckets live in a fixed open-addressing table keyed by the client's
// IPv4 address. The table never grows: a lookup probes a bounded window and, if
// it finds neither the client nor a free slot, evicts the least-recently-seen
// slot in that window. That keeps every operation O(PROBE_WINDOW) and the memory
// footprint constant, at the cost of being approximate under heavy IP churn --
// the right trade for a DoS guard that must stay cheap on the hot path.
typedef struct
{
  uint32_t ip;    // client IPv4 (network byte order); 0 = empty slot
  double tokens;  // tokens currently available
  double last;    // monotonic seconds at the last refill
  bool used;
} bucket_t;

#define TABLE_SIZE 4096u // power of two, so index masking is a bitwise AND
#define PROBE_WINDOW 16u

static bucket_t g_table[TABLE_SIZE];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static bool g_enabled = true;
static double g_rps = 100.0;
static double g_burst = 200.0;

void rate_limit_init(void)
{
  g_enabled = env_bool("RATE_LIMIT_ENABLED", true);
  int rps = env_int("RATE_LIMIT_RPS", 100);
  int burst = env_int("RATE_LIMIT_BURST", 200);
  g_rps = rps > 0 ? (double)rps : 100.0;
  g_burst = burst > 0 ? (double)burst : 200.0;
}

static double monotonic_seconds(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// Fowler-Noll-Vo hash of the 4 address bytes, so nearby IPs scatter across the
// table instead of clustering.
static uint32_t hash_ip(uint32_t ip)
{
  uint32_t h = 2166136261u;
  for (int i = 0; i < 4; i++)
  {
    h ^= (ip >> (i * 8)) & 0xff;
    h *= 16777619u;
  }
  return h;
}

// Find (or allocate) the bucket for `ip`. Caller holds g_lock. Never returns
// NULL: a full probe window is resolved by evicting its stalest entry.
static bucket_t *acquire_bucket(uint32_t ip, double now)
{
  uint32_t base = hash_ip(ip) & (TABLE_SIZE - 1);
  bucket_t *lru = NULL;

  for (uint32_t i = 0; i < PROBE_WINDOW; i++)
  {
    bucket_t *b = &g_table[(base + i) & (TABLE_SIZE - 1)];
    if (!b->used)
    {
      b->used = true;
      b->ip = ip;
      b->tokens = g_burst;
      b->last = now;
      return b;
    }
    if (b->ip == ip)
    {
      return b;
    }
    if (!lru || b->last < lru->last)
    {
      lru = b; // track the least-recently-seen slot as the eviction candidate
    }
  }

  // Window full and no match: repurpose the stalest slot for this client.
  lru->ip = ip;
  lru->tokens = g_burst;
  lru->last = now;
  return lru;
}

// Consume one token for `ip`. Returns true if allowed. On rejection, writes the
// seconds until a token is available to *retry_after.
static bool allow_request(uint32_t ip, int *retry_after)
{
  double now = monotonic_seconds();

  pthread_mutex_lock(&g_lock);
  bucket_t *b = acquire_bucket(ip, now);

  // Refill for the time elapsed since we last saw this client, capped at burst.
  double elapsed = now - b->last;
  if (elapsed > 0)
  {
    b->tokens += elapsed * g_rps;
    if (b->tokens > g_burst)
    {
      b->tokens = g_burst;
    }
    b->last = now;
  }

  bool allowed;
  if (b->tokens >= 1.0)
  {
    b->tokens -= 1.0;
    allowed = true;
  }
  else
  {
    allowed = false;
    // Seconds until the bucket refills one token, rounded up (no libm ceil).
    double secs = (1.0 - b->tokens) / g_rps;
    int wait = (int)secs;
    if (secs > (double)wait)
    {
      wait++;
    }
    *retry_after = wait > 0 ? wait : 1;
  }
  pthread_mutex_unlock(&g_lock);
  return allowed;
}

static int send_429(connection_t *conn, http_request_t *request,
                    http_response_t *response, int retry_after)
{
  if (response->body)
  {
    free(response->body);
    response->body = NULL;
  }

  const char *msg = "{\"error\":\"rate limit exceeded\"}";
  response->status = HTTP_TOO_MANY_REQUESTS;
  snprintf(response->version, sizeof(response->version), "HTTP/1.1");
  response->body = strdup(msg);
  response->body_length = response->body ? strlen(response->body) : 0;
  response->keep_alive = request->keep_alive;

  int h = 0;
  snprintf(response->headers[h].name, sizeof(response->headers[h].name), "Content-Type");
  snprintf(response->headers[h].value, sizeof(response->headers[h].value), "application/json");
  h++;
  snprintf(response->headers[h].name, sizeof(response->headers[h].name), "Retry-After");
  snprintf(response->headers[h].value, sizeof(response->headers[h].value), "%d", retry_after);
  h++;
  response->header_count = h;

  return send_http_response(conn, response);
}

int rate_limit_middleware(connection_t *conn, http_request_t *request,
                          http_response_t *response, middleware_ctx_t *next)
{
  if (!g_enabled)
  {
    return middleware_next(conn, request, response, next);
  }

  uint32_t ip = conn->client_addr.sin_addr.s_addr;
  int retry_after = 1;
  if (!allow_request(ip, &retry_after))
  {
    return send_429(conn, request, response, retry_after);
  }

  return middleware_next(conn, request, response, next);
}
