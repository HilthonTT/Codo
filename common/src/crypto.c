#include "crypto.h"

static key_management_system_t kms = {0};

// Key generation functions
int generate_aes_key(crypto_algorithm_t algorithm, uint8_t **key, size_t *key_size)
{
  size_t size;

  switch (algorithm)
  {
  case CRYPTO_AES_128_GCM:
  case CRYPTO_AES_128_CBC:
    size = 16;
    break;
  case CRYPTO_AES_256_GCM:
  case CRYPTO_AES_256_CBC:
    size = 32;
    break;
  default:
    return -1;
  }

  *key = secure_malloc(size);
  if (!*key)
  {
    return -1;
  }

  if (secure_random_bytes(*key, size) != 0)
  {
    secure_free(*key, size);
    return -1;
  }

  *key_size = size;
  return 0;
}

EVP_PKEY *generate_rsa_keypair(int key_size)
{
  EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
  if (!ctx)
  {
    return NULL;
  }

  if (EVP_PKEY_keygen_init(ctx) <= 0)
  {
    EVP_PKEY_CTX_free(ctx);
    return NULL;
  }

  if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, key_size) <= 0)
  {
    EVP_PKEY_CTX_free(ctx);
    return NULL;
  }

  EVP_PKEY *pkey = NULL;
  if (EVP_PKEY_keygen(ctx, &pkey) <= 0)
  {
    EVP_PKEY_CTX_free(ctx);
    return NULL;
  }

  EVP_PKEY_CTX_free(ctx);
  return pkey;
}

EVP_PKEY *generate_ec_keypair(int curve_nid)
{
  EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
  if (!ctx)
  {
    return NULL;
  }

  if (EVP_PKEY_keygen_init(ctx) <= 0)
  {
    EVP_PKEY_CTX_free(ctx);
    return NULL;
  }

  if (EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, curve_nid) <= 0)
  {
    EVP_PKEY_CTX_free(ctx);
    return NULL;
  }

  EVP_PKEY *pkey = NULL;
  if (EVP_PKEY_keygen(ctx, &pkey) <= 0)
  {
    EVP_PKEY_CTX_free(ctx);
    return NULL;
  }

  EVP_PKEY_CTX_free(ctx);
  return pkey;
}

// Key management functions
secure_key_t *create_secure_key(key_type_t type, crypto_algorithm_t algorithm)
{
  secure_key_t *key = secure_malloc(sizeof(secure_key_t));
  if (!key)
  {
    return NULL;
  }

  key->key_id = ++kms.next_key_id;
  key->type = type;
  key->algorithm = algorithm;
  key->creation_time = time(NULL);
  key->expiration_time = key->creation_time + (365 * 24 * 3600); // 1 year
  key->usage_count = 0;
  key->max_usage = 1000000; // Default max usage
  key->revoked = false;
  key->key_data = NULL;
  key->key_size = 0;

  pthread_mutex_init(&key->lock, NULL);

  // Generate key material based on algorithm
  int result = -1;

  switch (algorithm)
  {
  case CRYPTO_AES_128_GCM:
  case CRYPTO_AES_128_CBC:
  case CRYPTO_AES_256_GCM:
  case CRYPTO_AES_256_CBC:
    if (kms.hsm.available)
    {
      result = kms.hsm.generate_key(algorithm, &key->key_data, &key->key_size);
    }
    else
    {
      result = generate_aes_key(algorithm, &key->key_data, &key->key_size);
    }
    break;

  case CRYPTO_RSA_2048:
  case CRYPTO_RSA_4096:
  {
    int rsa_size = (algorithm == CRYPTO_RSA_2048) ? 2048 : 4096;
    EVP_PKEY *pkey = generate_rsa_keypair(rsa_size);
    if (pkey)
    {
      // Serialize private key
      BIO *bio = BIO_new(BIO_s_mem());
      if (PEM_write_bio_PrivateKey(bio, pkey, NULL, NULL, 0, NULL, NULL))
      {
        BUF_MEM *bio_mem;
        BIO_get_mem_ptr(bio, &bio_mem);
        key->key_size = bio_mem->length;
        key->key_data = secure_malloc(key->key_size);
        if (key->key_data)
        {
          memcpy(key->key_data, bio_mem->data, key->key_size);
          result = 0;
        }
      }
      BIO_free(bio);
      EVP_PKEY_free(pkey);
    }
    break;
  }

  case CRYPTO_ECDSA_P256:
  case CRYPTO_ECDSA_P384:
  case CRYPTO_ECDSA_P521:
  case CRYPTO_ECDH_P256:
  case CRYPTO_ECDH_P384:
  {
    int curve_nid;
    switch (algorithm)
    {
    case CRYPTO_ECDSA_P256:
    case CRYPTO_ECDH_P256:
      curve_nid = NID_X9_62_prime256v1;
      break;
    case CRYPTO_ECDSA_P384:
    case CRYPTO_ECDH_P384:
      curve_nid = NID_secp384r1;
      break;
    case CRYPTO_ECDSA_P521:
      curve_nid = NID_secp521r1;
      break;
    default:
      curve_nid = NID_X9_62_prime256v1;
      break;
    }

    EVP_PKEY *pkey = generate_ec_keypair(curve_nid);
    if (pkey)
    {
      // Serialize private key
      BIO *bio = BIO_new(BIO_s_mem());
      if (PEM_write_bio_PrivateKey(bio, pkey, NULL, NULL, 0, NULL, NULL))
      {
        BUF_MEM *bio_mem;
        BIO_get_mem_ptr(bio, &bio_mem);
        key->key_size = bio_mem->length;
        key->key_data = secure_malloc(key->key_size);
        if (key->key_data)
        {
          memcpy(key->key_data, bio_mem->data, key->key_size);
          result = 0;
        }
      }
      BIO_free(bio);
      EVP_PKEY_free(pkey);
    }
    break;
  }

  default:
    break;
  }

  if (result != 0)
  {
    if (key->key_data)
    {
      secure_free(key->key_data, key->key_size);
    }
    pthread_mutex_destroy(&key->lock);
    secure_free(key, sizeof(secure_key_t));
    return NULL;
  }

  return key;
}

