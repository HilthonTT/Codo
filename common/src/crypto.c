// Password hashing, token signing and the mlock'd arena behind them.
//
// The arena is a fixed mmap'd region locked into RAM, carved into 64-byte
// blocks tracked by a bitmap. Derived key material lives there rather than on
// the heap so it is never written to swap, and every release zeroes the
// blocks before handing them back.

#include "crypto.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#define SECURE_BLOCK_SIZE 64

static struct
{
  void *memory_pool;
  size_t pool_size;
  size_t block_size;
  size_t allocated;
  bool *allocation_map;
  pthread_mutex_t pool_lock;
  pthread_mutex_t rng_lock;
} g_crypto = {.memory_pool = MAP_FAILED};

static int init_secure_memory_pool(void)
{
  g_crypto.pool_size = SECURE_MEMORY_SIZE;
  g_crypto.block_size = SECURE_BLOCK_SIZE;
  size_t num_blocks = g_crypto.pool_size / g_crypto.block_size;

  g_crypto.memory_pool = mmap(NULL, g_crypto.pool_size,
                              PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

  if (g_crypto.memory_pool == MAP_FAILED)
  {
    perror("mmap secure memory");
    return -1;
  }

  // Lock pages into RAM so key material is never swapped to disk
  if (mlock(g_crypto.memory_pool, g_crypto.pool_size) != 0)
  {
    perror("mlock secure memory");
    munmap(g_crypto.memory_pool, g_crypto.pool_size);
    g_crypto.memory_pool = MAP_FAILED;
    return -1;
  }

  g_crypto.allocation_map = calloc(num_blocks, sizeof(bool));
  if (!g_crypto.allocation_map)
  {
    munlock(g_crypto.memory_pool, g_crypto.pool_size);
    munmap(g_crypto.memory_pool, g_crypto.pool_size);
    g_crypto.memory_pool = MAP_FAILED;
    return -1;
  }

  pthread_mutex_init(&g_crypto.pool_lock, NULL);
  g_crypto.allocated = 0;

  return 0;
}

static void cleanup_secure_memory_pool(void)
{
  if (g_crypto.memory_pool != MAP_FAILED)
  {
    memset(g_crypto.memory_pool, 0, g_crypto.pool_size);
    munlock(g_crypto.memory_pool, g_crypto.pool_size);
    munmap(g_crypto.memory_pool, g_crypto.pool_size);
    g_crypto.memory_pool = MAP_FAILED;
  }

  if (g_crypto.allocation_map)
  {
    free(g_crypto.allocation_map);
    g_crypto.allocation_map = NULL;
  }

  pthread_mutex_destroy(&g_crypto.pool_lock);
}

void *secure_malloc(size_t size)
{
  pthread_mutex_lock(&g_crypto.pool_lock);

  size_t blocks_needed = (size + g_crypto.block_size - 1) / g_crypto.block_size;
  size_t total_blocks = g_crypto.pool_size / g_crypto.block_size;

  if (blocks_needed == 0)
  {
    blocks_needed = 1;
  }

  if (blocks_needed > total_blocks)
  {
    pthread_mutex_unlock(&g_crypto.pool_lock);
    return NULL;
  }

  for (size_t i = 0; i + blocks_needed <= total_blocks; i++)
  {
    bool found = true;

    for (size_t j = 0; j < blocks_needed; j++)
    {
      if (g_crypto.allocation_map[i + j])
      {
        found = false;
        break;
      }
    }

    if (found)
    {
      for (size_t j = 0; j < blocks_needed; j++)
      {
        g_crypto.allocation_map[i + j] = true;
      }

      g_crypto.allocated += blocks_needed * g_crypto.block_size;
      void *ptr = (uint8_t *)g_crypto.memory_pool + i * g_crypto.block_size;

      pthread_mutex_unlock(&g_crypto.pool_lock);
      return ptr;
    }
  }

  pthread_mutex_unlock(&g_crypto.pool_lock);
  return NULL; // No free blocks
}

void secure_free(void *ptr, size_t size)
{
  if (!ptr)
  {
    return;
  }

  pthread_mutex_lock(&g_crypto.pool_lock);

  size_t offset = (uint8_t *)ptr - (uint8_t *)g_crypto.memory_pool;
  size_t start_block = offset / g_crypto.block_size;
  size_t blocks_to_free = (size + g_crypto.block_size - 1) / g_crypto.block_size;

  if (blocks_to_free == 0)
  {
    blocks_to_free = 1;
  }

  memset(ptr, 0, blocks_to_free * g_crypto.block_size);

  for (size_t i = 0; i < blocks_to_free; i++)
  {
    g_crypto.allocation_map[start_block + i] = false;
  }

  g_crypto.allocated -= blocks_to_free * g_crypto.block_size;

  pthread_mutex_unlock(&g_crypto.pool_lock);
}

int secure_random_bytes(uint8_t *buffer, size_t size)
{
  pthread_mutex_lock(&g_crypto.rng_lock);

  // Try getrandom() first (Linux 3.17+)
  ssize_t result = getrandom(buffer, size, 0);
  if (result == (ssize_t)size)
  {
    pthread_mutex_unlock(&g_crypto.rng_lock);
    return 0;
  }

  if (RAND_bytes(buffer, size) == 1)
  {
    pthread_mutex_unlock(&g_crypto.rng_lock);
    return 0;
  }

  int fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0)
  {
    pthread_mutex_unlock(&g_crypto.rng_lock);
    return -1;
  }

  size_t total_read = 0;
  while (total_read < size)
  {
    ssize_t bytes_read = read(fd, buffer + total_read, size - total_read);
    if (bytes_read <= 0)
    {
      close(fd);
      pthread_mutex_unlock(&g_crypto.rng_lock);
      return -1;
    }
    total_read += bytes_read;
  }

  close(fd);
  pthread_mutex_unlock(&g_crypto.rng_lock);
  return 0;
}

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
    *derived_key = NULL;
    return -1;
  }

  return 0;
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

int init_crypto_framework(void)
{
  if (init_secure_memory_pool() != 0)
  {
    return -1;
  }

  pthread_mutex_init(&g_crypto.rng_lock, NULL);

  printf("Cryptographic framework initialized (%zu byte secure pool)\n",
         g_crypto.pool_size);
  return 0;
}

void cleanup_crypto_framework(void)
{
  pthread_mutex_destroy(&g_crypto.rng_lock);
  cleanup_secure_memory_pool();
}
