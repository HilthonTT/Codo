#ifndef ENV_H
#define ENV_H

#include <stdbool.h>

int load_env(const char *path);
const char *env_str(const char *key, const char *def);
int env_int(const char *key, int def);
bool env_bool(const char *key, bool def);

#endif