void destroy_secure_key(secure_key_t *key)
{
  if (!key)
    return;

  pthread_mutex_lock(&key->lock);

  if (key->key_data)
  {
    secure_free(key->key_data, key->key_size);
  }

  pthread_mutex_unlock(&key->lock);
  pthread_mutex_destroy(&key->lock);

  secure_free(key, sizeof(secure_key_t));
}

int store_key(secure_key_t *key)
{
  pthread_rwlock_wrlock(&kms.keys_lock);

  for (int i = 0; i < MAX_KEYS; i++)
  {
    if (!kms.keys[i])
    {
      kms.keys[i] = key;
      pthread_rwlock_unlock(&kms.keys_lock);
      return 0;
    }
  }

  pthread_rwlock_unlock(&kms.keys_lock);
  return -1; // No space
}

secure_key_t *find_key(uint32_t key_id)
{
  pthread_rwlock_rdlock(&kms.keys_lock);

  secure_key_t *key = NULL;
  for (int i = 0; i < MAX_KEYS; i++)
  {
    if (kms.keys[i] && kms.keys[i]->key_id == key_id && !kms.keys[i]->revoked)
    {
      key = kms.keys[i];
      break;
    }
  }

  pthread_rwlock_unlock(&kms.keys_lock);
  return key;
}

