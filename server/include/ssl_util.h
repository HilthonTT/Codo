#ifndef SSL_UTIL_H
#define SSL_UTIL_H

#include <stdbool.h>

struct http_server;

int init_ssl(struct http_server *server, const char *cert_file, const char *key_file);
void cleanup_ssl(struct http_server *server);

// Turn TLS on when it is requested and both the certificate and the key exist,
// reporting the outcome. A missing pair or a failed init is not fatal -- the
// server keeps serving plaintext. Returns 1 when TLS is on, 0 when it was
// skipped, -1 when the certificates were present but init failed.
int init_ssl_if_available(struct http_server *server, bool enabled,
                          const char *cert_file, const char *key_file);

#endif
