#include <stdio.h>

#include "api.h"
#include "handlers.h"
#include "http_types.h"
#include "route.h"
#include "storage.h"
#include "todo_handlers.h"
#include "user_auth.h"

int api_init(const char *db_file, const char *wal_file)
{
  if (init_storage_engine(db_file, wal_file) != 0)
  {
    fprintf(stderr, "Failed to initialize storage engine\n");
    return -1;
  }

  // Seeds the id counter from the engine we just opened, so it has to run
  // after init_storage_engine().
  todo_api_init();

  // Brings up the crypto framework, the JWT secret and the user id counter.
  // Fatal on failure: without it passwords can't be hashed and tokens can't be
  // signed, so every authenticated route would 500.
  if (user_api_init() != 0)
  {
    fprintf(stderr, "Failed to initialize user auth\n");
    todo_api_shutdown();
    cleanup_storage_engine();
    return -1;
  }

  return 0;
}

void api_mount(http_server_t *server)
{
  add_route(server, "/api/hello", HTTP_GET, api_hello_handler);
  add_route(server, "/api/echo", HTTP_POST, api_echo_handler);
  add_route(server, "/api/status", HTTP_GET, api_status_handler);
  add_route(server, "/api/stats", HTTP_GET, api_stats_handler);
  add_route(server, "/ws/chat", HTTP_GET, websocket_chat_handler);

  // Observability endpoints. /metrics and /healthz only read atomics, so they
  // run inline; /readyz proves the storage engine can serve a transaction, so
  // it is offloaded to the storage pool like the Todo routes.
  add_route(server, "/metrics", HTTP_GET, api_metrics_handler);
  add_route(server, "/healthz", HTTP_GET, api_healthz_handler);
  add_route_offloaded(server, "/readyz", HTTP_GET, api_readyz_handler);

  // The Todo CRUD routes on top of the storage engine, and the user account /
  // login endpoints that issue the tokens guarding them.
  todo_api_register_routes(server);
  user_api_register_routes(server);
}

void api_shutdown(void)
{
  todo_api_shutdown();
  cleanup_storage_engine();

  // Crypto framework last: nothing above needs it during teardown.
  user_api_shutdown();
}
