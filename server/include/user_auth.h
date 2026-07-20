#ifndef USER_AUTH_H
#define USER_AUTH_H

#include "connection.h"
#include "http_types.h"
#include "middleware.h"
#include "server.h"

// User accounts + JWT authentication for the Todo API.
//
// Users live in the same btree as todos under "user:<username>" keys; the
// value is {"id":N,"username":..,"salt":..,"hash":..,"iter":N} with the
// PBKDF2-HMAC-SHA256 salt/hash hex-encoded. Passwords are never stored.

// Initialize the crypto framework, load the JWT secret (JWT_SECRET) and token
// TTL (JWT_TTL_SECONDS, default 3600), and seed the user id counter from disk.
// Call once after init_storage_engine(). Returns 0 on success.
int user_api_init(void);

// Tear the crypto framework down. Call once the server has stopped serving.
void user_api_shutdown(void);

// Register the auth routes on the given server:
//   POST /api/auth/register  create a user   {"username":..,"password":..}
//   POST /api/auth/login     issue a JWT     {"username":..,"password":..}
//   GET  /api/auth/me        current user (requires a Bearer token)
void user_api_register_routes(http_server_t *server);

// Middleware: require a valid `Authorization: Bearer <jwt>` on the routes that
// serve per-user data (/api/todos* and /api/auth/me). On success the verified
// claims are published to the handler via conn->auth_user_id /
// conn->auth_username; any other path passes through with those fields
// cleared. A missing, invalid, or expired token is answered 401.
int jwt_middleware(connection_t *conn, http_request_t *request,
                   http_response_t *response, middleware_ctx_t *next);

// Individual route handlers (exposed for explicit wiring / testing).
int user_register_handler(connection_t *conn, http_request_t *request, http_response_t *response);
int user_login_handler(connection_t *conn, http_request_t *request, http_response_t *response);
int user_me_handler(connection_t *conn, http_request_t *request, http_response_t *response);

#endif