int aes_gcm_encrypt(const uint8_t *key, size_t key_size,
                    const uint8_t *iv, size_t iv_size,
                    const uint8_t *plaintext, size_t plaintext_size,
                    const uint8_t *aad, size_t aad_size,
                    uint8_t **ciphertext, size_t *ciphertext_size,
                    uint8_t *tag, size_t *tag_size)
{
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  if (!ctx)
  {
    return -1;
  }

  const EVP_CIPHER *cipher;
  switch (key_size)
  {
  case 16:
    cipher = EVP_aes_128_gcm();
    break;
  case 32:
    cipher = EVP_aes_256_gcm();
    break;
  default:
    EVP_CIPHER_CTX_free(ctx);
    return -1;
  }

  // Initialize encryption
  if (EVP_EncryptInit_ex(ctx, cipher, NULL, NULL, NULL) != 1)
  {
    EVP_CIPHER_CTX_free(ctx);
    return -1;
  }

  // Set IV Length
  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, iv_size, NULL) != 1)
  {
    EVP_CIPHER_CTX_free(ctx);
    return -1;
  }

  // Set key and IV
  if (EVP_EncryptInit_ex(ctx, NULL, NULL, key, iv) != 1)
  {
    EVP_CIPHER_CTX_free(ctx);
    return -1;
  }

  *ciphertext = secure_malloc(plaintext_size);
  if (!*ciphertext)
  {
    EVP_CIPHER_CTX_free(ctx);
    return -1;
  }

  int len;
  int ciphertext_len = 0;

  // Add AAD if present
  if (aad && aad_size > 0)
  {
    if (EVP_EncryptUpdate(ctx, NULL, &len, aad, aad_size) != 1)
    {
      secure_free(*ciphertext, plaintext_size);
      EVP_CIPHER_CTX_free(ctx);
      return -1;
    }
  }

  // Encrypt plaintext
  if (EVP_EncryptUpdate(ctx, *ciphertext, &len, plaintext, plaintext_size) != 1)
  {
    secure_free(*ciphertext, plaintext_size);
    EVP_CIPHER_CTX_free(ctx);
    return -1;
  }
  ciphertext_len = len;

  // Finalize encryption
  if (EVP_EncryptFinal_ex(ctx, *ciphertext + ciphertext_len, &len) != 1)
  {
    secure_free(*ciphertext, plaintext_size);
    EVP_CIPHER_CTX_free(ctx);
    return -1;
  }
  ciphertext_len += len;

  // Get authentication tag
  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) != 1)
  {
    secure_free(*ciphertext, plaintext_size);
    EVP_CIPHER_CTX_free(ctx);
    return -1;
  }

  *ciphertext_size = ciphertext_len;
  *tag_size = 16;

  EVP_CIPHER_CTX_free(ctx);
  return 0;
}

