#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>

#include "env.h"

int load_env(const char *path)
{
  FILE *f = fopen(path, "r");
  if (!f)
  {
    return -1;
  }

  char line[1024];
  while (fgets(line, sizeof line, f))
  {
    // strip trailing newline / CR
    line[strcspn(line, "\r\n")] = '\0';

    // skip blanks and comments
    char *p = line;
    while (*p == ' ' || *p == '\t')
    {
      p++;
    }
    if (*p == '\0' || *p == '#')
    {
      continue;
    }

    // tolerate "export KEY=value" lines
    if (strncmp(p, "export ", 7) == 0)
    {
      p += 7;
      while (*p == ' ' || *p == '\t')
      {
        p++;
      }
    }

    // split on first '='
    char *eq = strchr(p, '=');
    if (!eq)
    {
      continue;
    }
    *eq = '\0';

    const char *key = p;
    char *val = eq + 1;

    // trim trailing whitespace on key, leading on value
    char *kend = eq;
    while (kend > p && (kend[-1] == ' ' || kend[-1] == '\t'))
    {
      *--kend = '\0';
    }
    while (*val == ' ' || *val == '\t')
    {
      val++;
    }

    // strip optional surrounding quotes on the value
    size_t len = strlen(val);
    if (len >= 2 && (val[0] == '"' || val[0] == '\'') && val[len - 1] == val[0])
    {
      val[len - 1] = '\0';
      val++;
    }

    setenv(key, val, 0); // 0 = don't overwrite real env vars
  }

  fclose(f);

  return 0;
}

const char *env_str(const char *key, const char *def)
{
  const char *v = getenv(key);
  return (v && *v) ? v : def;
}

int env_int(const char *key, int def)
{
  const char *v = getenv(key);
  if (!v || !*v)
  {
    return def;
  }

  errno = 0;
  char *end = NULL;
  long n = strtol(v, &end, 10);
  if (errno != 0 || end == v || *end != '\0')
  {
    fprintf(stderr, "warning: %s=\"%s\" is not a valid integer, using %d\n",
            key, v, def);
    return def;
  }
  // strtol parses a long; reject values that don't fit an int instead of
  // silently truncating (e.g. on LP64, values in (INT_MAX, LONG_MAX]).
  if (n < INT_MIN || n > INT_MAX)
  {
    fprintf(stderr, "warning: %s=\"%s\" is out of range for int, using %d\n",
            key, v, def);
    return def;
  }
  return (int)n;
}

bool env_bool(const char *key, bool def)
{
  const char *v = getenv(key);
  if (!v || !*v)
  {
    return def;
  }
  if (!strcasecmp(v, "1") || !strcasecmp(v, "true") ||
      !strcasecmp(v, "yes") || !strcasecmp(v, "on"))
  {
    return true;
  }
  if (!strcasecmp(v, "0") || !strcasecmp(v, "false") ||
      !strcasecmp(v, "no") || !strcasecmp(v, "off"))
  {
    return false;
  }
  fprintf(stderr, "warning: %s=\"%s\" is not a valid boolean, using %s\n",
          key, v, def ? "true" : "false");
  return def;
}
