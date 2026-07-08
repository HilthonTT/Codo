#ifndef WEBSOCKET_H
#define WEBSOCKET_H

#include <stddef.h>

#include "connection.h"
#include "http_types.h"

// Complete the RFC 6455 opening handshake: validate the client's
// Sec-WebSocket-Key, stage a "101 Switching Protocols" response in the
// connection's write buffer, and mark the connection as an established
// WebSocket. The worker sends the staged bytes and then switches the connection
// into frame-read mode. Returns 0 on success; on a bad/failed handshake it
// stages an HTTP error response instead and returns its result.
int handle_websocket_upgrade(connection_t *conn, http_request_t *request);

// Process every complete client frame currently buffered in conn->read_buffer,
// appending the server's replies -- echoes of data frames, pongs for pings, and
// a Close for a Close or protocol error -- to conn->write_buffer. Consumed bytes
// are removed from the read buffer (any trailing partial frame is preserved for
// the next read). *should_close is set when the connection must be torn down
// after the staged replies are flushed. Always returns 0.
int ws_process_frames(connection_t *conn, bool *should_close);

void generate_websocket_accept_key(const char *key, char *accept_key);

#endif
