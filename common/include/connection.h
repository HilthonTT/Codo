#ifndef CONNECTION_H
#define CONNECTION_H

#include <netinet/in.h>
#include <sys/socket.h>
#include <openssl/ssl.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#include "config.h"
#include "http_types.h"

// Per-connection state machine
typedef enum
{
  CONN_STATE_READING_REQUEST,
  CONN_STATE_PROCESSING,
  CONN_STATE_WRITING_RESPONSE,
  CONN_STATE_KEEPALIVE,
  CONN_STATE_WEBSOCKET,
  CONN_STATE_CLOSING,
} connection_state_t;

typedef struct connection
{
  int socket_fd;
  struct sockaddr_storage client_addr; // IPv4 or IPv6; see net_util.h
  connection_state_t state;

  // SSL support
  SSL *ssl;
  bool ssl_enabled;

  // Request/response data. Both buffers are heap-allocated and sized to what
  // the connection actually needs, rather than being worst-case arrays inside
  // every connection_t: a 1 MiB write buffer plus the header tables used to
  // make this struct 1.3 MB, so 1000 idle keep-alive connections cost 1.3 GB
  // of almost entirely untouched memory. read_buffer starts at
  // CONN_READ_BUFFER_MIN and grows on demand up to the server's
  // max_request_size; write_buffer is allocated only when there is a response
  // to stage. conn_release_idle() hands both back between requests.
  char *read_buffer;
  size_t read_buffer_pos; // bytes currently buffered
  size_t read_buffer_cap; // bytes allocated

  char *write_buffer;
  size_t write_buffer_pos;  // bytes already written to the socket
  size_t write_buffer_size; // bytes staged for sending
  size_t write_buffer_cap;  // bytes allocated

  http_request_t request;
  http_response_t response;

  // Authenticated user for the request currently being processed. Set (or
  // cleared) by jwt_middleware before the route handler runs; a user id of 0
  // means unauthenticated. Only meaningful for routes behind the JWT
  // middleware.
  uint64_t auth_user_id;
  char auth_username[64];

  // PROXY protocol: whether the header at the head of this connection has
  // been consumed yet. Only meaningful when the server trusts the header.
  bool proxy_header_done;

  // Set true while a worker thread has handed this connection off to the
  // storage thread pool to run a blocking handler. While set, the owning
  // worker must not touch the connection (no event handling, no timeout
  // reaping) -- the pool thread owns it until it re-arms the socket for output.
  _Atomic(bool) offloaded;

  // Expect: 100-continue progress for the current request: bytes of the
  // interim "100 Continue" line already written to the socket, and whether it
  // has fully flushed. Both reset when the connection is reused for the next
  // request.
  size_t continue_pos;
  bool continue_sent;

  // Timing
  time_t last_activity;
  time_t connection_time;

  // Websocket support
  bool websocket_handshake_complete;
  // Set once a Close frame (or a fatal protocol error) has been staged for
  // sending: the write path tears the connection down after flushing it rather
  // than returning to frame-read mode.
  bool websocket_closing;

  // File serving
  int file_fd;
  off_t file_offset;
  size_t file_size;

  // Linked list for connection pool
  struct connection *next;
  struct connection *prev;
} connection_t;

// Forward decl to avoid pulling server.h here (circular).
struct http_server;

// Initial (and idle) size of the read buffer. A request larger than this grows
// it on demand; see conn_reserve_read.
#define CONN_READ_BUFFER_MIN BUFFER_SIZE

// Ensure read_buffer and the request/response header tables exist. Called
// before a connection is used for I/O; returns false on allocation failure.
bool conn_ensure_buffers(connection_t *conn);

// Ensure read_buffer can hold `need` bytes, growing geometrically up to `max`.
// Returns false when `need` exceeds `max` or the allocation fails.
bool conn_reserve_read(connection_t *conn, size_t need, size_t max);

// Ensure write_buffer can hold `need` bytes (allocating it if absent), up to
// MAX_RESPONSE_SIZE. Returns false when it cannot.
bool conn_reserve_write(connection_t *conn, size_t need);

// Release the per-request memory of an idle keep-alive connection: the write
// buffer and both header tables are freed and the read buffer is shrunk back to
// CONN_READ_BUFFER_MIN. What remains between requests is the struct plus one
// small read buffer.
void conn_release_idle(connection_t *conn);

connection_t *allocate_connection(struct http_server *server);
void free_connection(struct http_server *server, connection_t *conn);
void cleanup_connection(connection_t *conn);

int set_socket_nonblocking(int fd);
int set_socket_options(int fd);

#endif
