#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "http_protocol.h"
#include "route.h"
#include "server.h"
#include "util.h"

int add_route(http_server_t *server, const char *pattern,
              http_method_t method, route_handler_t handler)
{
  if (!server || !pattern || !handler)
  {
    return -1;
  }
  route_t *route = calloc(1, sizeof(route_t));
  if (!route)
  {
    return -1;
  }
  strncpy(route->pattern, pattern, sizeof(route->pattern) - 1);
  route->pattern[sizeof(route->pattern) - 1] = '\0';
  route->method = method;
  route->handler = handler;
  route->offload = false;
  route->next = server->routes;
  server->routes = route;
  return 0;
}

int add_route_offloaded(http_server_t *server, const char *pattern,
                        http_method_t method, route_handler_t handler)
{
  if (add_route(server, pattern, method, handler) != 0)
  {
    return -1;
  }
  // add_route prepends, so the route we just created is the list head.
  server->routes->offload = true;
  return 0;
}

route_t *find_route(http_server_t *server, const char *uri, http_method_t method)
{
  if (!server || !uri)
  {
    return NULL;
  }

  // First pass: prefer an exact pattern match.
  for (route_t *r = server->routes; r; r = r->next)
  {
    if (r->method != method)
    {
      continue;
    }
    size_t plen = strlen(r->pattern);
    if (plen > 0 && r->pattern[plen - 1] == '*')
    {
      continue; // wildcard routes are handled in the second pass
    }
    if (strcmp(r->pattern, uri) == 0)
    {
      return r;
    }
  }

  // Second pass: trailing '*' patterns match by prefix, e.g. "/api/todos/*"
  // matches "/api/todos/42".
  for (route_t *r = server->routes; r; r = r->next)
  {
    if (r->method != method)
    {
      continue;
    }
    size_t plen = strlen(r->pattern);
    if (plen > 0 && r->pattern[plen - 1] == '*' &&
        strncmp(r->pattern, uri, plen - 1) == 0)
    {
      return r;
    }
  }

  return NULL;
}

int default_file_handler(connection_t *conn, http_request_t *request, http_response_t *response)
{
  (void)response;

  if (request->method != HTTP_GET && request->method != HTTP_HEAD)
  {
    return send_error_response(conn, HTTP_METHOD_NOT_ALLOWED, "Method not allowed");
  }

  // Check if path is safe
  if (!is_valid_uri(request->uri))
  {
    return send_error_response(conn, HTTP_FORBIDDEN, "Access denied");
  }

  // Construct file path
  char file_path[2048];
  snprintf(file_path, sizeof(file_path), "%s%s", g_server.document_root, request->uri);

  // Check if file exists
  struct stat file_stat;
  if (stat(file_path, &file_stat) < 0)
  {
    return send_error_response(conn, HTTP_NOT_FOUND, "File not found");
  }

  // Send file
  return send_file_response(conn, file_path);
}
