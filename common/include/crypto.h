#ifndef CRYPTO_H
#define CRYPTO_H

#include <stddef.h>
#include <stdint.h>

// Cryptographic primitives used by the auth stack: PBKDF2 password hashing
// (user_auth.c) and HMAC-SHA256 token signing (jwt.c), plus the mlock'd
// arena that keeps derived key material out of swap.
//
// init_crypto_framework() must run before any other call here; it brings up
// the secure arena and the RNG lock. Both are process-wide singletons.

// Size of the mlock'd secure arena, in bytes.
#define SECURE_MEMORY_SIZE (1024 * 1024)

int init_crypto_framework(void);
void cleanup_crypto_framework(void);

// Allocate from the mlock'd arena, so the bytes are never paged to disk.
// Returns NULL when the arena is full. secure_free() zeroes before releasing,
// and needs the same size that was allocated.
void *secure_malloc(size_t size);
void secure_free(void *ptr, size_t size);

// Fill buffer with cryptographically secure random bytes. Returns 0 on
// success, -1 if every entropy source failed.
int secure_random_bytes(uint8_t *buffer, size_t size);

// PBKDF2-HMAC-SHA256. On success *derived_key points at key_len bytes from
// the secure arena, which the caller releases with secure_free().
int pbkdf2_derive_key(const char *password, size_t password_len,
                      const uint8_t *salt, size_t salt_len,
                      int iterations, size_t key_len,
                      uint8_t **derived_key);

// HMAC-SHA256 over data. Returns 0 on success.
int crypto_hmac_sha256(const uint8_t *key, size_t key_size,
                       const uint8_t *data, size_t data_size,
                       uint8_t hmac[32]);

#endif
