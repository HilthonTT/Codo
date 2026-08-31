#include <signal.h>
#include <stdio.h>

#include "api.h"
#include "middleware.h"
#include "server.h"
#include "server_config.h"
#include "ssl_util.h"

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

static void install_signal_handlers(void)
{
  signal(SIGINT, http_signal_handler);
  signal(SIGTERM, http_signal_handler);
  // A write to a peer that has already gone away would otherwise kill us.
  signal(SIGPIPE, SIG_IGN);
}

int main(int argc, char *argv[])
{
  server_config_t config;
  if (server_config_load(&config, argc, argv) != 0)
  {
    return 1;
  }

  install_signal_handlers();

  if (http_server_init(&g_server, config.port, config.document_root) != 0)
  {
    fprintf(stderr, "Failed to initialize HTTP server\n");
    return 1;
  }

  if (api_init(config.db_file, config.wal_file) != 0)
  {
    http_server_cleanup(&g_server);
    return 1;
  }

  // The JWT stage of the chain needs the secret api_init() loaded, so the
  // middleware is registered after it.
  g_server.trust_proxy_protocol = config.trust_proxy_protocol;
  register_default_middleware(&g_server, config.cors_allow_origin);
  api_mount(&g_server);
  init_ssl_if_available(&g_server, config.ssl_enabled, config.ssl_cert_file,
                        config.ssl_key_file);

  printf("HTTP server starting on port %d\n", config.port);
  printf("Document root: %s\n", config.document_root);

  if (http_server_start(&g_server) != 0)
  {
    fprintf(stderr, "Failed to start HTTP server\n");
    http_server_cleanup(&g_server);
    api_shutdown();
    return 1;
  }

  // http_server_start blocks in the accept loop until server->running is
  // cleared by the signal handler, so once we get here we are shutting down.
  http_server_stop(&g_server);
  http_server_cleanup(&g_server);

  // No handler can be running now, so the API's caches, storage engine and
  // crypto framework are safe to tear down (this takes the final checkpoint).
  api_shutdown();

  printf("HTTP server stopped\n");

  return 0;
}
