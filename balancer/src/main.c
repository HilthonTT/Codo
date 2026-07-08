#define _GNU_SOURCE

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "balancer.h"
#include "env.h"

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

// Parse a comma-separated backend list of the form
//   host:port[:weight],host:port[:weight],...
// e.g. "127.0.0.1:8080,127.0.0.1:8081:2". Returns the number registered.
static int parse_backends(load_balancer_t *lb, const char *spec)
{
  char buf[1024];
  strncpy(buf, spec, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  int added = 0;
  char *saveptr = NULL;
  for (char *tok = strtok_r(buf, ",", &saveptr); tok != NULL;
       tok = strtok_r(NULL, ",", &saveptr))
  {
    // Trim leading whitespace.
    while (*tok == ' ' || *tok == '\t')
    {
      tok++;
    }

    char *colon = strchr(tok, ':');
    if (!colon)
    {
      fprintf(stderr, "skipping backend \"%s\": expected host:port\n", tok);
      continue;
    }
    *colon = '\0';
    const char *host = tok;
    char *port_str = colon + 1;

    // Optional ":weight" suffix.
    int weight = 1;
    char *colon2 = strchr(port_str, ':');
    if (colon2)
    {
      *colon2 = '\0';
      weight = atoi(colon2 + 1);
    }

    int port = atoi(port_str);
    if (port <= 0 || port > 65535)
    {
      fprintf(stderr, "skipping backend \"%s\": invalid port %s\n", host, port_str);
      continue;
    }

    if (add_backend(lb, host, port, weight) == 0)
    {
      printf("  backend %s:%d (weight %d)\n", host, port, weight > 0 ? weight : 1);
      added++;
    }
  }

  return added;
}

int main(int argc, char *argv[])
{
  const char *env_file = getenv("ENV_FILE");
  if (load_env(env_file ? env_file : ".env") != 0)
  {
    perror("load_env");
  }

  int port = env_int("BALANCER_PORT", 8000);
  const char *backends = env_str("BALANCER_BACKENDS", "127.0.0.1:8080");

  // CLI args override the environment: argv[1] = listen port, argv[2] = backends.
  if (argc > 1)
  {
    port = atoi(argv[1]);
  }
  if (argc > 2)
  {
    backends = argv[2];
  }

  if (port <= 0 || port > 65535)
  {
    fprintf(stderr, "Invalid balancer port: %d\n", port);
    return 1;
  }

  // A dead backend write would otherwise raise SIGPIPE and kill the balancer.
  signal(SIGINT, balancer_signal_handler);
  signal(SIGTERM, balancer_signal_handler);
  signal(SIGPIPE, SIG_IGN);

  if (load_balancer_init(&g_lb, port) != 0)
  {
    fprintf(stderr, "Failed to initialize load balancer\n");
    return 1;
  }

  printf("Configured backends:\n");
  if (parse_backends(&g_lb, backends) == 0)
  {
    fprintf(stderr, "No valid backends configured (BALANCER_BACKENDS)\n");
    return 1;
  }

  printf("Load balancer listening on port %d, proxying to %d backend(s)\n",
         port, g_lb.backend_count);

  return load_balancer_main_loop(&g_lb);
}
