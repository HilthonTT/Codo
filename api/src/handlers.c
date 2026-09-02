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
  const char *hello_msg =
      "{\"message\": \"Hello, World!\", \"timestamp\": \"2026-01-01T00:00:00Z\"}";

  return send_json_response(conn, request, response, HTTP_OK, hello_msg);
}

int api_echo_handler(connection_t *conn, http_request_t *request, http_response_t *response)
{
  if (request->method != HTTP_POST)
  {
    return send_error_response(conn, HTTP_METHOD_NOT_ALLOWED, "Method not allowed");
  }

  // Echoed verbatim, so the copy must be binary-safe rather than string-based.
  return send_body_response(conn, request, response, HTTP_OK, "text/plain",
                            request->body ? request->body : "",
                            request->body_length);
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

  return send_json_response(conn, request, response, HTTP_OK, status_json);
}

int api_stats_handler(connection_t *conn, http_request_t *request, http_response_t *response)
{
  char stats_json[512];
  if (stats_format_json(stats_json, sizeof(stats_json)) < 0)
  {
    return send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR, "Stats too large");
  }

  return send_json_response(conn, request, response, HTTP_OK, stats_json);
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

  // The Prometheus content type; text/plain with the exposition format version.
  return send_body_response(conn, request, response, HTTP_OK,
                            "text/plain; version=0.0.4; charset=utf-8", body,
                            strlen(body));
}

// GET /healthz -- liveness. Cheap and inline: if the event loop can answer, the
// process is alive. Never touches storage.
int api_healthz_handler(connection_t *conn, http_request_t *request, http_response_t *response)
{
  return send_json_response(conn, request, response, HTTP_OK, "{\"status\":\"ok\"}");
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

  return send_json_response(conn, request, response, HTTP_OK, "{\"status\":\"ready\"}");
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
