#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "auth.h"
#include "env.h"
#include "http_protocol.h"
#include "server.h"

#define MAX_API_KEYS 32
#define MAX_KEY_LEN 256

static char g_keys[MAX_API_KEYS][MAX_KEY_LEN];
static int g_key_count = 0;

void auth_init(void)
{
  g_key_count = 0;

  const char *raw = env_str("API_KEYS", "");
  if (!raw || !*raw)
  {
    return; // no keys configured -> auth disabled
  }

  // Split the comma-separated list on a private copy, trimming surrounding
  // whitespace so "k1, k2" is accepted. Empty entries are skipped.
  char buf[MAX_API_KEYS * MAX_KEY_LEN];
  strncpy(buf, raw, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  char *saveptr = NULL;
  char *tok = strtok_r(buf, ",", &saveptr);
  while (tok && g_key_count < MAX_API_KEYS)
  {
    while (*tok == ' ' || *tok == '\t')
    {
      tok++;
    }
    size_t len = strlen(tok);
    while (len > 0 && (tok[len - 1] == ' ' || tok[len - 1] == '\t'))
    {
      tok[--len] = '\0';
    }
    if (len > 0 && len < MAX_KEY_LEN)
    {
      strncpy(g_keys[g_key_count], tok, MAX_KEY_LEN - 1);
      g_keys[g_key_count][MAX_KEY_LEN - 1] = '\0';
      g_key_count++;
    }
    tok = strtok_r(NULL, ",", &saveptr);
  }
}

static const char *request_header(const http_request_t *request, const char *name)
{
  for (int i = 0; i < request->header_count; i++)
  {
    if (strcasecmp(request->headers[i].name, name) == 0)
    {
      return request->headers[i].value;
    }
  }
  return NULL;
}

// Only mutating verbs are guarded; reads and preflight stay open.
static bool method_is_protected(http_method_t method)
{
  return method == HTTP_POST || method == HTTP_PUT ||
         method == HTTP_DELETE || method == HTTP_PATCH;
}

// Paths owned by the JWT layer (see jwt_middleware): the todo API is guarded
// per-user by bearer tokens, and the auth endpoints must stay reachable
// without credentials or nobody could ever obtain a token. Requiring an API
// key on top would demand two Authorization mechanisms on one request.
static bool path_uses_jwt(const char *uri)
{
  return strncmp(uri, "/api/todos", 10) == 0 ||
         strncmp(uri, "/api/auth/", 10) == 0;
}

// Length-agnostic constant-time equality: the loop always runs over the longer
// of the two strings so a caller can't learn the key length or its matching
// prefix from response timing.
static bool secure_equals(const char *a, const char *b)
{
  size_t la = strlen(a);
  size_t lb = strlen(b);
  size_t n = la > lb ? la : lb;
  unsigned char diff = (unsigned char)(la ^ lb);
  for (size_t i = 0; i < n; i++)
  {
    unsigned char ca = i < la ? (unsigned char)a[i] : 0;
    unsigned char cb = i < lb ? (unsigned char)b[i] : 0;
    diff |= (unsigned char)(ca ^ cb);
  }
  return diff == 0;
}

static bool key_is_valid(const char *key)
{
  bool ok = false;
  // Check against every configured key (no early break) so the number of
  // comparisons doesn't depend on which key matched.
  for (int i = 0; i < g_key_count; i++)
  {
    if (secure_equals(key, g_keys[i]))
    {
      ok = true;
    }
  }
  return ok;
}

static int send_auth_error(connection_t *conn, http_request_t *request,
                           http_response_t *response, http_status_t status,
                           const char *msg)
{
  if (response->body)
  {
    free(response->body);
    response->body = NULL;
  }

  char body[128];
  snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);

  response->status = status;
  snprintf(response->version, sizeof(response->version), "HTTP/1.1");
  response->body = strdup(body);
  response->body_length = response->body ? strlen(response->body) : 0;
  response->keep_alive = request->keep_alive;

  int h = 0;
  snprintf(response->headers[h].name, sizeof(response->headers[h].name), "Content-Type");
  snprintf(response->headers[h].value, sizeof(response->headers[h].value), "application/json");
  h++;
  if (status == HTTP_UNAUTHORIZED)
  {
    snprintf(response->headers[h].name, sizeof(response->headers[h].name), "WWW-Authenticate");
    snprintf(response->headers[h].value, sizeof(response->headers[h].value),
             "Bearer realm=\"codo\", charset=\"UTF-8\"");
    h++;
  }
  response->header_count = h;

  return send_http_response(conn, response);
}

int auth_middleware(connection_t *conn, http_request_t *request,
                    http_response_t *response, middleware_ctx_t *next)
{
  if (g_key_count == 0 || !method_is_protected(request->method) ||
      path_uses_jwt(request->uri))
  {
    return middleware_next(conn, request, response, next);
  }

  // Accept the key from X-API-Key, or an Authorization: Bearer <key> header.
  const char *presented = request_header(request, "X-API-Key");
  if (!presented)
  {
    const char *authz = request_header(request, "Authorization");
    if (authz && strncasecmp(authz, "Bearer ", 7) == 0)
    {
      presented = authz + 7;
      while (*presented == ' ')
      {
        presented++;
      }
    }
  }

  if (!presented || !*presented)
  {
    return send_auth_error(conn, request, response, HTTP_UNAUTHORIZED,
                           "missing API key");
  }
  if (!key_is_valid(presented))
  {
    return send_auth_error(conn, request, response, HTTP_FORBIDDEN,
                           "invalid API key");
  }

  return middleware_next(conn, request, response, next);
}
