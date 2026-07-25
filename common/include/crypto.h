#ifndef CRYPTO_H
#define CRYPTO_H

#define _POSIX_C_SOURCE 200112L

#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <stdbool.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <string.h>

#include <openssl/evp.h>
#include <openssl/aes.h>
#include <openssl/rsa.h>
#include <openssl/ec.h>
#include <openssl/ecdh.h>
#include <openssl/ecdsa.h>
#include <openssl/rand.h>
#include <openssl/kdf.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/pkcs12.h>
#include <openssl/err.h>

#define MAX_KEY_SIZE 4096
#define MAX_BLOCK_SIZE 64
#define MAX_IV_SIZE 16
#define MAX_TAG_SIZE 16
#define MAX_SALT_SIZE 32
#define MAX_KEYS 1000
#define SECURE_MEMORY_SIZE (1024 * 1024) // 1MB secure memory pool

// Encryption algorithms
typedef enum
{
  CRYPTO_AES_128_GCM,
  CRYPTO_AES_256_GCM,
  CRYPTO_AES_128_CBC,
  CRYPTO_AES_256_CBC,
  CRYPTO_CHACHA20_POLY1305,
  CRYPTO_RSA_2048,
  CRYPTO_RSA_4096,
  CRYPTO_ECDSA_P256,
  CRYPTO_ECDSA_P384,
  CRYPTO_ECDSA_P521,
  CRYPTO_ECDH_P256,
  CRYPTO_ECDH_P384,
} crypto_algorithm_t;

// Key types
typedef enum
{
  KEY_TYPE_SYMMETRIC,
  KEY_TYPE_RSA_PRIVATE,
  KEY_TYPE_EC_PRIVATE,
  KEY_TYPE_EC_PUBLIC,
} key_type_t;

// Secure key structure
typedef struct
{
  uint32_t key_id;
  key_type_t type;
  crypto_algorithm_t algorithm;
  size_t key_size;
  uint8_t *key_data;
  time_t creation_time;
  time_t expiration_time;
  uint32_t usage_count;
  uint32_t max_usage;
  bool revoked;
  pthread_mutex_t lock;
} secure_key_t;

// Cryptographic context
typedef struct
{
  EVP_PKEY *private_key;
  EVP_PKEY *public_key;
  X509 *certificate;
  crypto_algorithm_t algorithm;
  uint8_t *session_key;
  size_t session_key_size;
  uint8_t iv[MAX_IV_SIZE];
  uint8_t tag[MAX_TAG_SIZE];
  size_t tag_size;
} crypto_context_t;

// Secure memory pool
typedef struct
{
  void *memory_pool;
  size_t pool_size;
  size_t allocated;
  bool *allocation_map;
  size_t block_size;
  pthread_mutex_t pool_lock;
} secure_memory_pool_t;

// Key management system
typedef struct
{
  secure_key_t *keys[MAX_KEYS];
  uint32_t next_key_id;
  pthread_rwlock_t keys_lock;

  // Hardware security module interface
  struct
  {
    bool available;
    void *handle;
    int (*init)(void);
    int (*generate_key)(crypto_algorithm_t alg, uint8_t **key, size_t *key_size);
    int (*encrypt)(const uint8_t *key, size_t key_size, const uint8_t *plain, size_t plain_size, uint8_t **cipher, size_t *cipher_size);
    int (*decrypt)(const uint8_t *key, size_t key_size, const uint8_t *cipher, size_t cipher_size, uint8_t **plain, size_t *plain_size);
    void (*cleanup)(void);
  } hsm;

  // Secure random number generator
  struct
  {
    bool initialized;
    pthread_mutex_t rng_lock;
    SHA256_CTX entropy_ctx;
    uint8_t entropy_pool[256];
    size_t entropy_count;
  } rng;

  secure_memory_pool_t secure_memory;
} key_management_system_t;

int init_crypto_framework(void);
void cleanup_crypto_framework(void);
int init_secure_memory_pool(void);
void cleanup_secure_memory_pool(void);
void *secure_malloc(size_t size);
void secure_free(void *ptr, size_t size);
int secure_random_bytes(uint8_t *buffer, size_t size);
// Secure random number generation
int init_hardware_rng(void);

int generate_aes_key(crypto_algorithm_t algorithm, uint8_t **key, size_t *key_size);
EVP_PKEY *generate_rsa_keypair(int key_size);
EVP_PKEY *generate_ec_keypair(int curve_nid);
secure_key_t *create_secure_key(key_type_t type, crypto_algorithm_t algorithm);
void destroy_secure_key(secure_key_t *key);
int store_key(secure_key_t *key);
secure_key_t *find_key(uint32_t key_id);

int aes_gcm_encrypt(const uint8_t *key, size_t key_size,
                    const uint8_t *iv, size_t iv_size,
                    const uint8_t *plaintext, size_t plaintext_size,
                    const uint8_t *aad, size_t aad_size,
                    uint8_t **ciphertext, size_t *ciphertext_size,
                    uint8_t *tag, size_t *tag_size);
int aes_gcm_decrypt(const uint8_t *key, size_t key_size,
                    const uint8_t *iv, size_t iv_size,
                    const uint8_t *ciphertext, size_t ciphertext_size,
                    const uint8_t *aad, size_t aad_size,
                    const uint8_t *tag, size_t tag_size,
                    uint8_t **plaintext, size_t *plaintext_size);

int rsa_sign(EVP_PKEY *private_key, const uint8_t *data, size_t data_size, uint8_t **signature, size_t *signature_size);
int rsa_verify(EVP_PKEY *public_key, const uint8_t *data, size_t data_size,
               const uint8_t *signature, size_t signature_size);

int pbkdf2_derive_key(const char *password, size_t password_len,
                      const uint8_t *salt, size_t salt_len,
                      int iterations, size_t key_len,
                      uint8_t **derived_key);

int hkdf_derive_key(const uint8_t *ikm, size_t ikm_len,
                    const uint8_t *salt, size_t salt_len,
                    const uint8_t *info, size_t info_len,
                    size_t key_len, uint8_t **derived_key);

uint32_t crypto_generate_key(crypto_algorithm_t algorithm);

int crypto_encrypt(uint32_t key_id, const uint8_t *plaintext, size_t plaintext_size,
                   const uint8_t *aad, size_t aad_size,
                   uint8_t **ciphertext, size_t *ciphertext_size,
                   uint8_t *iv, size_t *iv_size,
                   uint8_t *tag, size_t *tag_size);

int crypto_decrypt(uint32_t key_id, const uint8_t *ciphertext, size_t ciphertext_size,
                   const uint8_t *aad, size_t aad_size,
                   const uint8_t *iv, size_t iv_size,
                   const uint8_t *tag, size_t tag_size,
                   uint8_t **plaintext, size_t *plaintext_size);

int crypto_sign(uint32_t key_id, const uint8_t *data, size_t data_size,
                uint8_t **signature, size_t *signature_size);

int crypto_verify(uint32_t key_id, const uint8_t *data, size_t data_size,
                  const uint8_t *signature, size_t signature_size);

void crypto_revoke_key(uint32_t key_id);

int crypto_hash_sha256(const uint8_t *data, size_t data_size, uint8_t hash[32]);

int crypto_hmac_sha256(const uint8_t *key, size_t key_size,
                       const uint8_t *data, size_t data_size,
                       uint8_t hmac[32]);

#endif