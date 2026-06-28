#define _GNU_SOURCE

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "compression.h"
#include "connection.h"
#include "server.h"
#include "stats.h"

connection_t *allocate_connection(http_server_t *server)
{
  if (!server)
  {
    return NULL;
  }
  connection_t *conn = calloc(1, sizeof(connection_t));
  if (!conn)
  {
    return NULL;
  }
  // calloc zeroes everything, but 0 is a valid (stdin) fd. Default the fd
  // fields to -1 so cleanup_connection knows there is nothing to close yet.
  conn->socket_fd = -1;
  conn->file_fd = -1;
  pthread_mutex_lock(&server->stats_mutex);
  server->active_connections++;
  pthread_mutex_unlock(&server->stats_mutex);
  stats_record_connection_accepted();
  return conn;
}

void free_connection(http_server_t *server, connection_t *conn)
{
  if (!conn)
  {
    return;
  }
  cleanup_connection(conn);
  if (server)
  {
    pthread_mutex_lock(&server->stats_mutex);
    if (server->active_connections > 0)
    {
      server->active_connections--;
    }
    if (server->active_connections_count > 0)
    {
      server->active_connections_count--;
    }
    pthread_mutex_unlock(&server->stats_mutex);
  }
  stats_record_connection_closed();
  free(conn);
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
