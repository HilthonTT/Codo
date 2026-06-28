#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "handlers.h"
#include "http_protocol.h"
#include "http_types.h"
#include "server.h"
#include "stats.h"
#include "websocket.h"

int api_hello_handler(connection_t *conn, http_request_t *request, http_response_t *response)
{
  const char *hello_msg = "{\"message\": \"Hello, World!\", \"timestamp\": \"2026-01-01T00:00:00Z\"}";

  response->status = HTTP_OK;
  strcpy(response->version, "HTTP/1.1");
  response->body = strdup(hello_msg);
  response->body_length = strlen(hello_msg);
  response->keep_alive = request->keep_alive;

  strcpy(response->headers[0].name, "Content-Type");
  strcpy(response->headers[0].value, "application/json");
  response->header_count = 1;

  return send_http_response(conn, response);
}

int api_echo_handler(connection_t *conn, http_request_t *request, http_response_t *response)
{
  if (request->method != HTTP_POST)
  {
    return send_error_response(conn, HTTP_METHOD_NOT_ALLOWED, "Method not allowed");
  }

  size_t body_len = request->body_length;
  const char *src = request->body ? request->body : "";

  response->body = malloc(body_len + 1);
  if (!response->body)
  {
    return send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR, "Out of memory");
  }
  memcpy(response->body, src, body_len);
  response->body[body_len] = '\0';
  response->body_length = body_len;

  response->status = HTTP_OK;
  strcpy(response->version, "HTTP/1.1");
  response->keep_alive = request->keep_alive;

  strcpy(response->headers[0].name, "Content-Type");
  strcpy(response->headers[0].value, "text/plain");
  response->header_count = 1;

  return send_http_response(conn, response);
}

int api_status_handler(connection_t *conn, http_request_t *request, http_response_t *response)
{
  char status_json[1024];
  snprintf(status_json, sizeof(status_json),
           "{"
           "\"server\": \"%s\","
           "\"total_connections\": %lu,"
           "\"total_requests\": %lu,"
           "\"active_connections\": %lu"
           "}",
           g_server.server_name,
           (unsigned long)g_server.total_connections,
           (unsigned long)g_server.total_requests,
           (unsigned long)g_server.active_connections_count);

  response->status = HTTP_OK;
  strcpy(response->version, "HTTP/1.1");
  response->body = strdup(status_json);
  response->body_length = response->body ? strlen(response->body) : 0;
  response->keep_alive = request->keep_alive;

  strcpy(response->headers[0].name, "Content-Type");
  strcpy(response->headers[0].value, "application/json");
  response->header_count = 1;

  return send_http_response(conn, response);
}

int api_stats_handler(connection_t *conn, http_request_t *request, http_response_t *response)
{
  char stats_json[512];
  if (stats_format_json(stats_json, sizeof(stats_json)) < 0)
  {
    return send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR, "Stats too large");
  }

  response->status = HTTP_OK;
  strcpy(response->version, "HTTP/1.1");
  response->body = strdup(stats_json);
  response->body_length = response->body ? strlen(response->body) : 0;
  response->keep_alive = request->keep_alive;

  strcpy(response->headers[0].name, "Content-Type");
  strcpy(response->headers[0].value, "application/json");
  response->header_count = 1;

  return send_http_response(conn, response);
}

int websocket_chat_handler(connection_t *conn, http_request_t *request, http_response_t *response)
{
  (void)response;
  if (request->is_websocket_upgrade)
  {
    return handle_websocket_upgrade(conn, request);
  }
  return send_error_response(conn, HTTP_BAD_REQUEST, "WebSocket upgrade required");
}
