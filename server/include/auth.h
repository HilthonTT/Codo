#ifndef AUTH_H
#define AUTH_H

#include "connection.h"
#include "http_types.h"
#include "middleware.h"

// Load the accepted API keys from the environment. Call once at startup. Key:
//   API_KEYS   comma-separated list of accepted keys (default empty)
// When the list is empty, auth is disabled and the middleware is a pass-through,
// so the server is open by default and only locks down once keys are set.
void auth_init(void);

// Middleware: require a valid API key on mutating requests (POST/PUT/DELETE/
// PATCH). Safe methods (GET/HEAD/OPTIONS) always pass. The key may be supplied
// as `X-API-Key: <key>` or `Authorization: Bearer <key>`. A protected request
// with no credentials is answered 401; one with a wrong key, 403.
int auth_middleware(connection_t *conn, http_request_t *request,
                    http_response_t *response, middleware_ctx_t *next);

#endif
