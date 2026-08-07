#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "compression.h"

int gzip_compress_buffer(const char *input, size_t input_len,
                         unsigned char **out, size_t *out_len)
{
  if (!out || !out_len || (!input && input_len > 0))
  {
    return -1;
  }
  *out = NULL;
  *out_len = 0;

  z_stream strm;
  memset(&strm, 0, sizeof(strm));
  // 15 | 16 selects the gzip wrapper (rather than raw deflate) so the output is
  // valid for a "Content-Encoding: gzip" response.
  if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                   15 | 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
  {
    return -1;
  }

  // deflateBound gives a worst-case size for a single-shot Z_FINISH pass, so one
  // malloc is enough -- no grow-and-retry loop.
  uLong bound = deflateBound(&strm, (uLong)input_len);
  unsigned char *buf = malloc(bound > 0 ? bound : 1);
  if (!buf)
  {
    deflateEnd(&strm);
    return -1;
  }

  strm.next_in = (Bytef *)input;
  strm.avail_in = (uInt)input_len;
  strm.next_out = buf;
  strm.avail_out = (uInt)bound;

  int rc = deflate(&strm, Z_FINISH);
  if (rc != Z_STREAM_END)
  {
    // Z_OK here means the bound was somehow insufficient; treat any non-final
    // return as a failure and let the caller fall back to the uncompressed body.
    deflateEnd(&strm);
    free(buf);
    return -1;
  }

  *out_len = (size_t)(bound - strm.avail_out);
  *out = buf;
  deflateEnd(&strm);
  return 0;
}
