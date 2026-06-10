#ifndef OTA_SERVER_H
#define OTA_SERVER_H

#include <openssl/ssl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "connection.h"
#include "route.h"
#include "worker.h"

typedef struct http_server {
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

  // Route handling
  route_t *routes;
  route_handler_t default_handler;

  // Connection pool
  connection_t *connection_pool;
  int max_connections;
  int active_connections;

  // Configuration
  bool enable_keepalive;
  int keepalive_timeout;
  bool enable_compression;
  size_t max_request_size;

  // Statistics
  uint64_t total_requests;
  uint64_t total_connections;
  uint64_t active_connections_count;

  // Control flags
  volatile bool running;
  pthread_mutex_t stats_mutex;
} http_server_t;

int http_server_init(http_server_t *server, int port, const char *document_root);
int http_server_start(http_server_t *server);
int http_server_stop(http_server_t *server);
int http_server_cleanup(http_server_t *server);

// Shared globals, defined in main.c.
extern http_server_t g_server;
extern volatile bool g_running;

#endif
