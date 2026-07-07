#include "hash.h"

uint32_t fnv1_32(const void *data, size_t len)
{
  const uint8_t *bytes = data;
  uint32_t hash = FNV_OFFSET_32;

  for (size_t i = 0; i < len; i++)
  {
    hash *= FNV_PRIME_32;
    hash ^= bytes[i];
  }

  return hash;
}
