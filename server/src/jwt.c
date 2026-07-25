#define _GNU_SOURCE

// crypto.h sets _POSIX_C_SOURCE itself, so it must come before any system
// header has locked feature macros in.
#include "crypto.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "env.h"
#include "json_util.h"
#include "jwt.h"

// HMAC secret, loaded once by jwt_init(). 64 bytes is the SHA-256 block size;
// a longer secret would be hashed down by HMAC anyway.
static uint8_t g_secret[64];
static size_t g_secret_len = 0;

int jwt_init(void)
{
  const char *env_secret = env_str("JWT_SECRET", "");
  if (env_secret && *env_secret)
  {
    size_t len = strlen(env_secret);
    if (len > sizeof(g_secret))
    {
      len = sizeof(g_secret);
    }
    memcpy(g_secret, env_secret, len);
    g_secret_len = len;
    return 0;
  }

  if (secure_random_bytes(g_secret, 32) != 0)
  {
    return -1;
  }
  g_secret_len = 32;
  fprintf(stderr,
          "JWT_SECRET not set; using a random secret -- issued tokens will "
          "not survive a server restart\n");
  return 0;
}

static const char B64URL[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

// Unpadded base64url (RFC 7515). Returns the encoded length, or -1 if out is
// too small. out is NUL-terminated.
static int b64url_encode(const uint8_t *in, size_t in_len,
                         char *out, size_t out_size)
{
  size_t o = 0;
  size_t i = 0;

  while (i + 3 <= in_len)
  {
    uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2];
    if (o + 4 >= out_size)
    {
      return -1;
    }
    out[o++] = B64URL[(v >> 18) & 63];
    out[o++] = B64URL[(v >> 12) & 63];
    out[o++] = B64URL[(v >> 6) & 63];
    out[o++] = B64URL[v & 63];
    i += 3;
  }

  size_t rem = in_len - i;
  if (rem == 1)
  {
    uint32_t v = (uint32_t)in[i] << 16;
    if (o + 2 >= out_size)
    {
      return -1;
    }
    out[o++] = B64URL[(v >> 18) & 63];
    out[o++] = B64URL[(v >> 12) & 63];
  }
  else if (rem == 2)
  {
    uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8;
    if (o + 3 >= out_size)
    {
      return -1;
    }
    out[o++] = B64URL[(v >> 18) & 63];
    out[o++] = B64URL[(v >> 12) & 63];
    out[o++] = B64URL[(v >> 6) & 63];
  }

  if (o >= out_size)
  {
    return -1;
  }
  out[o] = '\0';
  return (int)o;
}

static int b64url_value(char c)
{
  if (c >= 'A' && c <= 'Z')
  {
    return c - 'A';
  }
  if (c >= 'a' && c <= 'z')
  {
    return c - 'a' + 26;
  }
  if (c >= '0' && c <= '9')
  {
    return c - '0' + 52;
  }
  if (c == '-')
  {
    return 62;
  }
  if (c == '_')
  {
    return 63;
  }
  return -1;
}

// Decode unpadded base64url. Returns the decoded length, or -1 on a bad
// character, impossible length, or overflow of out.
static int b64url_decode(const char *in, size_t in_len,
                         uint8_t *out, size_t out_size)
{
  if (in_len % 4 == 1)
  {
    return -1;
  }

  size_t o = 0;
  uint32_t acc = 0;
  int bits = 0;
  for (size_t i = 0; i < in_len; i++)
  {
    int v = b64url_value(in[i]);
    if (v < 0)
    {
      return -1;
    }
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8)
    {
      bits -= 8;
      if (o >= out_size)
      {
        return -1;
      }
      out[o++] = (uint8_t)((acc >> bits) & 0xff);
    }
  }
  return (int)o;
}

