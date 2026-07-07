#define _GNU_SOURCE

#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "connection.h"
#include "http_protocol.h"
#include "http_types.h"
#include "websocket.h"

void generate_websocket_accept_key(const char *key, char *accept_key)
{
  if (!key || !accept_key)
  {
    return;
  }

  static const char *guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  char concatenated[256];
  snprintf(concatenated, sizeof(concatenated), "%s%s", key, guid);

  unsigned char sha1[SHA_DIGEST_LENGTH];
  SHA1((const unsigned char *)concatenated, strlen(concatenated), sha1);

  BIO *b64 = BIO_new(BIO_f_base64());
  BIO *mem = BIO_new(BIO_s_mem());
  if (!b64 || !mem)
  {
    if (b64)
      BIO_free(b64);
    if (mem)
      BIO_free(mem);
    accept_key[0] = '\0';
    return;
  }
  b64 = BIO_push(b64, mem);
  BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
  BIO_write(b64, sha1, SHA_DIGEST_LENGTH);
  BIO_flush(b64);

  BUF_MEM *bptr = NULL;
  BIO_get_mem_ptr(b64, &bptr);
  size_t copy_len = 0;
  if (bptr)
  {
    copy_len = bptr->length < 63 ? bptr->length : 63;
    memcpy(accept_key, bptr->data, copy_len);
  }
  accept_key[copy_len] = '\0';

  BIO_free_all(b64);
}

int handle_websocket_upgrade(connection_t *conn, http_request_t *request)
{
  if (!conn || !request)
  {
    return -1;
  }

  // WebSocket framing (RFC 6455) is not implemented. Returning 101 here and
  // then reverting to HTTP parsing on the same stream is a request-smuggling /
  // mis-framing hazard, so we deliberately do NOT accept the upgrade: reply
  // 501 and let the connection be torn down. No connection is ever left in the
  // ambiguous post-101 state, and websocket_handshake_complete stays false.
  return send_error_response(conn, HTTP_NOT_IMPLEMENTED, "WebSocket not supported");
}

int handle_websocket_frame(connection_t *conn, const char *data, size_t length)
{
  // Minimal stub: this implementation does not yet parse WebSocket frames.
  // It exists so the symbol is defined and callers compile.
  (void)conn;
  (void)data;
  (void)length;
  return 0;
}
