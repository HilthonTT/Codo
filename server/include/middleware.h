#ifndef MIDDLEWARE_H
#define MIDDLEWARE_H

#include "connection.h"
#include "http_types.h"
#include "route.h"

struct http_server;

// Opaque cursor threaded through a middleware chain. A middleware receives one
// and passes it to middleware_next() to invoke the rest of the pipeline.
typedef struct middleware_ctx middleware_ctx_t;

// A middleware wraps the rest of the request pipeline. It may inspect or modify
// the request/response, run code both before and after calling middleware_next
// (e.g. start a timer, then log the outcome), and short-circuit the pipeline by
// writing a response itself and NOT calling middleware_next. Return value
// follows the handler convention: 0 on success, non-zero on error.
typedef int (*middleware_fn_t)(connection_t *conn, http_request_t *request,
                               http_response_t *response, middleware_ctx_t *next);

struct middleware_ctx
{
  const middleware_fn_t *chain; // ordered array of middlewares
  int count;                    // number of middlewares in the chain
  int index;                    // index of the next middleware to run
  route_handler_t handler;      // terminal handler, run once the chain is done
};

// Register a middleware. Middlewares run in registration order: the first
// registered is the outermost wrapper -- it runs first on the way in and last
// on the way out. Returns 0 on success, -1 if the table is full or args are bad.
int add_middleware(struct http_server *server, middleware_fn_t fn);

// Invoke the next stage of the pipeline (the next middleware, or the terminal
// handler if the chain is exhausted). Called by a middleware with the cursor it
// was handed.
int middleware_next(connection_t *conn, http_request_t *request,
                    http_response_t *response, middleware_ctx_t *next);

// Run `handler` wrapped by the server's registered middleware chain. This is
// the single entry point the worker uses instead of invoking a handler
// directly, so every handler (route and default) goes through the chain.
int run_with_middleware(connection_t *conn, http_request_t *request,
                        http_response_t *response, route_handler_t handler);

// Built-in middleware: logs client IP, method, URI, response status, and the
// time spent handling the request. Register it first so it times the whole
// chain (including any other middleware).
int logging_middleware(connection_t *conn, http_request_t *request,
                       http_response_t *response, middleware_ctx_t *next);

#endif
