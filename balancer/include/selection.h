#ifndef SELECTION_H
#define SELECTION_H

// Passive health check tuning. A backend is marked unhealthy after this many
// consecutive failures (connect refused, read/write errors, EPOLLERR/HUP).
// It is re-admitted after HEALTH_RECOVERY_SECS have passed since the last
// failure, giving it a chance to prove itself again.
#define HEALTH_FAILURE_THRESHOLD 3
#define HEALTH_RECOVERY_SECS 30

#include <netinet/in.h>

#include "types.h"

backend_t *backend_round_robin_select(load_balancer_t *lb);
backend_t *least_connection_select(load_balancer_t *lb);
backend_t *ip_hash_select(load_balancer_t *lb, const struct sockaddr_in *client_addr);
backend_t *random_select(load_balancer_t *lb);

#endif