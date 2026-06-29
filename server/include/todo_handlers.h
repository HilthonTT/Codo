#ifndef TODO_HANDLERS_H
#define TODO_HANDLERS_H

#include "connection.h"
#include "http_types.h"
#include "server.h"

// Seed the in-memory id counter from todos already persisted on disk.
// Call once after init_storage_engine().
void todo_api_init(void);

// Register the Todo CRUD routes on the given server:
//   GET    /api/todos        list all todos
//   POST   /api/todos        create a todo
//   GET    /api/todos/{id}   fetch one todo
//   PUT    /api/todos/{id}   replace one todo
//   DELETE /api/todos/{id}   delete one todo
void todo_api_register_routes(http_server_t *server);

// Individual route handlers (exposed for explicit wiring / testing).
int todo_list_handler(connection_t *conn, http_request_t *request, http_response_t *response);
int todo_create_handler(connection_t *conn, http_request_t *request, http_response_t *response);
int todo_get_handler(connection_t *conn, http_request_t *request, http_response_t *response);
int todo_update_handler(connection_t *conn, http_request_t *request, http_response_t *response);
int todo_delete_handler(connection_t *conn, http_request_t *request, http_response_t *response);

#endif
