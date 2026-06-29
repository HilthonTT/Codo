#ifndef BALANCER_H
#define BALANCER_H

#include <stdbool.h>
#include <stdint.h>

// A single upstream the balancer can forward to.
typedef struct backend
{
  char host[256];
  int port;
  bool healthy;
} backend_t;

// Balancer-wide configuration. Populate this from the environment / CLI in
// main.c, then hand it to your accept loop.
typedef struct balancer_config
{
  int listen_port;     // port the balancer listens on
  backend_t *backends; // dynamically sized pool of upstreams
  int backend_count;
} balancer_config_t;

// --- Implement these as you build out the balancer ---
// int balancer_init(balancer_config_t *cfg);
// int balancer_run(balancer_config_t *cfg);
// void balancer_cleanup(balancer_config_t *cfg);

#endif
