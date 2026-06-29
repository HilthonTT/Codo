#define _GNU_SOURCE

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

// Shared primitives from common/ -- the balancer reuses the same config
// loader, socket helpers and HTTP request parser the server is built on.
#include "balancer.h"
#include "env.h"

volatile bool g_running = true;

static void balancer_signal_handler(int signum)
{
  (void)signum;
  g_running = false;
}

int main(int argc, char *argv[])
{
  const char *env_file = getenv("ENV_FILE");
  if (load_env(env_file ? env_file : ".env") != 0)
  {
    perror("load_env");
  }

  balancer_config_t cfg = {0};
  cfg.listen_port = env_int("BALANCER_PORT", 8000);

  // CLI override: codo-balancer [listen_port]
  if (argc > 1)
  {
    cfg.listen_port = atoi(argv[1]);
  }

  signal(SIGINT, balancer_signal_handler);
  signal(SIGTERM, balancer_signal_handler);
  signal(SIGPIPE, SIG_IGN);

  printf("codo-balancer scaffold: would listen on port %d\n", cfg.listen_port);
  printf("Not implemented yet -- build out balancer_init/run/cleanup in balancer/src.\n");

  // TODO(you): set up the listen socket (set_socket_options /
  // set_socket_nonblocking from common/connection.h), accept connections,
  // parse with parse_http_request (common/http_protocol.h), pick a backend,
  // and proxy bytes through.

  return 0;
}
