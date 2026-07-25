#ifndef RATE_LIMIT_H
#define RATE_LIMIT_H

#include "connection.h"
#include "http_types.h"
#include "middleware.h"

// Read the rate-limit configuration from the environment and size the bucket
// table. Call once at startup, before the middleware runs. Keys:
//   RATE_LIMIT_ENABLED  on/off        (default true)
//   RATE_LIMIT_RPS      refill rate   (tokens added per second, default 100)
//   RATE_LIMIT_BURST    bucket size   (max tokens / burst allowance, default 200)
// With ENABLED=false the middleware is a pass-through.
void rate_limit_init(void);

// Middleware: token-bucket rate limiting keyed by client IP. When a client has
// no tokens left it short-circuits the chain with 429 Too Many Requests and a
// Retry-After header; otherwise it spends a token and passes the request on.
int rate_limit_middleware(connection_t *conn, http_request_t *request,
                          http_response_t *response, middleware_ctx_t *next);

#endif
