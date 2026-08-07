#include <signal.h>
#include <stdio.h>

#include "balancer.h"
#include "balancer_config.h"

// Single global load balancer instance.
static load_balancer_t g_lb;

static void balancer_signal_handler(int signum)
{
  (void)signum;
  // The accept/proxy loop lives in epoll_wait; the default action for an
  // unhandled SIGINT/SIGTERM terminates the process, so simply re-raise with
  // the default disposition to exit promptly.
  signal(signum, SIG_DFL);
  raise(signum);
}

static void install_signal_handlers(void)
{
  signal(SIGINT, balancer_signal_handler);
  signal(SIGTERM, balancer_signal_handler);
  // A dead backend write would otherwise raise SIGPIPE and kill the balancer.
  signal(SIGPIPE, SIG_IGN);
}

int main(int argc, char *argv[])
{
  balancer_config_t config;
  if (balancer_config_load(&config, argc, argv) != 0)
  {
    return 1;
  }

  install_signal_handlers();

  if (load_balancer_init(&g_lb, config.port) != 0)
  {
    fprintf(stderr, "Failed to initialize load balancer\n");
    return 1;
  }

  printf("Configured backends:\n");
  if (balancer_add_backends(&g_lb, config.backends) == 0)
  {
    fprintf(stderr, "No valid backends configured (BALANCER_BACKENDS)\n");
    return 1;
  }

  printf("Load balancer listening on port %d, proxying to %d backend(s)\n",
         config.port, g_lb.backend_count);

  return load_balancer_main_loop(&g_lb);
}
