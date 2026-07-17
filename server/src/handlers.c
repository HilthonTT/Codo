#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "handlers.h"
#include "http_protocol.h"
#include "http_types.h"
#include "metrics.h"
#include "server.h"
#include "stats.h"
#include "storage.h"
#include "websocket.h"

int api_hello_handler(connection_t *conn, http_request_t *request, http_response_t *response)
{
  const char *hello_msg = "{\"message\": \"Hello, World!\", \"timestamp\": \"2026-01-01T00:00:00Z\"}";

  response->status = HTTP_OK;
  snprintf(response->version, sizeof(response->version), "HTTP/1.1");
  response->body = strdup(hello_msg);
  response->body_length = strlen(hello_msg);
  response->keep_alive = request->keep_alive;

  snprintf(response->headers[0].name, sizeof(response->headers[0].name), "Content-Type");
  snprintf(response->headers[0].value, sizeof(response->headers[0].value), "application/json");
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
  snprintf(response->version, sizeof(response->version), "HTTP/1.1");
  response->keep_alive = request->keep_alive;

  snprintf(response->headers[0].name, sizeof(response->headers[0].name), "Content-Type");
  snprintf(response->headers[0].value, sizeof(response->headers[0].value), "text/plain");
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
           (unsigned long)atomic_load(&g_server.total_connections),
           (unsigned long)atomic_load(&g_server.total_requests),
           (unsigned long)atomic_load(&g_server.active_connections_count));

  response->status = HTTP_OK;
  snprintf(response->version, sizeof(response->version), "HTTP/1.1");
  response->body = strdup(status_json);
  response->body_length = response->body ? strlen(response->body) : 0;
  response->keep_alive = request->keep_alive;

  snprintf(response->headers[0].name, sizeof(response->headers[0].name), "Content-Type");
  snprintf(response->headers[0].value, sizeof(response->headers[0].value), "application/json");
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
  snprintf(response->version, sizeof(response->version), "HTTP/1.1");
  response->body = strdup(stats_json);
  response->body_length = response->body ? strlen(response->body) : 0;
  response->keep_alive = request->keep_alive;

  snprintf(response->headers[0].name, sizeof(response->headers[0].name), "Content-Type");
  snprintf(response->headers[0].value, sizeof(response->headers[0].value), "application/json");
  response->header_count = 1;

  return send_http_response(conn, response);
}

// GET /metrics -- Prometheus text exposition. Request counters and the latency
// histogram come from the metrics module; a few process gauges are appended
// here since they live on g_server. Runs inline: it only reads atomics.
int api_metrics_handler(connection_t *conn, http_request_t *request, http_response_t *response)
{
  char body[8192];
  int n = metrics_format_prometheus(body, sizeof(body));
  if (n < 0)
  {
    return send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR, "Metrics too large");
  }

  // Append server-level gauges that the shared metrics module can't see.
  int extra = snprintf(body + n, sizeof(body) - (size_t)n,
                       "# HELP codo_active_connections Currently open connections.\n"
                       "# TYPE codo_active_connections gauge\n"
                       "codo_active_connections %lu\n"
                       "# HELP codo_connections_total Connections accepted since start.\n"
                       "# TYPE codo_connections_total counter\n"
                       "codo_connections_total %lu\n"
                       "# HELP codo_requests_received_total Requests seen by the accept loop.\n"
                       "# TYPE codo_requests_received_total counter\n"
                       "codo_requests_received_total %lu\n",
                       (unsigned long)atomic_load(&g_server.active_connections_count),
                       (unsigned long)atomic_load(&g_server.total_connections),
                       (unsigned long)atomic_load(&g_server.total_requests));
  if (extra < 0 || (size_t)extra >= sizeof(body) - (size_t)n)
  {
    return send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR, "Metrics too large");
  }

  response->status = HTTP_OK;
  snprintf(response->version, sizeof(response->version), "HTTP/1.1");
  response->body = strdup(body);
  response->body_length = response->body ? strlen(response->body) : 0;
  response->keep_alive = request->keep_alive;

  // The Prometheus content type; text/plain with the exposition format version.
  snprintf(response->headers[0].name, sizeof(response->headers[0].name), "Content-Type");
  snprintf(response->headers[0].value, sizeof(response->headers[0].value),
           "text/plain; version=0.0.4; charset=utf-8");
  response->header_count = 1;

  return send_http_response(conn, response);
}

// GET /healthz -- liveness. Cheap and inline: if the event loop can answer, the
// process is alive. Never touches storage.
int api_healthz_handler(connection_t *conn, http_request_t *request, http_response_t *response)
{
  const char *body = "{\"status\":\"ok\"}";
  response->status = HTTP_OK;
  snprintf(response->version, sizeof(response->version), "HTTP/1.1");
  response->body = strdup(body);
  response->body_length = response->body ? strlen(response->body) : 0;
  response->keep_alive = request->keep_alive;

  snprintf(response->headers[0].name, sizeof(response->headers[0].name), "Content-Type");
  snprintf(response->headers[0].value, sizeof(response->headers[0].value), "application/json");
  response->header_count = 1;

  return send_http_response(conn, response);
}

// GET /readyz -- readiness. Proves the storage engine can serve a transaction,
// so a load balancer only routes traffic once the backend is truly ready. This
// touches storage, so it is registered offloaded (runs on the storage pool).
int api_readyz_handler(connection_t *conn, http_request_t *request, http_response_t *response)
{
  transaction_t *txn = begin_transaction();
  if (!txn)
  {
    return send_error_response(conn, HTTP_SERVICE_UNAVAILABLE, "Storage not ready");
  }
  commit_transaction(txn);
  free(txn);

  const char *body = "{\"status\":\"ready\"}";
  response->status = HTTP_OK;
  snprintf(response->version, sizeof(response->version), "HTTP/1.1");
  response->body = strdup(body);
  response->body_length = response->body ? strlen(response->body) : 0;
  response->keep_alive = request->keep_alive;

  snprintf(response->headers[0].name, sizeof(response->headers[0].name), "Content-Type");
  snprintf(response->headers[0].value, sizeof(response->headers[0].value), "application/json");
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
