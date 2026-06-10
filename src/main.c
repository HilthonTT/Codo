#define _GNU_SOURCE

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "handlers.h"
#include "route.h"
#include "server.h"
#include "ssl_util.h"

// Global server instance and shutdown flag, shared across translation units
// (workers reach into g_server directly via these externs).
http_server_t g_server;
volatile bool g_running = true;

static void signal_handler(int signum) {
  (void)signum;
  g_running = false;
  g_server.running = false;
}

int main(int argc, char *argv[]) {
  int port = 8080;
  char *document_root = "/var/www/html";

  // Parse command line arguments
  if (argc > 1) {
    port = atoi(argv[1]);
  }
  if (argc > 2) {
    document_root = argv[2];
  }

  // Setup signal handlers
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);
  signal(SIGPIPE, SIG_IGN);

  // Initialize HTTP server
  if (http_server_init(&g_server, port, document_root) != 0) {
    fprintf(stderr, "Failed to initialize HTTP server\n");
    return 1;
  }

  // Add example routes
  add_route(&g_server, "/api/hello", HTTP_GET, api_hello_handler);
  add_route(&g_server, "/api/echo", HTTP_POST, api_echo_handler);
  add_route(&g_server, "/api/status", HTTP_GET, api_status_handler);
  add_route(&g_server, "/ws/chat", HTTP_GET, websocket_chat_handler);

  // Enable SSL if certificates are available
  if (access("server.crt", F_OK) == 0 && access("server.key", F_OK) == 0) {
    if (init_ssl(&g_server, "server.crt", "server.key") == 0) {
      printf("SSL enabled\n");
    }
  }

  printf("HTTP server starting on port %d\n", port);
  printf("Document root: %s\n", document_root);

  if (http_server_start(&g_server) != 0) {
    fprintf(stderr, "Failed to start HTTP server\n");
    http_server_cleanup(&g_server);
    return 1;
  }

  // http_server_start blocks in the accept loop until server->running is
  // cleared by the signal handler, so once we get here we are shutting down.
  http_server_stop(&g_server);
  http_server_cleanup(&g_server);

  printf("HTTP server stopped\n");

  return 0;
}
