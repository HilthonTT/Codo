#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "env.h"
#include "server_config.h"

static int is_safe_path_component(const char *value)
{
  if (!value || !*value)
  {
    return 0;
  }

  if (strstr(value, "..") || strchr(value, '/') || strchr(value, '\\'))
  {
    return 0;
  }

  return 1;
}

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

  if (!is_safe_path_component(config->db_file))
  {
    fprintf(stderr, "Invalid DB_FILE: %s\n", config->db_file ? config->db_file : "(null)");
    return -1;
  }

  if (!is_safe_path_component(config->wal_file))
  {
    fprintf(stderr, "Invalid WAL_FILE: %s\n", config->wal_file ? config->wal_file : "(null)");
    return -1;
  }

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
