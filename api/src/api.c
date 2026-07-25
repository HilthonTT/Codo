#define _GNU_SOURCE

#include <stdio.h>

#include "api.h"
#include "handlers.h"
#include "http_types.h"
#include "route.h"
#include "storage.h"
#include "todo_handlers.h"

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
  return 0;
}

void api_mount(http_server_t *server)
{
  add_route(server, "/api/hello", HTTP_GET, api_hello_handler);
  add_route(server, "/api/echo", HTTP_POST, api_echo_handler);
  add_route(server, "/api/status", HTTP_GET, api_status_handler);
  add_route(server, "/api/stats", HTTP_GET, api_stats_handler);
  add_route(server, "/ws/chat", HTTP_GET, websocket_chat_handler);

  // The Todo CRUD routes on top of the storage engine.
  todo_api_register_routes(server);
}

void api_shutdown(void)
{
  todo_api_shutdown();
  cleanup_storage_engine();
}
