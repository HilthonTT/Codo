#ifndef SERVER_H
#define SERVER_H

#include <openssl/ssl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "connection.h"
#include "middleware.h"
#include "route.h"
#include "thread_pool.h"
#include "worker.h"

typedef struct http_server
{
  int listen_fd;
  int listen_port;
  char *document_root;
  char *server_name;

  // SSL configuration
  SSL_CTX *ssl_ctx;
  bool ssl_enabled;
  char *ssl_cert_file;
  char *ssl_key_file;

  // Worker threads
  worker_thread_t workers[WORKER_THREADS];
  int worker_count;

  // Thread pool for offloading blocking (storage-backed) handlers so they
  // don't stall a worker's epoll loop. NULL if pool creation failed, in which
  // case handlers run inline.
  thread_pool_t *pool;

  // Route handling
  route_t *routes;
  route_handler_t default_handler;

  // Middlewares wrapping every route handler, run in registration order (the
  // first registered is the outermost). See middleware.h.
  middleware_fn_t middlewares[MAX_MIDDLEWARES];
  int middleware_count;

  // Connection pool
  connection_t *connection_pool;
  int max_connections;
  _Atomic(int) active_connections;

  // Configuration
  bool enable_keepalive;
  int keepalive_timeout;
  bool enable_compression;
  size_t max_request_size;
  // Value emitted in Access-Control-Allow-Origin by the CORS middleware. "*" by
  // default; override with the CORS_ALLOW_ORIGIN environment variable.
  char cors_allow_origin[256];

  // Statistics. These are updated concurrently from every worker thread and the
  // accept loop, so they're atomics rather than plain integers behind a mutex --
  // it keeps the counter bumps off the request hot path.
  _Atomic(uint64_t) total_requests;
  _Atomic(uint64_t) total_connections;
  _Atomic(uint64_t) active_connections_count;

  // Control flags
  volatile bool running;
} http_server_t;

int http_server_init(http_server_t *server, int port, const char *document_root);
int http_server_start(http_server_t *server);
int http_server_stop(http_server_t *server);
int http_server_cleanup(http_server_t *server);

// Shared globals, defined in main.c.
extern http_server_t g_server;
extern volatile bool g_running;

#endif
