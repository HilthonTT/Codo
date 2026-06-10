#ifndef OTA_COMPRESSION_H
#define OTA_COMPRESSION_H

#include <stddef.h>

#include "connection.h"

int init_gzip_compression(connection_t *conn);
void compress_response_body(connection_t *conn, const char *input, size_t input_size);
void cleanup_gzip_compression(connection_t *conn);

#endif
