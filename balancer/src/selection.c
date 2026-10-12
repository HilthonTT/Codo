#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hash.h"
#include "net_util.h"
#include "selection.h"

static void passive_recovery(load_balancer_t *lb)
{
  assert(lb);

  // Passive recovery: re-admit backends whose last failure is far enough
  // in the past. Cheap enough to run on every selection. Uses
  // CLOCK_MONOTONIC to match mark_backend_failure — must be the same clock.
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  time_t now = (time_t)ts.tv_sec;
  for (int i = 0; i < lb->backend_count; i++)
  {
    backend_t *b = &lb->backends[i];
    if (b->health_status == 0 && (now - b->last_health_check) >= HEALTH_RECOVERY_SECS)
    {
      b->health_status = 1;
      b->consecutive_failures = 0;
    }
  }
}

backend_t *backend_round_robin_select(load_balancer_t *lb)
{
  assert(lb);

  backend_t *selected = NULL;
  int total_weight = 0;

  pthread_mutex_lock(&lb->backend_mutex);

  passive_recovery(lb);

  for (int i = 0; i < lb->backend_count; i++)
  {
    if (lb->backends[i].health_status == 1)
    {
      total_weight += lb->backends[i].weight;
    }
  }

  if (total_weight == 0)
  {
    pthread_mutex_unlock(&lb->backend_mutex);
    return NULL;
  }

  // Smooth weighted round-robin selection (nginx-style). current_weight can
  // be well below zero (e.g. a light backend left negative after a heavy peer
  // went unhealthy), so the first healthy candidate always seeds the search.
  int best_weight = 0;

  for (int i = 0; i < lb->backend_count; i++)
  {
    backend_t *backend = &lb->backends[i];

    if (backend->health_status != 1)
    {
      continue;
    }

    backend->current_weight += backend->weight;

    if (!selected || backend->current_weight > best_weight)
    {
      best_weight = backend->current_weight;
      selected = backend;
    }
  }

  if (selected)
  {
    selected->current_weight -= total_weight;
  }

  pthread_mutex_unlock(&lb->backend_mutex);
  return selected;
}

backend_t *least_connection_select(load_balancer_t *lb)
{
  assert(lb);

  backend_t *least_conn_backend = NULL;
  int least_conn = -1;

  pthread_mutex_lock(&lb->backend_mutex);

  passive_recovery(lb);

  for (int i = 0; i < lb->backend_count; i++)
  {
    backend_t *backend = &lb->backends[i];

    if (backend->health_status != 1)
    {
      continue;
    }

    if (least_conn == -1 || backend->current_connections < least_conn)
    {
      least_conn = backend->current_connections;
      least_conn_backend = backend;
    }
  }

  // No reservation here: lb_handle_new_connection increments
  // current_connections once the connection is actually established, and
  // lb_close_connection decrements it. Incrementing here as well counted every
  // connection twice and leaked one count on each failed dispatch.

  pthread_mutex_unlock(&lb->backend_mutex);

  return least_conn_backend;
}

backend_t *ip_hash_select(load_balancer_t *lb, const struct sockaddr_storage *client_addr)
{
  assert(lb);

  pthread_mutex_lock(&lb->backend_mutex);
  passive_recovery(lb);

  if (lb->backend_count <= 0)
  {
    pthread_mutex_unlock(&lb->backend_mutex);
    return NULL;
  }

  // Hash the raw address bytes rather than a text form: it covers IPv4 and
  // IPv6 uniformly, and an IPv4 client arriving on a dual-stack listener maps
  // to the same backend either way.
  uint32_t hash = (uint32_t)net_addr_hash(client_addr);
  size_t idx = hash % (uint32_t)lb->backend_count;

  size_t initial_idx = idx;

  for (int i = 0; i < lb->backend_count; i++)
  {
    size_t check_idx = (initial_idx + (uint32_t)i) % (uint32_t)lb->backend_count;

    backend_t *backend = &lb->backends[check_idx];
    if (backend->health_status != 1)
    {
      continue;
    }
    pthread_mutex_unlock(&lb->backend_mutex);
    return backend;
  }

  pthread_mutex_unlock(&lb->backend_mutex);

  return NULL;
}

backend_t *random_select(load_balancer_t *lb)
{
  assert(lb);

  pthread_mutex_lock(&lb->backend_mutex);
  passive_recovery(lb);

  if (lb->backend_count <= 0)
  {
    pthread_mutex_unlock(&lb->backend_mutex);
    return NULL;
  }

  size_t alive_indices[lb->backend_count];
  size_t alive_count = 0;

  for (int i = 0; i < lb->backend_count; i++)
  {
    backend_t *backend = &lb->backends[i];
    if (backend->health_status == 1)
    {
      alive_indices[alive_count++] = (size_t)i;
    }
  }

  if (alive_count == 0)
  {
    pthread_mutex_unlock(&lb->backend_mutex);
    return NULL;
  }

  // rand() picked under the lock so the snapshot can't go stale between
  // building alive_indices and dereferencing. rand() itself is not
  // thread-safe — consider rand_r() with per-thread seed, or a better PRNG.
  size_t pick = alive_indices[rand() % alive_count];
  backend_t *selected = &lb->backends[pick];

  pthread_mutex_unlock(&lb->backend_mutex);
  return selected;
}
