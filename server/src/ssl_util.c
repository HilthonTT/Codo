#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "server.h"
#include "ssl_util.h"

int init_ssl(http_server_t *server, const char *cert_file, const char *key_file) {
  if (!server || !cert_file || !key_file) {
    return -1;
  }

  // OpenSSL 1.1.0+ initializes itself on first use; the old SSL_library_init /
  // OpenSSL_add_all_algorithms calls are no-ops.
  server->ssl_ctx = SSL_CTX_new(TLS_server_method());
  if (!server->ssl_ctx) {
    ERR_print_errors_fp(stderr);
    return -1;
  }

  SSL_CTX_set_min_proto_version(server->ssl_ctx, TLS1_2_VERSION);

  if (SSL_CTX_use_certificate_file(server->ssl_ctx, cert_file, SSL_FILETYPE_PEM) <= 0) {
    ERR_print_errors_fp(stderr);
    SSL_CTX_free(server->ssl_ctx);
    server->ssl_ctx = NULL;
    return -1;
  }

  if (SSL_CTX_use_PrivateKey_file(server->ssl_ctx, key_file, SSL_FILETYPE_PEM) <= 0) {
    ERR_print_errors_fp(stderr);
    SSL_CTX_free(server->ssl_ctx);
    server->ssl_ctx = NULL;
    return -1;
  }

  if (!SSL_CTX_check_private_key(server->ssl_ctx)) {
    fprintf(stderr, "SSL: private key does not match certificate\n");
    SSL_CTX_free(server->ssl_ctx);
    server->ssl_ctx = NULL;
    return -1;
  }

  server->ssl_enabled = true;
  server->ssl_cert_file = strdup(cert_file);
  server->ssl_key_file = strdup(key_file);
  return 0;
}

int init_ssl_if_available(http_server_t *server, bool enabled,
                          const char *cert_file, const char *key_file) {
  if (!enabled || !cert_file || !key_file) {
    return 0;
  }
  if (access(cert_file, F_OK) != 0 || access(key_file, F_OK) != 0) {
    return 0;
  }

  if (init_ssl(server, cert_file, key_file) != 0) {
    fprintf(stderr, "SSL init failed, continuing without SSL\n");
    return -1;
  }

  printf("SSL enabled (%s / %s)\n", cert_file, key_file);
  return 1;
}

void cleanup_ssl(http_server_t *server) {
  if (!server) {
    return;
  }
  if (server->ssl_ctx) {
    SSL_CTX_free(server->ssl_ctx);
    server->ssl_ctx = NULL;
  }
  free(server->ssl_cert_file);
  free(server->ssl_key_file);
  server->ssl_cert_file = NULL;
  server->ssl_key_file = NULL;
  server->ssl_enabled = false;
  EVP_cleanup();
  ERR_free_strings();
}
