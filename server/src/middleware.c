#define _GNU_SOURCE

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "middleware.h"
#include "server.h"
#include "util.h"

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

  printf("%s %s %s -> %d (%.2f ms)\n",
         client_ip,
         http_method_to_string(request->method),
         request->uri,
         response->status,
         elapsed_ms);
  fflush(stdout);

  return rc;
}
