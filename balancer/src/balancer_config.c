#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "balancer.h"
#include "balancer_config.h"
#include "env.h"

int balancer_config_load(balancer_config_t *config, int argc, char *argv[])
{
  if (!config)
  {
    return -1;
  }

  const char *env_file = getenv("ENV_FILE");
  if (load_env(env_file ? env_file : ".env") != 0)
  {
    perror("load_env");
  }

  // env_* reads the .env table first and lets the real environment win.
  config->port = env_int("BALANCER_PORT", 8000);
  config->backends = env_str("BALANCER_BACKENDS", "127.0.0.1:8080");
  config->proxy_protocol = env_bool("PROXY_PROTOCOL", false);

  // CLI args override the environment.
  if (argc > 1)
  {
    config->port = atoi(argv[1]);
  }
  if (argc > 2)
  {
    config->backends = argv[2];
  }

  if (config->port <= 0 || config->port > 65535)
  {
    fprintf(stderr, "Invalid balancer port: %d\n", config->port);
    return -1;
  }

  return 0;
}

int balancer_add_backends(load_balancer_t *lb, const char *spec)
{
  char buf[1024];
  strncpy(buf, spec, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  int added = 0;
  char *saveptr = NULL;
  for (char *tok = strtok_r(buf, ",", &saveptr); tok != NULL;
       tok = strtok_r(NULL, ",", &saveptr))
  {
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
