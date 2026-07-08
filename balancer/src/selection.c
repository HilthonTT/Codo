#define _GNU_SOURCE

#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hash.h"
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

// Format the client's IPv4 address into buf. Falls back to "0.0.0.0" when the
// address is missing or not AF_INET so the hash stays deterministic instead of
// reading garbage.
static const char *get_client_ip(const struct sockaddr_in *client_addr,
                                 char *buf, size_t buf_len)
{
  assert(buf && buf_len >= INET_ADDRSTRLEN);

  if (!client_addr || client_addr->sin_family != AF_INET ||
      !inet_ntop(AF_INET, &client_addr->sin_addr, buf, (socklen_t)buf_len))
  {
    snprintf(buf, buf_len, "0.0.0.0");
  }
  return buf;
}

backend_t *backend_round_robin_select(load_balancer_t *lb)
{
  assert(lb);

  backend_t *selected = NULL;
  int total_weight = 0;

  pthread_mutex_lock(&lb->backend_mutex);

  passive_recovery(lb);

  // Calculate total weight of healthy backends
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

  // Smooth weighted round-robin selection (nginx-style).
  int best_weight = -1;

  for (int i = 0; i < lb->backend_count; i++)
  {
    backend_t *backend = &lb->backends[i];

    if (backend->health_status != 1)
    {
      continue;
    }

    backend->current_weight += backend->weight;

    if (backend->current_weight > best_weight)
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

  // Reserve the connection while still holding the lock so concurrent
  // callers don't all pick the same "least" backend (thundering herd).
  // NOTE: if the caller already increments current_connections after
  // dispatch, DELETE this block to avoid double-counting.
  if (least_conn_backend)
  {
    least_conn_backend->current_connections++;
  }

  pthread_mutex_unlock(&lb->backend_mutex);

  return least_conn_backend;
}

backend_t *ip_hash_select(load_balancer_t *lb, const struct sockaddr_in *client_addr)
{
  assert(lb);

  pthread_mutex_lock(&lb->backend_mutex);
  passive_recovery(lb);

  if (lb->backend_count <= 0)
  {
    pthread_mutex_unlock(&lb->backend_mutex);
    return NULL;
  }

  char ip_buf[INET_ADDRSTRLEN];
  const char *ip = get_client_ip(client_addr, ip_buf, sizeof(ip_buf));

  uint32_t hash = fnv1_32(ip, strlen(ip));
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