int init_secure_memory_pool(void)
{
  kms.secure_memory.pool_size = SECURE_MEMORY_SIZE;
  kms.secure_memory.block_size = 64; // 64-bytes blocks
  size_t num_blocks = kms.secure_memory.pool_size / kms.secure_memory.block_size;

  // Allocate secure memory pool using mlock
  kms.secure_memory.memory_pool = mmap(NULL, kms.secure_memory.pool_size,
                                       PROT_READ | PROT_WRITE,
                                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

  if (kms.secure_memory.memory_pool == MAP_FAILED)
  {
    perror("mmap secure memory");
    return -1;
  }

  // Lock pages into RAM so key material is never swapped to disk
  if (mlock(kms.secure_memory.memory_pool, kms.secure_memory.pool_size) != 0)
  {
    perror("mlock secure memory");
    munmap(kms.secure_memory.memory_pool, kms.secure_memory.pool_size);
    kms.secure_memory.memory_pool = MAP_FAILED;
    return -1;
  }

  // Initialize allocation bitmap
  kms.secure_memory.allocation_map = calloc(num_blocks, sizeof(bool));
  if (!kms.secure_memory.allocation_map)
  {
    munlock(kms.secure_memory.memory_pool, kms.secure_memory.pool_size);
    munmap(kms.secure_memory.memory_pool, kms.secure_memory.pool_size);
    kms.secure_memory.memory_pool = MAP_FAILED;
    return -1;
  }

  pthread_mutex_init(&kms.secure_memory.pool_lock, NULL);
  kms.secure_memory.allocated = 0;

  printf("Secure memory pool initialized: %zu bytes\n", kms.secure_memory.pool_size);

  return 0;
}

void cleanup_secure_memory_pool(void)
{
  if (kms.secure_memory.memory_pool && kms.secure_memory.memory_pool != MAP_FAILED)
  {
    // Clear memory before releasing
    memset(kms.secure_memory.memory_pool, 0, kms.secure_memory.pool_size);
    munlock(kms.secure_memory.memory_pool, kms.secure_memory.pool_size);
    munmap(kms.secure_memory.memory_pool, kms.secure_memory.pool_size);
    kms.secure_memory.memory_pool = MAP_FAILED;
  }

  if (kms.secure_memory.allocation_map)
  {
    free(kms.secure_memory.allocation_map);
    kms.secure_memory.allocation_map = NULL;
  }

  pthread_mutex_destroy(&kms.secure_memory.pool_lock);
}

void *secure_malloc(size_t size)
{
  pthread_mutex_lock(&kms.secure_memory.pool_lock);

  size_t blocks_needed = (size + kms.secure_memory.block_size - 1) / kms.secure_memory.block_size;
  size_t total_blocks = kms.secure_memory.pool_size / kms.secure_memory.block_size;

  if (blocks_needed == 0)
  {
    blocks_needed = 1;
  }

  if (blocks_needed > total_blocks)
  {
    pthread_mutex_unlock(&kms.secure_memory.pool_lock);
    return NULL;
  }

  for (size_t i = 0; i + blocks_needed <= total_blocks; i++)
  {
    bool found = true;

    for (size_t j = 0; j < blocks_needed; j++)
    {
      if (kms.secure_memory.allocation_map[i + j])
      {
        found = false;
        break;
      }
    }

    if (found)
    {
      // Mark blocks as allocated
      for (size_t j = 0; j < blocks_needed; j++)
      {
        kms.secure_memory.allocation_map[i + j] = true;
      }

      kms.secure_memory.allocated += blocks_needed * kms.secure_memory.block_size;
      void *ptr = (uint8_t *)kms.secure_memory.memory_pool + i * kms.secure_memory.block_size;

      pthread_mutex_unlock(&kms.secure_memory.pool_lock);
      return ptr;
    }
  }

  pthread_mutex_unlock(&kms.secure_memory.pool_lock);
  return NULL; // No free blocks
}

void secure_free(void *ptr, size_t size)
{
  if (!ptr)
  {
    return;
  }

  pthread_mutex_lock(&kms.secure_memory.pool_lock);

  size_t offset = (uint8_t *)ptr - (uint8_t *)kms.secure_memory.memory_pool;
  size_t start_block = offset / kms.secure_memory.block_size;
  size_t blocks_to_free = (size + kms.secure_memory.block_size - 1) / kms.secure_memory.block_size;

  if (blocks_to_free == 0)
  {
    blocks_to_free = 1;
  }

  // Clear the full block-rounded region before freeing
  memset(ptr, 0, blocks_to_free * kms.secure_memory.block_size);

  // Mark blocks as free
  for (size_t i = 0; i < blocks_to_free; i++)
  {
    kms.secure_memory.allocation_map[start_block + i] = false;
  }

  kms.secure_memory.allocated -= blocks_to_free * kms.secure_memory.block_size;

  pthread_mutex_unlock(&kms.secure_memory.pool_lock);
}

int secure_random_bytes(uint8_t *buffer, size_t size)
{
  pthread_mutex_lock(&kms.rng.rng_lock);

  // Try getrandom() first (Linux 3.17+)
  ssize_t result = getrandom(buffer, size, 0);
  if (result == (ssize_t)size)
  {
    pthread_mutex_unlock(&kms.rng.rng_lock);
    return 0;
  }

  // Fallback to OpenSSL RAND_bytes
  if (RAND_bytes(buffer, size) == 1)
  {
    pthread_mutex_unlock(&kms.rng.rng_lock);
    return 0;
  }

  int fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0)
  {
    pthread_mutex_unlock(&kms.rng.rng_lock);
    return -1;
  }

  size_t total_read = 0;
  while (total_read < size)
  {
    ssize_t bytes_read = read(fd, buffer + total_read, size - total_read);
    if (bytes_read <= 0)
    {
      close(fd);
      pthread_mutex_unlock(&kms.rng.rng_lock);
      return -1;
    }
    total_read += bytes_read;
  }

  close(fd);
  pthread_mutex_unlock(&kms.rng.rng_lock);
  return 0;
}

int init_hardware_rng(void)
{
  // Try to use hardware RNG if available

  int rng_fd = open("/dev/hwrng", O_RDONLY);
  if (rng_fd >= 0)
  {
    uint8_t test_bytes[16];
    if (read(rng_fd, test_bytes, sizeof(test_bytes)) == sizeof(test_bytes))
    {
      close(rng_fd);
      printf("Hardware RNG available\n");
      return 0;
    }

    close(rng_fd);
  }

  // Fallback to /dev/urandom
  printf("Using software RNG\n");

  return 0;
}

