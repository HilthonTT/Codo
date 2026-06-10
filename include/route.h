#ifndef OTA_ROUTE_H
#define OTA_ROUTE_H

#include "connection.h"
#include "http_types.h"

// Route handler function type
typedef int (*route_handler_t)(connection_t *conn, http_request_t *request, http_response_t *response);

typedef struct route {
  char pattern[512];
  http_method_t method;
  route_handler_t handler;
  struct route *next;
} route_t;

struct http_server;

int add_route(struct http_server *server, const char *pattern,
              http_method_t method, route_handler_t handler);
route_t *find_route(struct http_server *server, const char *uri,
                    http_method_t method);
int default_file_handler(connection_t *conn, http_request_t *request,
                         http_response_t *response);

#endif
