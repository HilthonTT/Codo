#define _GNU_SOURCE

#include <string.h>
#include <zlib.h>

#include "compression.h"
#include "connection.h"

int init_gzip_compression(connection_t *conn)
{
  if (!conn)
  {
    return -1;
  }
  if (conn->gzip_initialized)
  {
    return 0;
  }
  memset(&conn->gzip_stream, 0, sizeof(conn->gzip_stream));
  // 15 | 16 enables the gzip wrapper rather than raw deflate.
  if (deflateInit2(&conn->gzip_stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                   15 | 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
  {
    return -1;
  }
  conn->gzip_initialized = true;
  return 0;
}

void compress_response_body(connection_t *conn, const char *input, size_t input_size)
{
  // Minimal stub: feeding input through the deflate stream and assembling a
  // gzip-compressed response body is not yet wired into the response path.
  // We touch the parameters so the build stays warning-free.
  (void)conn;
  (void)input;
  (void)input_size;
}

void cleanup_gzip_compression(connection_t *conn)
{
  if (!conn || !conn->gzip_initialized)
  {
    return;
  }
  deflateEnd(&conn->gzip_stream);
  conn->gzip_initialized = false;
}