int aes_gcm_decrypt(const uint8_t *key, size_t key_size,
                    const uint8_t *iv, size_t iv_size,
                    const uint8_t *ciphertext, size_t ciphertext_size,
                    const uint8_t *aad, size_t aad_size,
                    const uint8_t *tag, size_t tag_size,
                    uint8_t **plaintext, size_t *plaintext_size)
{
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  if (!ctx)
  {
    return -1;
  }

  const EVP_CIPHER *cipher;
  switch (key_size)
  {
  case 16:
    cipher = EVP_aes_128_gcm();
    break;
  case 32:
    cipher = EVP_aes_256_gcm();
    break;
  default:
    EVP_CIPHER_CTX_free(ctx);
    return -1;
  }

  // Initialize decryption
  if (EVP_DecryptInit_ex(ctx, cipher, NULL, NULL, NULL) != 1)
  {
    EVP_CIPHER_CTX_free(ctx);
    return -1;
  }

  // Set IV length
  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, iv_size, NULL) != 1)
  {
    EVP_CIPHER_CTX_free(ctx);
    return -1;
  }

  // Set key and IV
  if (EVP_DecryptInit_ex(ctx, NULL, NULL, key, iv) != 1)
  {
    EVP_CIPHER_CTX_free(ctx);
    return -1;
  }

  *plaintext = secure_malloc(ciphertext_size);
  if (!*plaintext)
  {
    EVP_CIPHER_CTX_free(ctx);
    return -1;
  }

  int len;
  int plaintext_len = 0;

  // Add AAD if present
  if (aad && aad_size > 0)
  {
    if (EVP_DecryptUpdate(ctx, NULL, &len, aad, aad_size) != 1)
    {
      secure_free(*plaintext, ciphertext_size);
      EVP_CIPHER_CTX_free(ctx);
      return -1;
    }
  }

  // Decrypt cipher text
  if (EVP_DecryptUpdate(ctx, *plaintext, &len, ciphertext, ciphertext_size) != 1)
  {
    secure_free(*plaintext, ciphertext_size);
    EVP_CIPHER_CTX_free(ctx);
    return -1;
  }

  plaintext_len = len;

  // Set expected authentication tag
  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, tag_size, (void *)tag) != 1)
  {
    secure_free(*plaintext, ciphertext_size);
    EVP_CIPHER_CTX_free(ctx);
    return -1;
  }

  // Finalize decryption and verify authentication
  int ret = EVP_DecryptFinal_ex(ctx, *plaintext + plaintext_len, &len);
  if (ret <= 0)
  {
    // Authentication failed
    secure_free(*plaintext, ciphertext_size);
    EVP_CIPHER_CTX_free(ctx);
    return -1;
  }
  plaintext_len += len;

  *plaintext_size = plaintext_len;

  EVP_CIPHER_CTX_free(ctx);
  return 0;
}

int rsa_sign(EVP_PKEY *private_key, const uint8_t *data, size_t data_size, uint8_t **signature, size_t *signature_size)
{
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  if (!ctx)
  {
    return -1;
  }

  if (EVP_DigestSignInit(ctx, NULL, EVP_sha256(), NULL, private_key) != 1)
  {
    EVP_MD_CTX_free(ctx);
    return -1;
  }

  if (EVP_DigestSignUpdate(ctx, data, data_size) != 1)
  {
    EVP_MD_CTX_free(ctx);
    return -1;
  }

  // Get signature length
  size_t sig_len;
  if (EVP_DigestSignFinal(ctx, NULL, &sig_len) != 1)
  {
    EVP_MD_CTX_free(ctx);
    return -1;
  }

  *signature = secure_malloc(sig_len);
  if (!*signature)
  {
    EVP_MD_CTX_free(ctx);
    return -1;
  }

  if (EVP_DigestSignFinal(ctx, *signature, &sig_len) != 1)
  {
    secure_free(*signature, sig_len);
    EVP_MD_CTX_free(ctx);
    return -1;
  }

  *signature_size = sig_len;
  EVP_MD_CTX_free(ctx);
  return 0;
}

