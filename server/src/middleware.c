#define _GNU_SOURCE

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "http_protocol.h"
#include "metrics.h"
#include "middleware.h"
#include "server.h"
#include "util.h"

// Look up a request header value by name (case-insensitive). Returns NULL when
// the header is absent.
static const char *request_header(const http_request_t *request, const char *name)
{
  for (int i = 0; i < request->header_count; i++)
  {
    if (strcasecmp(request->headers[i].name, name) == 0)
    {
      return request->headers[i].value;
    }
  }
  return NULL;
}

int add_middleware(http_server_t *server, middleware_fn_t fn)
{
  if (!server || !fn)
  {
    return -1;
  }
  if (server->middleware_count >= MAX_MIDDLEWARES)
  {
    return -1;
  }
  server->middlewares[server->middleware_count++] = fn;
  return 0;
}

int middleware_next(connection_t *conn, http_request_t *request,
                    http_response_t *response, middleware_ctx_t *ctx)
{
  // More middleware to run: invoke the next one, handing it a cursor advanced
  // by one so its middleware_next() call reaches the following stage. The copy
  // lives on this stack frame, which outlives the nested call.
  if (ctx->index < ctx->count)
  {
    middleware_fn_t fn = ctx->chain[ctx->index];
    middleware_ctx_t next = {ctx->chain, ctx->count, ctx->index + 1, ctx->handler};
    return fn(conn, request, response, &next);
  }

  // Chain exhausted: run the terminal route handler.
  return ctx->handler ? ctx->handler(conn, request, response) : 0;
}

int run_with_middleware(connection_t *conn, http_request_t *request,
                        http_response_t *response, route_handler_t handler)
{
  middleware_ctx_t ctx = {
      g_server.middlewares, g_server.middleware_count, 0, handler};
  return middleware_next(conn, request, response, &ctx);
}

int logging_middleware(connection_t *conn, http_request_t *request,
                       http_response_t *response, middleware_ctx_t *next)
{
  struct timespec start;
  clock_gettime(CLOCK_MONOTONIC, &start);

  // Run the rest of the pipeline (any inner middleware + the handler). The
  // response is fully populated once this returns.
  int rc = middleware_next(conn, request, response, next);

  struct timespec end;
  clock_gettime(CLOCK_MONOTONIC, &end);
  double elapsed_ms = (end.tv_sec - start.tv_sec) * 1e3 +
                      (end.tv_nsec - start.tv_nsec) / 1e6;

  char client_ip[INET_ADDRSTRLEN];
  if (!inet_ntop(AF_INET, &conn->client_addr.sin_addr, client_ip, sizeof(client_ip)))
  {
    strcpy(client_ip, "-");
  }

  // Sanitize control bytes in the URI before logging so a crafted request line
  // cannot inject terminal escapes or forge log lines. The URI has no spaces or
  // newlines (parsed with %2047s) but may carry other control characters.
  char safe_uri[sizeof(request->uri)];
  size_t ui = 0;
  for (const char *u = request->uri; *u && ui + 1 < sizeof(safe_uri); u++)
  {
    unsigned char c = (unsigned char)*u;
    safe_uri[ui++] = (c < 0x20 || c == 0x7f) ? '?' : (char)c;
  }
  safe_uri[ui] = '\0';

  printf("%s %s %s -> %d (%.2f ms)\n",
         client_ip,
         http_method_to_string(request->method),
         safe_uri,
         response->status,
         elapsed_ms);
  fflush(stdout);

  return rc;
}

int metrics_middleware(connection_t *conn, http_request_t *request,
                       http_response_t *response, middleware_ctx_t *next)
{
  struct timespec start;
  clock_gettime(CLOCK_MONOTONIC, &start);

  int rc = middleware_next(conn, request, response, next);

  struct timespec end;
  clock_gettime(CLOCK_MONOTONIC, &end);
  double elapsed = (end.tv_sec - start.tv_sec) +
                   (end.tv_nsec - start.tv_nsec) / 1e9;

  // response->status is fully resolved once the chain returns -- including any
  // short-circuit reply (a 429 from rate limiting, a 401 from auth), so those
  // are counted too.
  metrics_record_request(request->method, response->status, elapsed);
  return rc;
}

int cors_middleware(connection_t *conn, http_request_t *request,
                    http_response_t *response, middleware_ctx_t *next)
{
  const char *allow =
      g_server.cors_allow_origin[0] ? g_server.cors_allow_origin : "*";
  const char *origin = request_header(request, "Origin");

  // Preflight: the browser sends OPTIONS ahead of a non-simple cross-origin
  // request. Answer it right here (never reaching the route/file handler) with
  // the methods and headers we permit, then a 204. Without this, OPTIONS would
  // fall through to the default file handler and be rejected as 405.
  if (request->method == HTTP_OPTIONS)
  {
    if (response->body)
    {
      free(response->body);
      response->body = NULL;
    }
    response->status = HTTP_NO_CONTENT;
    snprintf(response->version, sizeof(response->version), "HTTP/1.1");
    response->body_length = 0;
    response->keep_alive = request->keep_alive;

    int h = 0;
    snprintf(response->headers[h].name, sizeof(response->headers[h].name),
             "Access-Control-Allow-Methods");
    snprintf(response->headers[h].value, sizeof(response->headers[h].value),
             "GET, POST, PUT, DELETE, OPTIONS");
    h++;

    // Echo the headers the client announced it wants to send, falling back to a
    // sane default. Reflecting the request keeps custom headers working without
    // the server having to enumerate them.
    const char *req_headers =
        request_header(request, "Access-Control-Request-Headers");
    snprintf(response->headers[h].name, sizeof(response->headers[h].name),
             "Access-Control-Allow-Headers");
    snprintf(response->headers[h].value, sizeof(response->headers[h].value),
             "%s", req_headers ? req_headers : "Content-Type");
    h++;

    snprintf(response->headers[h].name, sizeof(response->headers[h].name),
             "Access-Control-Max-Age");
    snprintf(response->headers[h].value, sizeof(response->headers[h].value),
             "86400");
    h++;
    response->header_count = h;

    // send_http_response appends Access-Control-Allow-Origin from this field.
    if (origin)
    {
      snprintf(response->cors_origin, sizeof(response->cors_origin), "%s", allow);
    }
    return send_http_response(conn, response);
  }

  // Actual request: record the allowed origin so send_http_response adds the
  // header to whatever the downstream handler produces. Only tag genuine CORS
  // requests (those carrying an Origin) so same-origin traffic is untouched.
  if (origin)
  {
    snprintf(response->cors_origin, sizeof(response->cors_origin), "%s", allow);
  }
  return middleware_next(conn, request, response, next);
}
