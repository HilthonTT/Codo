#define _GNU_SOURCE

#include <sys/types.h>
#include <string.h>
#include <unistd.h>

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

int write_wal_record(uint64_t txn_id, wal_record_type_t type, uint32_t page_id, const void* data, size_t data_length) {
  pthread_mutex_lock(&storage_engine.wal_mutex);

  size_t record_size = sizeof(wal_record_type_t) + data_length;

  // Check if we need to flush the buffer
  if (storage_engine.wal_buffer_pos + record_size > WAL_BUFFER_SIZE) {
    // Write buffer to disk
    ssize_t bytes_written = write(storage_engine.wal_fd, storage_engine.wal_buffer, storage_engine.wal_buffer_pos);

    if (bytes_written != (ssize_t)storage_engine.wal_buffer_pos) {
      pthread_mutex_unlock(&storage_engine.wal_mutex);
      return -1;
    }

    storage_engine.wal_buffer_pos = 0;
    fsync(storage_engine.wal_fd);
  }

  // Create WAL record
  wal_record_t* record = (wal_record_t*)(storage_engine.buffer_pool + storage_engine.wal_buffer_pos);
  record->lsn = allocate_lsn();
  record->txn_id = txn_id;
  record->type = type;
  record->page_id = page_id;
  record->data_length = data_length;

  if (data && data_length > 0) {
    memcpy(record->data, data, data_length);
  }

  record->checksum = calculate_checksum(record, record_size - sizeof(record->checksum));

  storage_engine.wal_buffer_pos += record_size;
  atomic_fetch_add(&storage_engine.stats.wal_records_written, 1);

  thread_mutex_unlock(&storage_engine.wal_mutex);

  return 0;
}

int flush_wal_buffer(void) {
  pthread_mutex_lock(&storage_engine.wal_mutex);

  if (storage_engine.wal_buffer_pos > 0) {
    ssize_t bytes_written = write(storage_engine.wal_fd, storage_engine.wal_buffer, storage_engine.wal_buffer_pos);

    if (bytes_written != (ssize_t)storage_engine.wal_buffer_pos) {
      pthread_mutex_unlock(&storage_engine.wal_mutex);
      return -1;
    }

    fsync(storage_engine.wal_fd);
    storage_engine.wal_buffer_pos = 0;
  }

  thread_mutex_unlock(&storage_engine.wal_mutex);
  return 0;
}