int jwt_create(uint64_t user_id, const char *username, long ttl_seconds,
               char *out, size_t out_size)
{
  if (g_secret_len == 0)
  {
    return -1; // jwt_init not called
  }

  time_t now = time(NULL);
  char payload[256];
  int payload_len = snprintf(payload, sizeof(payload),
                             "{\"sub\":\"%s\",\"uid\":%llu,\"iat\":%lld,\"exp\":%lld}",
                             username, (unsigned long long)user_id,
                             (long long)now, (long long)(now + ttl_seconds));
  if (payload_len < 0 || (size_t)payload_len >= sizeof(payload))
  {
    return -1;
  }

  static const char header[] = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
  char signing_input[512];
  int header_b64 = b64url_encode((const uint8_t *)header, sizeof(header) - 1,
                                 signing_input, sizeof(signing_input));
  if (header_b64 < 0 || (size_t)header_b64 + 1 >= sizeof(signing_input))
  {
    return -1;
  }
  signing_input[header_b64] = '.';

  int payload_b64 = b64url_encode((const uint8_t *)payload, (size_t)payload_len,
                                  signing_input + header_b64 + 1,
                                  sizeof(signing_input) - header_b64 - 1);
  if (payload_b64 < 0)
  {
    return -1;
  }
  size_t signed_len = (size_t)header_b64 + 1 + (size_t)payload_b64;

  uint8_t mac[32];
  if (crypto_hmac_sha256(g_secret, g_secret_len,
                         (const uint8_t *)signing_input, signed_len, mac) != 0)
  {
    return -1;
  }

  char sig_b64[64];
  if (b64url_encode(mac, sizeof(mac), sig_b64, sizeof(sig_b64)) < 0)
  {
    return -1;
  }

  int n = snprintf(out, out_size, "%s.%s", signing_input, sig_b64);
  if (n < 0 || (size_t)n >= out_size)
  {
    return -1;
  }
  return 0;
}

jwt_result_t jwt_verify(const char *token, uint64_t *user_id,
                        char *username, size_t username_size)
{
  if (g_secret_len == 0 || !token)
  {
    return JWT_INVALID;
  }

  const char *dot1 = strchr(token, '.');
  if (!dot1)
  {
    return JWT_INVALID;
  }
  const char *dot2 = strchr(dot1 + 1, '.');
  if (!dot2 || strchr(dot2 + 1, '.'))
  {
    return JWT_INVALID;
  }

  // The signature is always recomputed as HS256 with our secret -- the token's
  // alg header is never consulted, so downgrade tricks don't apply.
  uint8_t expected[32];
  if (crypto_hmac_sha256(g_secret, g_secret_len, (const uint8_t *)token,
                         (size_t)(dot2 - token), expected) != 0)
  {
    return JWT_INVALID;
  }

  uint8_t presented[sizeof(expected)];
  if (b64url_decode(dot2 + 1, strlen(dot2 + 1),
                    presented, sizeof(presented)) != (int)sizeof(expected))
  {
    return JWT_INVALID;
  }

  unsigned char diff = 0;
  for (size_t i = 0; i < sizeof(expected); i++)
  {
    diff |= (unsigned char)(expected[i] ^ presented[i]);
  }
  if (diff != 0)
  {
    return JWT_INVALID;
  }

  // Signature is good; the payload is trusted from here on.
  char payload[512];
  int payload_len = b64url_decode(dot1 + 1, (size_t)(dot2 - dot1 - 1),
                                  (uint8_t *)payload, sizeof(payload));
  if (payload_len < 0)
  {
    return JWT_INVALID;
  }

  uint64_t uid = 0;
  uint64_t exp = 0;
  if (!json_get_uint64(payload, (size_t)payload_len, "uid", &uid) ||
      !json_get_uint64(payload, (size_t)payload_len, "exp", &exp) ||
      !json_get_string(payload, (size_t)payload_len, "sub",
                       username, username_size))
  {
    return JWT_INVALID;
  }

  if ((uint64_t)time(NULL) >= exp)
  {
    return JWT_EXPIRED;
  }

  *user_id = uid;
  return JWT_OK;
}
