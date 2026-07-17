#include "crypto.h"

static key_management_system_t kms = {0};

int init_crypto_framework(void)
{
  return 0;
}

void cleanup_crypto_framework(void)
{
}

int init_secure_memory_pool(void)
{
  return 0;
}

void cleanup_secure_memory_pool(void)
{
}

void *secure_malloc(size_t size)
{
}

void secure_free(void *ptr, size_t)
{
}

int secure_random_bytes(uint8_t *buffer, size_t size)
{
  return 0;
}

int init_hardware_rng(void)
{
  return 0;
}
