#define _GNU_SOURCE

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "btree_storage.h"
#include "handlers.h"
#include "route.h"
#include "server.h"
#include "ssl_util.h"
#include "todo_handlers.h"
#include "env.h"

// Global server instance and shutdown flag, shared across translation units
// (workers reach into g_server directly via these externs).
http_server_t g_server;
volatile bool g_running = true;

static void http_signal_handler(int signum)
{
  (void)signum;
  g_running = false;
  g_server.running = false;
}

int main(int argc, char *argv[])
{
  const char *env_file = getenv("ENV_FILE");
  if (load_env(env_file ? env_file : ".env") != 0)
  {
    perror("load_env");
  }

  int port = env_int("PORT", 8080);
  const char *document_root = env_str("DOCUMENT_ROOT", "/var/www/html");
  const char *ssl_cert = env_str("SSL_CERT_FILE", "server.crt");
  const char *ssl_key = env_str("SSL_KEY_FILE", "server.key");
  bool ssl_enabled = env_bool("SSL_ENABLED", true);
  const char *db_file = env_str("DB_FILE", "codo.db");
  const char *wal_file = env_str("WAL_FILE", "codo.wal");

  // Command line arguments still override everything
  if (argc > 1)
  {
    port = atoi(argv[1]);
  }
  if (argc > 2)
  {
    document_root = argv[2];
  }

  if (port <= 0 || port > 65535)
  {
    fprintf(stderr, "Invalid port: %d\n", port);
    return 1;
  }

  // Setup signal handlers
  signal(SIGINT, http_signal_handler);
  signal(SIGTERM, http_signal_handler);
  signal(SIGPIPE, SIG_IGN);

  // Initialize HTTP server
  if (http_server_init(&g_server, port, document_root) != 0)
  {
    fprintf(stderr, "Failed to initialize HTTP server\n");
    return 1;
  }

  // Initialize the btree storage engine that backs the Todo API.
  if (init_storage_engine(db_file, wal_file) != 0)
  {
    fprintf(stderr, "Failed to initialize storage engine\n");
    http_server_cleanup(&g_server);
    return 1;
  }
  todo_api_init();

  // Add example routes
  add_route(&g_server, "/api/hello", HTTP_GET, api_hello_handler);
  add_route(&g_server, "/api/echo", HTTP_POST, api_echo_handler);
  add_route(&g_server, "/api/status", HTTP_GET, api_status_handler);
  add_route(&g_server, "/ws/chat", HTTP_GET, websocket_chat_handler);

  // Mount the Todo CRUD web API on top of the storage engine.
  todo_api_register_routes(&g_server);

  // Enable SSL if requested and certificates are available
  if (ssl_enabled &&
      access(ssl_cert, F_OK) == 0 && access(ssl_key, F_OK) == 0)
  {
    if (init_ssl(&g_server, ssl_cert, ssl_key) == 0)
    {
      printf("SSL enabled (%s / %s)\n", ssl_cert, ssl_key);
    }
    else
    {
      fprintf(stderr, "SSL init failed, continuing without SSL\n");
    }
  }

  printf("HTTP server starting on port %d\n", port);
  printf("Document root: %s\n", document_root);

  if (http_server_start(&g_server) != 0)
  {
    fprintf(stderr, "Failed to start HTTP server\n");
    http_server_cleanup(&g_server);
    return 1;
  }

  // http_server_start blocks in the accept loop until server->running is
  // cleared by the signal handler, so once we get here we are shutting down.
  http_server_stop(&g_server);
  http_server_cleanup(&g_server);

  // Flush and close the storage engine (final checkpoint).
  cleanup_storage_engine();

  printf("HTTP server stopped\n");

  return 0;
}
