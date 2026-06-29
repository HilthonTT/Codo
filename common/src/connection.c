#define _GNU_SOURCE

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/ssl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "compression.h"
#include "connection.h"

// Tear down everything owned by a single connection. This is shared between
// the server's connection pool and any other consumer (e.g. the balancer),
// so it deliberately does not touch server-wide bookkeeping -- see
// free_connection() in the server for that.
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
  if (conn->gzip_initialized)
  {
    cleanup_gzip_compression(conn);
  }
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

  // Reuse address
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0)
  {
    return -1;
  }

  // Disable Nagle's algorithm
  if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval)) < 0)
  {
    return -1;
  }

  return 0;
}
