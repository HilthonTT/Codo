#define _GNU_SOURCE

#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "connection.h"
#include "http_protocol.h"
#include "http_types.h"
#include "websocket.h"

// WebSocket opcodes (RFC 6455 section 5.2).
#define WS_OP_CONTINUATION 0x0
#define WS_OP_TEXT 0x1
#define WS_OP_BINARY 0x2
#define WS_OP_CLOSE 0x8
#define WS_OP_PING 0x9
#define WS_OP_PONG 0xA

// Close status codes we emit (RFC 6455 section 7.4.1).
#define WS_CLOSE_NORMAL 1000
#define WS_CLOSE_PROTOCOL_ERROR 1002
#define WS_CLOSE_UNSUPPORTED_DATA 1003
#define WS_CLOSE_MESSAGE_TOO_BIG 1009

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

  // The client must present a Sec-WebSocket-Key; without it we cannot compute a
  // valid accept token, so reject the handshake as a normal HTTP error.
  if (request->websocket_key[0] == '\0')
  {
    return send_error_response(conn, HTTP_BAD_REQUEST, "Missing Sec-WebSocket-Key");
  }

  char accept_key[64];
  generate_websocket_accept_key(request->websocket_key, accept_key);
  if (accept_key[0] == '\0')
  {
    return send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR, "Handshake failed");
  }

  // Stage the 101 response directly: it carries no body and needs the exact
  // Upgrade/Connection/Accept header set, so send_http_response (which always
  // emits Content-Length and a Connection header) is not a good fit here.
  int n = snprintf(conn->write_buffer, sizeof(conn->write_buffer),
                   "HTTP/1.1 101 Switching Protocols\r\n"
                   "Upgrade: websocket\r\n"
                   "Connection: Upgrade\r\n"
                   "Sec-WebSocket-Accept: %s\r\n"
                   "\r\n",
                   accept_key);
  if (n < 0 || (size_t)n >= sizeof(conn->write_buffer))
  {
    return send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR, "Handshake too large");
  }

  conn->write_buffer_pos = 0;
  conn->write_buffer_size = (size_t)n;
  conn->state = CONN_STATE_WRITING_RESPONSE;
  conn->websocket_handshake_complete = true;
  conn->websocket_closing = false;

  // Set purely so the logging middleware reports 101 rather than a stale status;
  // the response struct itself is not serialized for the handshake.
  conn->response.status = HTTP_SWITCHING_PROTOCOLS;
  return 0;
}

// Append an unmasked server frame (FIN set) with the given opcode and payload to
// the connection's write buffer. Returns 0, or -1 if it would not fit.
static int ws_append_frame(connection_t *conn, unsigned char opcode,
                           const unsigned char *payload, size_t len)
{
  unsigned char header[10];
  size_t hlen;

  header[0] = (unsigned char)(0x80 | opcode); // FIN=1, no RSV bits
  if (len < 126)
  {
    header[1] = (unsigned char)len;
    hlen = 2;
  }
  else if (len <= 0xFFFF)
  {
    header[1] = 126;
    header[2] = (unsigned char)((len >> 8) & 0xFF);
    header[3] = (unsigned char)(len & 0xFF);
    hlen = 4;
  }
  else
  {
    header[1] = 127;
    for (int i = 0; i < 8; i++)
    {
      header[2 + i] = (unsigned char)((len >> (56 - 8 * i)) & 0xFF);
    }
    hlen = 10;
  }

  if (conn->write_buffer_size + hlen + len > sizeof(conn->write_buffer))
  {
    return -1; // no room; caller stops producing output
  }

  memcpy(conn->write_buffer + conn->write_buffer_size, header, hlen);
  conn->write_buffer_size += hlen;
  if (len > 0)
  {
    memcpy(conn->write_buffer + conn->write_buffer_size, payload, len);
    conn->write_buffer_size += len;
  }
  return 0;
}

// Append a Close frame carrying a 2-byte status code.
static void ws_append_close(connection_t *conn, uint16_t code)
{
  unsigned char payload[2] = {(unsigned char)((code >> 8) & 0xFF),
                              (unsigned char)(code & 0xFF)};
  ws_append_frame(conn, WS_OP_CLOSE, payload, sizeof(payload));
}

