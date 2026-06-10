#ifndef OTA_WEBSOCKET_H
#define OTA_WEBSOCKET_H

#include <stddef.h>

#include "connection.h"
#include "http_types.h"

int handle_websocket_upgrade(connection_t *conn, http_request_t *request);
int handle_websocket_frame(connection_t *conn, const char *data, size_t length);
void generate_websocket_accept_key(const char *key, char *accept_key);

#endif
