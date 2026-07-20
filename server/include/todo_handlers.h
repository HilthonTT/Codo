#ifndef TODO_HANDLERS_H
#define TODO_HANDLERS_H

#include "connection.h"
#include "http_types.h"
#include "server.h"

// Seed the in-memory id counter from todos already persisted on disk and build
// the single-todo read cache (capacity from TODO_CACHE_CAPACITY, default 1024).
// Call once after init_storage_engine().
void todo_api_init(void);

// Tear the read cache down. Call once the server has stopped serving.
void todo_api_shutdown(void);

// Register the Todo CRUD routes on the given server. All /api/todos routes
// require a JWT (see jwt_middleware in user_auth.h) and operate only on the
// authenticated user's todos:
//   GET    /api/todos        list the user's todos
//   POST   /api/todos        create a todo owned by the user
//   GET    /api/todos/{id}   fetch one todo (served from the LRU cache on a hit)
//   PUT    /api/todos/{id}   replace one todo
//   DELETE /api/todos/{id}   delete one todo
//   GET    /api/cache        read-cache hit/miss counters (public)
void todo_api_register_routes(http_server_t *server);

// Individual route handlers (exposed for explicit wiring / testing).
int todo_list_handler(connection_t *conn, http_request_t *request, http_response_t *response);
int todo_create_handler(connection_t *conn, http_request_t *request, http_response_t *response);
int todo_get_handler(connection_t *conn, http_request_t *request, http_response_t *response);
int todo_update_handler(connection_t *conn, http_request_t *request, http_response_t *response);
int todo_delete_handler(connection_t *conn, http_request_t *request, http_response_t *response);
int todo_cache_stats_handler(connection_t *conn, http_request_t *request, http_response_t *response);

#endif
