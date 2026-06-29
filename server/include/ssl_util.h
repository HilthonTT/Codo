#ifndef SSL_UTIL_H
#define SSL_UTIL_H

struct http_server;

int init_ssl(struct http_server *server, const char *cert_file, const char *key_file);
void cleanup_ssl(struct http_server *server);

#endif
