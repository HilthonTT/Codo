#define _GNU_SOURCE

#include "btree_storage.h"

static storage_engine_t storage_engine = {0};

// Utility functions
uint64_t hash_key(const char *key, size_t length) {
  // FNV-1a hash
  uint64_t hash = 14695981039346656037ULL;
  for (size_t i = 0; i < length; i++) {
    hash ^= (uint8_t)key[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

uint32_t hash_page_id(uint32_t page_id) {
  // Simple hash function
  return page_id % storage_engine.hash_table_size;
}

uint32_t calculate_checksum(const void* data, size_t length) {
  // Simple CRC32-like checksum
  uint32_t checksum = 0;
  const uint8_t* bytes = (const uint8_t*)data;

  for (size_t i = 0; i < length; i++) {
    checksum = (checksum << 1) ^ bytes[i];
  }

  return checksum;
}

uint64_t allocate_lsn(void) {
  return atomic_fetch_add(&storage_engine.next_lsn, 1);
}