int rsa_verify(EVP_PKEY *public_key, const uint8_t *data, size_t data_size,
               const uint8_t *signature, size_t signature_size)
{
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  if (!ctx)
    return -1;

  if (EVP_DigestVerifyInit(ctx, NULL, EVP_sha256(), NULL, public_key) != 1)
  {
    EVP_MD_CTX_free(ctx);
    return -1;
  }

  if (EVP_DigestVerifyUpdate(ctx, data, data_size) != 1)
  {
    EVP_MD_CTX_free(ctx);
    return -1;
  }

  int ret = EVP_DigestVerifyFinal(ctx, signature, signature_size);
  EVP_MD_CTX_free(ctx);

  return (ret == 1) ? 0 : -1;
}

// Key derivation functions
int pbkdf2_derive_key(const char *password, size_t password_len,
                      const uint8_t *salt, size_t salt_len,
                      int iterations, size_t key_len,
                      uint8_t **derived_key)
{
  *derived_key = secure_malloc(key_len);
  if (!*derived_key)
  {
    return -1;
  }

  if (PKCS5_PBKDF2_HMAC(password, password_len, salt, salt_len,
                        iterations, EVP_sha256(), key_len, *derived_key) != 1)
  {
    secure_free(*derived_key, key_len);
    return -1;
  }

  return 0;
}

int hkdf_derive_key(const uint8_t *ikm, size_t ikm_len,
                    const uint8_t *salt, size_t salt_len,
                    const uint8_t *info, size_t info_len,
                    size_t key_len, uint8_t **derived_key)
{
  *derived_key = secure_malloc(key_len);
  if (!*derived_key)
  {
    return -1;
  }

  EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL);
  if (!ctx)
  {
    secure_free(*derived_key, key_len);
    return -1;
  }

  if (EVP_PKEY_derive_init(ctx) != 1)
  {
    EVP_PKEY_CTX_free(ctx);
    secure_free(*derived_key, key_len);
    return -1;
  }

  if (EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha256()) != 1)
  {
    EVP_PKEY_CTX_free(ctx);
    secure_free(*derived_key, key_len);
    return -1;
  }

  if (EVP_PKEY_CTX_set1_hkdf_key(ctx, ikm, ikm_len) != 1)
  {
    EVP_PKEY_CTX_free(ctx);
    secure_free(*derived_key, key_len);
    return -1;
  }

  if (salt && salt_len > 0)
  {
    if (EVP_PKEY_CTX_set1_hkdf_salt(ctx, salt, salt_len) != 1)
    {
      EVP_PKEY_CTX_free(ctx);
      secure_free(*derived_key, key_len);
      return -1;
    }
  }

  if (info && info_len > 0)
  {
    if (EVP_PKEY_CTX_add1_hkdf_info(ctx, info, info_len) != 1)
    {
      EVP_PKEY_CTX_free(ctx);
      secure_free(*derived_key, key_len);
      return -1;
    }
  }

  size_t out_len = key_len;
  if (EVP_PKEY_derive(ctx, *derived_key, &out_len) != 1)
  {
    EVP_PKEY_CTX_free(ctx);
    secure_free(*derived_key, key_len);
    return -1;
  }

  EVP_PKEY_CTX_free(ctx);
  return 0;
}

// High-level API functions
uint32_t crypto_generate_key(crypto_algorithm_t algorithm)
{
  key_type_t key_type;

  switch (algorithm)
  {
  case CRYPTO_AES_128_GCM:
  case CRYPTO_AES_256_GCM:
  case CRYPTO_AES_128_CBC:
  case CRYPTO_AES_256_CBC:
  case CRYPTO_CHACHA20_POLY1305:
    key_type = KEY_TYPE_SYMMETRIC;
    break;
  case CRYPTO_RSA_2048:
  case CRYPTO_RSA_4096:
    key_type = KEY_TYPE_RSA_PRIVATE;
    break;
  case CRYPTO_ECDSA_P256:
  case CRYPTO_ECDSA_P384:
  case CRYPTO_ECDSA_P521:
  case CRYPTO_ECDH_P256:
  case CRYPTO_ECDH_P384:
    key_type = KEY_TYPE_EC_PRIVATE;
    break;
  default:
    return 0;
  }

  secure_key_t *key = create_secure_key(key_type, algorithm);
  if (!key)
  {
    return 0;
  }

  if (store_key(key) != 0)
  {
    destroy_secure_key(key);
    return 0;
  }

  return key->key_id;
}

