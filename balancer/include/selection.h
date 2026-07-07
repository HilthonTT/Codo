#ifndef SELECTION_H
#define sELECTION_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

// Passive health check tuning. A backend is marked unhealthy after this many
// consecutive failures (connect refused, read/write errors, EPOLLERR/HUP).
// It is re-admitted after HEALTH_RECOVERY_SECS have passed since the last
// failure, giving it a chance to prove itself again.
#define HEALTH_FAILURE_THRESHOLD 3
#define HEALTH_RECOVERY_SECS 30

#include <assert.h>
#include <time.h>

#include "types.h"
#include "hash.h"

backend_t *backend_round_robin_select(load_balancer_t *lb);
backend_t *least_connection_select(load_balancer_t *lb);
backend_t *ip_hash_select(load_balancer_t *lb);
backend_t *random_select(load_balancer_t *lb);
backend_t *random_select(load_balancer_t *lb);

#endif