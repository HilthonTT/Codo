#ifndef JWT_H
#define JWT_H

#include <stddef.h>
#include <stdint.h>

// Minimal HS256 JSON Web Tokens on top of the crypto framework
// (crypto_hmac_sha256 / secure_random_bytes). Tokens carry the claims
//   sub  username
//   uid  numeric user id
//   iat  issued-at (unix seconds)
//   exp  expiry (unix seconds)

typedef enum
{
  JWT_OK = 0,
  JWT_INVALID = -1, // malformed token or bad signature
  JWT_EXPIRED = -2, // signature valid but past exp
} jwt_result_t;

// Load the signing secret. Key:
//   JWT_SECRET   the HMAC secret (recommended; up to 64 bytes used)
// When unset, a random secret is generated and a warning printed -- tokens
// then stop verifying across a restart. Call once at startup, after
// init_crypto_framework(). Returns 0 on success.
int jwt_init(void);

// Mint a signed token for the given user, valid for ttl_seconds from now.
// username must already be validated (no characters needing JSON escaping).
// Returns 0 on success, -1 on error (including out too small).
int jwt_create(uint64_t user_id, const char *username, long ttl_seconds,
               char *out, size_t out_size);

// Verify a token's signature and expiry. On JWT_OK, *user_id and username are
// filled from the claims. Constant-time signature comparison; the alg header
// is never trusted (the HS256 HMAC is always recomputed), so alg-confusion
// ("none") tokens fail verification.
jwt_result_t jwt_verify(const char *token, uint64_t *user_id,
                        char *username, size_t username_size);

#endif