int crypto_encrypt(uint32_t key_id, const uint8_t *plaintext, size_t plaintext_size,
                   const uint8_t *aad, size_t aad_size,
                   uint8_t **ciphertext, size_t *ciphertext_size,
                   uint8_t *iv, size_t *iv_size,
                   uint8_t *tag, size_t *tag_size)
{
  secure_key_t *key = find_key(key_id);
  if (!key)
  {
    return -1;
  }

  pthread_mutex_lock(&key->lock);

  // Check key validity
  time_t now = time(NULL);
  if (key->revoked || now > key->expiration_time ||
      key->usage_count >= key->max_usage)
  {
    pthread_mutex_unlock(&key->lock);
    return -1;
  }

  int result = -1;

  switch (key->algorithm)
  {
  case CRYPTO_AES_128_GCM:
  case CRYPTO_AES_256_GCM:
    // Generate random IV
    *iv_size = 12; // 96-bit IV for GCM
    if (secure_random_bytes(iv, *iv_size) != 0)
    {
      break;
    }

    if (kms.hsm.available)
    {
      result = kms.hsm.encrypt(key->key_data, key->key_size,
                               plaintext, plaintext_size,
                               ciphertext, ciphertext_size);
    }
    else
    {
      result = aes_gcm_encrypt(key->key_data, key->key_size,
                               iv, *iv_size,
                               plaintext, plaintext_size,
                               aad, aad_size,
                               ciphertext, ciphertext_size,
                               tag, tag_size);
    }
    break;

  case CRYPTO_AES_128_CBC:
  case CRYPTO_AES_256_CBC:
    // Generate random IV
    *iv_size = 16; // 128-bit IV for CBC
    if (secure_random_bytes(iv, *iv_size) != 0)
    {
      break;
    }

    // Implement CBC mode encryption
    // ... (implementation details)
    break;

  default:
    break;
  }

  if (result == 0)
  {
    key->usage_count++;
  }

  pthread_mutex_unlock(&key->lock);
  return result;
}

int crypto_decrypt(uint32_t key_id, const uint8_t *ciphertext, size_t ciphertext_size,
                   const uint8_t *aad, size_t aad_size,
                   const uint8_t *iv, size_t iv_size,
                   const uint8_t *tag, size_t tag_size,
                   uint8_t **plaintext, size_t *plaintext_size)
{
  secure_key_t *key = find_key(key_id);
  if (!key)
  {
    return -1;
  }

  pthread_mutex_lock(&key->lock);

  // Check key validity
  time_t now = time(NULL);
  if (key->revoked || now > key->expiration_time ||
      key->usage_count >= key->max_usage)
  {
    pthread_mutex_unlock(&key->lock);
    return -1;
  }

  int result = -1;

  switch (key->algorithm)
  {
  case CRYPTO_AES_128_GCM:
  case CRYPTO_AES_256_GCM:
    if (kms.hsm.available)
    {
      result = kms.hsm.decrypt(key->key_data, key->key_size,
                               ciphertext, ciphertext_size,
                               plaintext, plaintext_size);
    }
    else
    {
      result = aes_gcm_decrypt(key->key_data, key->key_size,
                               iv, iv_size,
                               ciphertext, ciphertext_size,
                               aad, aad_size,
                               tag, tag_size,
                               plaintext, plaintext_size);
    }
    break;

  case CRYPTO_AES_128_CBC:
  case CRYPTO_AES_256_CBC:
    // Implement CBC mode decryption
    // ... (implementation details)
    break;

  default:
    break;
  }

  if (result == 0)
  {
    key->usage_count++;
  }

  pthread_mutex_unlock(&key->lock);
  return result;
}

int crypto_sign(uint32_t key_id, const uint8_t *data, size_t data_size,
                uint8_t **signature, size_t *signature_size)
{
  secure_key_t *key = find_key(key_id);
  if (!key || key->type != KEY_TYPE_RSA_PRIVATE)
  {
    return -1;
  }

  pthread_mutex_lock(&key->lock);

  // Check key validity
  time_t now = time(NULL);
  if (key->revoked || now > key->expiration_time ||
      key->usage_count >= key->max_usage)
  {
    pthread_mutex_unlock(&key->lock);
    return -1;
  }

  // Load private key from stored data
  BIO *bio = BIO_new_mem_buf(key->key_data, key->key_size);
  EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
  BIO_free(bio);

  if (!pkey)
  {
    pthread_mutex_unlock(&key->lock);
    return -1;
  }

  int result = rsa_sign(pkey, data, data_size, signature, signature_size);

  if (result == 0)
  {
    key->usage_count++;
  }

  EVP_PKEY_free(pkey);
  pthread_mutex_unlock(&key->lock);
  return result;
}

