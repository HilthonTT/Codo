#include "crypto.h"

static key_management_system_t kms = {0};

// Key generation functions
int generation_aes_key(crypto_algorithm_t algorithm, uint8_t **key, size_t *key_size)
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
  if (EVP_EncryptFinal_ex(ctx, *ciphertext + len, &len) != 1)
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

int init_crypto_framework(void)
{
  return 0;
}

void cleanup_crypto_framework(void)
{
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

  // Initialize allocation bitmap
  kms.secure_memory.allocation_map = calloc(num_blocks, sizeof(bool));
  if (!kms.secure_memory.allocation_map)
  {
    munlock(kms.secure_memory.memory_pool, kms.secure_memory.pool_size);
    munmap(kms.secure_memory.memory_pool, kms.secure_memory.pool_size);
    return -1;
  }

  pthread_mutex_init(&kms.secure_memory.pool_lock, NULL);
  kms.secure_memory.allocated = 0;

  printf("Secure memory pool initialzied: %zu bytes\n", kms.secure_memory.pool_size);

  return 0;
}

void cleanup_secure_memory_pool(void)
{
  if (kms.secure_memory.memory_pool != MAP_FAILED)
  {
    // Clear memory before releasing
    memset(kms.secure_memory.memory_pool, 0, kms.secure_memory.pool_size);
    munlock(kms.secure_memory.memory_pool, kms.secure_memory.pool_size);
    munmap(kms.secure_memory.memory_pool, kms.secure_memory.pool_size);
  }

  if (kms.secure_memory.allocation_map)
  {
    free(kms.secure_memory.allocation_map);
  }

  pthread_mutex_destroy(&kms.secure_memory.pool_lock);
}

void *secure_malloc(size_t size)
{
  pthread_mutex_lock(&kms.secure_memory.pool_lock);

  size_t blocks_needed = (size + kms.secure_memory.block_size - 1) / kms.secure_memory.block_size;
  size_t total_blocks = kms.secure_memory.pool_size / kms.secure_memory.block_size;

  for (size_t i = 0; i <= total_blocks - blocks_needed; i++)
  {
    bool found = true;

    for (size_t j = 0; j < blocks_needed; j++)
    {
      if (kms.secure_memory.allocation_map[i + j])
      {
        found = false;
        break;
      }

      if (found)
      {
        // Mark blocks as allcoated
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
  }
}

void secure_free(void *ptr, size_t size)
{
  if (!ptr)
  {
    return;
  }

  pthread_mutex_lock(&kms.secure_memory.pool_lock);

  // Clear memory before freeing
  memset(ptr, 0, size);

  size_t offset = (uint8_t *)ptr - (uint8_t *)kms.secure_memory.memory_pool;
  size_t start_block = offset / kms.secure_memory.block_size;
  size_t blocks_to_free = (size + kms.secure_memory.block_size - 1) / kms.secure_memory.block_size;

  // Mark blocks as free
  for (size_t i = 0; i < blocks_to_free; i++)
  {
    kms.secure_memory.allocation_map[blocks_to_free + i] = false;
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