int ws_process_frames(connection_t *conn, bool *should_close)
{
  *should_close = false;

  unsigned char *buf = (unsigned char *)conn->read_buffer;
  size_t avail = conn->read_buffer_pos;
  size_t off = 0;

  while (off < avail)
  {
    // Minimum frame header is 2 bytes.
    if (avail - off < 2)
    {
      break;
    }

    unsigned char b0 = buf[off];
    unsigned char b1 = buf[off + 1];
    bool fin = (b0 & 0x80) != 0;
    unsigned char rsv = b0 & 0x70;
    unsigned char opcode = b0 & 0x0F;
    bool masked = (b1 & 0x80) != 0;
    uint64_t len = b1 & 0x7F;
    size_t header = 2;

    // We negotiated no extensions, so any reserved bit set is a protocol error.
    if (rsv != 0)
    {
      ws_append_close(conn, WS_CLOSE_PROTOCOL_ERROR);
      *should_close = true;
      break;
    }

    if (len == 126)
    {
      if (avail - off < 4)
      {
        break; // need the 16-bit extended length
      }
      len = ((uint64_t)buf[off + 2] << 8) | buf[off + 3];
      header = 4;
    }
    else if (len == 127)
    {
      if (avail - off < 10)
      {
        break; // need the 64-bit extended length
      }
      len = 0;
      for (int i = 0; i < 8; i++)
      {
        len = (len << 8) | buf[off + 2 + i];
      }
      header = 10;
    }

    // Client-to-server frames MUST be masked (RFC 6455 section 5.1).
    if (!masked)
    {
      ws_append_close(conn, WS_CLOSE_PROTOCOL_ERROR);
      *should_close = true;
      break;
    }

    // A frame whose payload cannot fit in our buffer can never be assembled.
    if (len > sizeof(conn->read_buffer))
    {
      ws_append_close(conn, WS_CLOSE_MESSAGE_TOO_BIG);
      *should_close = true;
      break;
    }

    size_t mask_off = off + header;
    if (avail - mask_off < 4)
    {
      break; // masking key not fully received yet
    }
    const unsigned char *mask = buf + mask_off;
    size_t payload_off = mask_off + 4;
    if (avail - payload_off < len)
    {
      break; // full payload not yet received
    }

    // Unmask the payload in place; we own these bytes until we consume them.
    unsigned char *payload = buf + payload_off;
    for (uint64_t i = 0; i < len; i++)
    {
      payload[i] ^= mask[i % 4];
    }

    size_t frame_end = payload_off + (size_t)len;

    if (opcode == WS_OP_TEXT || opcode == WS_OP_BINARY)
    {
      // This echo server does not reassemble fragmented messages; a data frame
      // must be self-contained (FIN set). Reject fragmentation cleanly rather
      // than emit a bogus reply.
      if (!fin)
      {
        ws_append_close(conn, WS_CLOSE_UNSUPPORTED_DATA);
        *should_close = true;
        off = frame_end;
        break;
      }
      if (ws_append_frame(conn, opcode, payload, (size_t)len) != 0)
      {
        ws_append_close(conn, WS_CLOSE_MESSAGE_TOO_BIG);
        *should_close = true;
        off = frame_end;
        break;
      }
    }
    else if (opcode == WS_OP_PING)
    {
      // Control frames are never fragmented and carry <=125 payload bytes.
      if (!fin || len > 125)
      {
        ws_append_close(conn, WS_CLOSE_PROTOCOL_ERROR);
        *should_close = true;
        off = frame_end;
        break;
      }
      ws_append_frame(conn, WS_OP_PONG, payload, (size_t)len);
    }
    else if (opcode == WS_OP_PONG)
    {
      // Unsolicited/echoed pong: nothing to do.
    }
    else if (opcode == WS_OP_CLOSE)
    {
      // Echo the peer's close code (default to normal) and shut down.
      uint16_t code = WS_CLOSE_NORMAL;
      if (len >= 2)
      {
        code = (uint16_t)((payload[0] << 8) | payload[1]);
      }
      ws_append_close(conn, code);
      *should_close = true;
      off = frame_end;
      break;
    }
    else
    {
      // Reserved / unknown opcode (including a stray continuation frame).
      ws_append_close(conn, WS_CLOSE_PROTOCOL_ERROR);
      *should_close = true;
      off = frame_end;
      break;
    }

    off = frame_end;
  }

  // Drop the consumed frames, keeping any trailing partial frame at the front.
  size_t remaining = avail - off;
  if (off > 0 && remaining > 0)
  {
    memmove(conn->read_buffer, conn->read_buffer + off, remaining);
  }
  conn->read_buffer_pos = remaining;
  return 0;
}
