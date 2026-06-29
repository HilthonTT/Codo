#define _GNU_SOURCE

#include <pthread.h>
#include <stdlib.h>

#include "connection.h"
#include "server.h"
#include "stats.h"

// Connection allocation tied to the server's live-connection bookkeeping.
// Kept out of common/connection.c because it reaches into http_server_t; a
// balancer manages its own connection accounting and reuses cleanup_connection
// and the socket helpers from common/ instead.
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
