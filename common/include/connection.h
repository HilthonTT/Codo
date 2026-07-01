#ifndef CONNECTION_H
#define CONNECTION_H

#include <netinet/in.h>
#include <openssl/ssl.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <time.h>
#include <zlib.h>

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
  struct sockaddr_in client_addr;
  connection_state_t state;

  // SSL support
  SSL *ssl;
  bool ssl_enabled;

  // Request/response data
  char read_buffer[BUFFER_SIZE];
  size_t read_buffer_pos;
  size_t read_buffer_size;

  char write_buffer[MAX_RESPONSE_SIZE];
  size_t write_buffer_pos;
  size_t write_buffer_size;

  http_request_t request;
  http_response_t response;

  // Timing
  time_t last_activity;
  time_t connection_time;

  // Websocket support
  bool websocket_handshake_complete;
  char websocket_frame_buffer[BUFFER_SIZE];
  size_t websocket_frame_pos;

  // File serving
  int file_fd;
  off_t file_offset;
  size_t file_size;

  // Compression
  z_stream gzip_stream;
  bool gzip_initialized;

  // Linked list for connection pool
  struct connection *next;
  struct connection *prev;
} connection_t;

// Forward decl to avoid pulling server.h here (circular).
struct http_server;

connection_t *allocate_connection(struct http_server *server);
void free_connection(struct http_server *server, connection_t *conn);
void cleanup_connection(connection_t *conn);

int set_socket_nonblocking(int fd);
int set_socket_options(int fd);

#endif
