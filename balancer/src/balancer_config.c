#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "balancer.h"
#include "balancer_config.h"
#include "env.h"

// Largest accepted backend weight. Keeps the smooth-WRR sum of weights across
// MAX_BACKENDS backends far from int overflow.
#define MAX_BACKEND_WEIGHT 10000

// Parse a decimal integer in [min, max] that spans the whole string.
static int parse_bounded_int(const char *s, long min, long max, int *out)
{
  char *end = NULL;
  errno = 0;
  long v = strtol(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0' || v < min || v > max)
  {
    return -1;
  }
  *out = (int)v;
  return 0;
}

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
  // Copied whole: a fixed buffer would silently cut a long list mid-entry and
  // register a truncated (wrong) port for the last backend.
  char *buf = strdup(spec);
  if (!buf)
  {
    perror("strdup");
    return 0;
  }

  int added = 0;
  char *saveptr = NULL;
  for (char *tok = strtok_r(buf, ",", &saveptr); tok != NULL;
       tok = strtok_r(NULL, ",", &saveptr))
  {
    while (*tok == ' ' || *tok == '\t')
    {
      tok++;
    }

    // An IPv6 literal contains colons itself, so it must be bracketed:
    // "[::1]:8080:2".
    const char *host = tok;
    char *colon = NULL;
    if (*tok == '[')
    {
      char *rbracket = strchr(tok, ']');
      if (!rbracket || rbracket[1] != ':')
      {
        fprintf(stderr, "skipping backend \"%s\": expected [ipv6]:port\n", tok);
        continue;
      }
      *rbracket = '\0';
      host = tok + 1;
      colon = rbracket + 1;
    }
    else
    {
      colon = strchr(tok, ':');
    }
    if (!colon)
    {
      fprintf(stderr, "skipping backend \"%s\": expected host:port\n", tok);
      continue;
    }
    *colon = '\0';
    char *port_str = colon + 1;

    // Optional ":weight" suffix.
    int weight = 1;
    char *colon2 = strchr(port_str, ':');
    if (colon2)
    {
      *colon2 = '\0';
      if (parse_bounded_int(colon2 + 1, 0, MAX_BACKEND_WEIGHT, &weight) != 0)
      {
        fprintf(stderr, "skipping backend \"%s\": invalid weight %s (0-%d)\n", host,
                colon2 + 1, MAX_BACKEND_WEIGHT);
        continue;
      }
    }

    int port = 0;
    if (parse_bounded_int(port_str, 1, 65535, &port) != 0)
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

  free(buf);
  return added;
}
