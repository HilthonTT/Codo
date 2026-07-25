#ifndef HANDLERS_H
#define HANDLERS_H

#include "connection.h"
#include "http_types.h"

int api_hello_handler(connection_t *conn, http_request_t *request, http_response_t *response);
int api_echo_handler(connection_t *conn, http_request_t *request, http_response_t *response);
int api_status_handler(connection_t *conn, http_request_t *request, http_response_t *response);
int api_stats_handler(connection_t *conn, http_request_t *request, http_response_t *response);
int websocket_chat_handler(connection_t *conn, http_request_t *request, http_response_t *response);

#endif
