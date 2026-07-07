#ifndef HASH_H
#define HASH_H

#include <stdint.h>
#include <string.h>

#define FNV_OFFSET_32 2166136261u
#define FNV_PRIME_32 16777619u

uint32_t fnv1_32(const void *data, size_t len);

#endif