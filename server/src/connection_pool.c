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
  atomic_fetch_add(&server->active_connections, 1);
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
    // Saturating atomic decrements: never wrap below zero if free is somehow
    // called more often than allocate.
    int cur = atomic_load(&server->active_connections);
    while (cur > 0 &&
           !atomic_compare_exchange_weak(&server->active_connections, &cur, cur - 1))
    {
      // cur is reloaded by the CAS on failure; retry.
    }

    uint64_t cnt = atomic_load(&server->active_connections_count);
    while (cnt > 0 &&
           !atomic_compare_exchange_weak(&server->active_connections_count, &cnt, cnt - 1))
    {
      // retry
    }
  }
  stats_record_connection_closed();
  free(conn);
}
