#ifndef SERVER_CONFIG_H
#define SERVER_CONFIG_H

#include <stdbool.h>

// Runtime configuration of the HTTP server binary. Compile-time limits (thread
// counts, buffer sizes, connection caps) live in common/include/config.h.
//
// The string fields point into the environment / the .env table / argv, all of
// which outlive main(), so nothing here needs freeing.
typedef struct
{
  int port;
  const char *document_root;
  bool ssl_enabled;
  const char *ssl_cert_file;
  const char *ssl_key_file;
  const char *db_file;
  const char *wal_file;
  const char *cors_allow_origin;
  // Trust a PROXY protocol v1 header from the peer (TRUST_PROXY_PROTOCOL).
  // Only enable when the listener is reachable solely by a trusted balancer:
  // the header lets its sender claim any client address.
  bool trust_proxy_protocol;
} server_config_t;

// Load the configuration in precedence order: .env file (path from ENV_FILE,
// default ".env"), then the real environment, then the command line
// (argv[1] = port, argv[2] = document root).
//
// Returns 0 on success, -1 if the result is unusable (an out-of-range port); a
// missing .env is not an error, it just leaves the defaults in place.
int server_config_load(server_config_t *config, int argc, char *argv[]);

#endif
