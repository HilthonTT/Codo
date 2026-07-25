#ifndef API_H
#define API_H

#include "server.h"

// The web API layer: every route Codo serves other than static files, plus the
// storage engine that backs the Todo resource. It sits on top of the HTTP
// server core (routes, middleware, workers) and owns nothing of it -- main()
// wires the two together and does no request-level work itself.

// Bring the API up: open the btree storage engine on the given files, seed the
// todo id counter from what is already on disk, build the read cache, and start
// the user-auth layer (crypto framework, JWT secret, user id counter).
// Call once, before mounting the routes. Returns 0 on success, -1 on failure
// (in which case nothing is left initialized).
int api_init(const char *db_file, const char *wal_file);

// Mount every API route on `server`. Handlers that touch the storage engine are
// registered for thread-pool offload so they never block a worker's event loop.
//
//   GET  /api/hello    GET  /api/status    GET /api/cache    GET /metrics
//   POST /api/echo     GET  /api/stats     GET /ws/chat      GET /healthz
//   /api/todos[/{id}]  -- full CRUD, see todo_handlers.h     GET /readyz
//   /api/auth/*        -- register / login / me, see user_auth.h
void api_mount(http_server_t *server);

// Tear the API down: drop the read cache, checkpoint/close the storage engine,
// and shut the crypto framework down. Call once the server has stopped serving,
// so no handler can be running.
void api_shutdown(void);

#endif
