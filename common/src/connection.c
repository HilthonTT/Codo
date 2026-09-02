#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/ssl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "connection.h"

// Tear down everything owned by a single connection. This is shared between
// the server's connection pool and any other consumer (e.g. the balancer),
// so it deliberately does not touch server-wide bookkeeping -- see
// free_connection() in the server for that.
// Round `need` up to the next power-of-two-ish step, starting from `from`.
static size_t grow_to(size_t from, size_t need)
{
  size_t cap = from ? from : CONN_READ_BUFFER_MIN;
  while (cap < need)
  {
    cap *= 2;
  }
  return cap;
}

bool conn_ensure_buffers(connection_t *conn)
{
  if (!conn)
  {
    return false;
  }
  if (!conn->read_buffer)
  {
    conn->read_buffer = malloc(CONN_READ_BUFFER_MIN);
    if (!conn->read_buffer)
    {
      return false;
    }
    conn->read_buffer_cap = CONN_READ_BUFFER_MIN;
    conn->read_buffer_pos = 0;
    conn->read_buffer[0] = '\0';
  }
  if (!conn->request.headers)
  {
    conn->request.headers = calloc(MAX_HEADERS, sizeof(http_header_t));
    if (!conn->request.headers)
    {
      return false;
    }
  }
  if (!conn->response.headers)
  {
    conn->response.headers = calloc(MAX_HEADERS, sizeof(http_header_t));
    if (!conn->response.headers)
    {
      return false;
    }
  }
  return true;
}

bool conn_reserve_read(connection_t *conn, size_t need, size_t max)
{
  if (!conn || need > max)
  {
    return false;
  }
  if (conn->read_buffer && conn->read_buffer_cap >= need)
  {
    return true;
  }

  size_t cap = grow_to(conn->read_buffer_cap, need);
  if (cap > max)
  {
    cap = max;
  }
  char *grown = realloc(conn->read_buffer, cap);
  if (!grown)
  {
    return false;
  }
  conn->read_buffer = grown;
  conn->read_buffer_cap = cap;
  return cap >= need;
}

bool conn_reserve_write(connection_t *conn, size_t need)
{
  if (!conn || need > MAX_RESPONSE_SIZE)
  {
    return false;
  }
  if (conn->write_buffer && conn->write_buffer_cap >= need)
  {
    return true;
  }

  size_t cap = grow_to(conn->write_buffer_cap, need);
  if (cap > MAX_RESPONSE_SIZE)
  {
    cap = MAX_RESPONSE_SIZE;
  }
  char *grown = realloc(conn->write_buffer, cap);
  if (!grown)
  {
    return false;
  }
  conn->write_buffer = grown;
  conn->write_buffer_cap = cap;
  return cap >= need;
}

void conn_release_idle(connection_t *conn)
{
  if (!conn)
  {
    return;
  }

  free(conn->write_buffer);
  conn->write_buffer = NULL;
  conn->write_buffer_cap = 0;
  conn->write_buffer_pos = 0;
  conn->write_buffer_size = 0;

  free(conn->request.headers);
  conn->request.headers = NULL;
  conn->request.header_count = 0;
  free(conn->response.headers);
  conn->response.headers = NULL;
  conn->response.header_count = 0;

  // Keep a minimum read buffer: the next request has to land somewhere, and
  // re-allocating it on every keep-alive round trip would trade memory for
  // allocator churn on the hot path. Only a buffer that grew for an oversized
  // request is handed back.
  if (conn->read_buffer && conn->read_buffer_cap > CONN_READ_BUFFER_MIN &&
      conn->read_buffer_pos <= CONN_READ_BUFFER_MIN)
  {
    char *shrunk = realloc(conn->read_buffer, CONN_READ_BUFFER_MIN);
    if (shrunk)
    {
      conn->read_buffer = shrunk;
      conn->read_buffer_cap = CONN_READ_BUFFER_MIN;
    }
  }
}

void cleanup_connection(connection_t *conn)
{
  if (!conn)
  {
    return;
  }
  if (conn->ssl)
  {
    SSL_shutdown(conn->ssl);
    SSL_free(conn->ssl);
    conn->ssl = NULL;
  }
  if (conn->socket_fd >= 0)
  {
    close(conn->socket_fd);
    conn->socket_fd = -1;
  }
  if (conn->file_fd >= 0)
  {
    close(conn->file_fd);
    conn->file_fd = -1;
  }
  if (conn->request.body)
  {
    free(conn->request.body);
    conn->request.body = NULL;
  }
  if (conn->response.body)
  {
    free(conn->response.body);
    conn->response.body = NULL;
  }
  free(conn->read_buffer);
  conn->read_buffer = NULL;
  conn->read_buffer_cap = 0;
  free(conn->write_buffer);
  conn->write_buffer = NULL;
  conn->write_buffer_cap = 0;
  free(conn->request.headers);
  conn->request.headers = NULL;
  free(conn->response.headers);
  conn->response.headers = NULL;
}

int set_socket_nonblocking(int fd)
{
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0)
  {
    return -1;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int set_socket_options(int fd)
{
  int optval = 1;

  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0)
  {
    return -1;
  }

  if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval)) < 0)
  {
    return -1;
  }

  return 0;
}
