#include <stdio.h>
#include <stdlib.h>

#include "env.h"
#include "server_config.h"

int server_config_load(server_config_t *config, int argc, char *argv[])
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
  config->port = env_int("PORT", 8080);
  config->document_root = env_str("DOCUMENT_ROOT", "/var/www/html");
  config->ssl_enabled = env_bool("SSL_ENABLED", true);
  config->ssl_cert_file = env_str("SSL_CERT_FILE", "server.crt");
  config->ssl_key_file = env_str("SSL_KEY_FILE", "server.key");
  config->db_file = env_str("DB_FILE", "codo.db");
  config->wal_file = env_str("WAL_FILE", "codo.wal");
  config->cors_allow_origin = env_str("CORS_ALLOW_ORIGIN", "*");
  config->trust_proxy_protocol = env_bool("TRUST_PROXY_PROTOCOL", false);

  // Command line arguments still override everything.
  if (argc > 1)
  {
    config->port = atoi(argv[1]);
  }
  if (argc > 2)
  {
    config->document_root = argv[2];
  }

  if (config->port <= 0 || config->port > 65535)
  {
    fprintf(stderr, "Invalid port: %d\n", config->port);
    return -1;
  }

  return 0;
}