int crypto_verify(uint32_t key_id, const uint8_t *data, size_t data_size,
                  const uint8_t *signature, size_t signature_size)
{
  secure_key_t *key = find_key(key_id);
  if (!key)
  {
    return -1;
  }

  pthread_mutex_lock(&key->lock);

  // Check key validity
  time_t now = time(NULL);
  if (key->revoked || now > key->expiration_time)
  {
    pthread_mutex_unlock(&key->lock);
    return -1;
  }

  // Load key from stored data
  BIO *bio = BIO_new_mem_buf(key->key_data, key->key_size);
  EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
  if (!pkey)
  {
    BIO_free(bio);
    bio = BIO_new_mem_buf(key->key_data, key->key_size);
    pkey = PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL);
  }
  BIO_free(bio);

  if (!pkey)
  {
    pthread_mutex_unlock(&key->lock);
    return -1;
  }

  int result = rsa_verify(pkey, data, data_size, signature, signature_size);

  if (result == 0)
  {
    key->usage_count++;
  }

  EVP_PKEY_free(pkey);
  pthread_mutex_unlock(&key->lock);
  return result;
}

void crypto_revoke_key(uint32_t key_id)
{
  secure_key_t *key = find_key(key_id);
  if (!key)
  {
    return;
  }

  pthread_mutex_lock(&key->lock);
  key->revoked = true;
  pthread_mutex_unlock(&key->lock);
}

// Secure hash functions
int crypto_hash_sha256(const uint8_t *data, size_t data_size, uint8_t hash[32])
{
  unsigned int hash_len = 0;

  if (EVP_Digest(data, data_size, hash, &hash_len, EVP_sha256(), NULL) != 1)
  {
    return -1;
  }

  return (hash_len == 32) ? 0 : -1;
}

int crypto_hmac_sha256(const uint8_t *key, size_t key_size,
                       const uint8_t *data, size_t data_size,
                       uint8_t hmac[32])
{
  unsigned int hmac_len;

  if (!HMAC(EVP_sha256(), key, key_size, data, data_size, hmac, &hmac_len))
  {
    return -1;
  }

  return (hmac_len == 32) ? 0 : -1;
}

// Initialization and cleanup
int init_crypto_framework(void)
{
  // Initialize OpenSSL
  OpenSSL_add_all_algorithms();
  ERR_load_crypto_strings();

  // Initialize secure memory pool
  if (init_secure_memory_pool() != 0)
  {
    return -1;
  }

  // Initialize random number generator
  pthread_mutex_init(&kms.rng.rng_lock, NULL);
  if (init_hardware_rng() != 0)
  {
    cleanup_secure_memory_pool();
    return -1;
  }

  // Initialize key management
  pthread_rwlock_init(&kms.keys_lock, NULL);
  kms.next_key_id = 1;

  // Try to initialize HSM (optional)
  kms.hsm.available = false;
  // ... HSM initialization code would go here

  printf("Cryptographic framework initialized\n");
  return 0;
}

void cleanup_crypto_framework(void)
{
  // Clean up keys
  pthread_rwlock_wrlock(&kms.keys_lock);
  for (int i = 0; i < MAX_KEYS; i++)
  {
    if (kms.keys[i])
    {
      destroy_secure_key(kms.keys[i]);
      kms.keys[i] = NULL;
    }
  }
  pthread_rwlock_unlock(&kms.keys_lock);
  pthread_rwlock_destroy(&kms.keys_lock);

  // Clean up HSM
  if (kms.hsm.available && kms.hsm.cleanup)
  {
    kms.hsm.cleanup();
  }

  // Clean up RNG
  pthread_mutex_destroy(&kms.rng.rng_lock);

  // Clean up secure memory
  cleanup_secure_memory_pool();

  // Clean up OpenSSL
  EVP_cleanup();
  ERR_free_strings();

  printf("Cryptographic framework cleanup completed\n");
}
