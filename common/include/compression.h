#ifndef COMPRESSION_H
#define COMPRESSION_H

#include <stddef.h>

#include "connection.h"

int init_gzip_compression(connection_t *conn);
void cleanup_gzip_compression(connection_t *conn);

// One-shot gzip of a whole buffer. On success returns 0, points *out at a freshly
// malloc'd buffer holding the gzip-wrapped (Content-Encoding: gzip) bytes, and
// writes their length to *out_len; the caller owns and frees *out. Returns -1 on
// failure, leaving *out NULL. Suited to fully-buffered response bodies rather
// than streaming.
int gzip_compress_buffer(const char *input, size_t input_len,
                         unsigned char **out, size_t *out_len);

#endif
