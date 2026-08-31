#ifndef BALANCER_CONFIG_H
#define BALANCER_CONFIG_H

#include <stdbool.h>

#include "types.h"

// Runtime configuration of the load balancer binary. `backends` points into the
// environment / the .env table / argv, all of which outlive main(), so nothing
// here needs freeing.
typedef struct
{
  int port;
  const char *backends;
  // Emit a PROXY protocol v1 header to backends (PROXY_PROTOCOL).
  bool proxy_protocol; // "host:port[:weight],..." -- see balancer_add_backends
} balancer_config_t;

// Load the configuration in precedence order: .env file (path from ENV_FILE,
// default ".env"), then the real environment, then the command line
// (argv[1] = listen port, argv[2] = backend list).
//
// Returns 0 on success, -1 if the result is unusable (an out-of-range port).
int balancer_config_load(balancer_config_t *config, int argc, char *argv[]);

// Register every backend in a comma-separated spec of the form
//   host:port[:weight],host:port[:weight],...
// e.g. "127.0.0.1:8080,127.0.0.1:8081:2". The weight defaults to 1; a malformed
// entry is reported and skipped rather than aborting the whole list. Returns
// the number of backends registered.
int balancer_add_backends(load_balancer_t *lb, const char *spec);

#endif
