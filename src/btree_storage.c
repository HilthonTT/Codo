#define _GNU_SOURCE

#include <sys/types.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

#include "btree_storage.h"

static storage_engine_t storage_engine = {0};

// Utility functions
uint64_t hash_key(const char *key, size_t length)
{
  // FNV-1a hash
  uint64_t hash = 14695981039346656037ULL;
  for (size_t i = 0; i < length; i++)
  {
    hash ^= (uint8_t)key[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

uint32_t hash_page_id(uint32_t page_id)
{
  // Simple hash function
  return page_id % storage_engine.hash_table_size;
}

uint32_t calculate_checksum(const void *data, size_t length)
{
  // Simple CRC32-like checksum
  uint32_t checksum = 0;
  const uint8_t *bytes = (const uint8_t *)data;

  for (size_t i = 0; i < length; i++)
  {
    checksum = (checksum << 1) ^ bytes[i];
  }

  return checksum;
}

uint64_t allocate_lsn(void)
{
  return atomic_fetch_add(&storage_engine.next_lsn, 1);
}

int write_wal_record(uint64_t txn_id, wal_record_type_t type, uint32_t page_id, const void *data, size_t data_length)
{
  pthread_mutex_lock(&storage_engine.wal_mutex);

  size_t record_size = sizeof(wal_record_type_t) + data_length;

  // Check if we need to flush the buffer
  if (storage_engine.wal_buffer_pos + record_size > WAL_BUFFER_SIZE)
  {
    // Write buffer to disk
    ssize_t bytes_written = write(storage_engine.wal_fd, storage_engine.wal_buffer, storage_engine.wal_buffer_pos);

    if (bytes_written != (ssize_t)storage_engine.wal_buffer_pos)
    {
      pthread_mutex_unlock(&storage_engine.wal_mutex);
      return -1;
    }

    storage_engine.wal_buffer_pos = 0;
    fsync(storage_engine.wal_fd);
  }

  // Create WAL record
  wal_record_t *record = (wal_record_t *)(storage_engine.buffer_pool + storage_engine.wal_buffer_pos);
  record->lsn = allocate_lsn();
  record->txn_id = txn_id;
  record->type = type;
  record->page_id = page_id;
  record->data_length = data_length;

  if (data && data_length > 0)
  {
    memcpy(record->data, data, data_length);
  }

  record->checksum = calculate_checksum(record, record_size - sizeof(record->checksum));

  storage_engine.wal_buffer_pos += record_size;
  atomic_fetch_add(&storage_engine.stats.wal_records_written, 1);

  pthread_mutex_unlock(&storage_engine.wal_mutex);

  return 0;
}

int flush_wal_buffer(void)
{
  pthread_mutex_lock(&storage_engine.wal_mutex);

  if (storage_engine.wal_buffer_pos > 0)
  {
    ssize_t bytes_written = write(storage_engine.wal_fd, storage_engine.wal_buffer, storage_engine.wal_buffer_pos);

    if (bytes_written != (ssize_t)storage_engine.wal_buffer_pos)
    {
      pthread_mutex_unlock(&storage_engine.wal_mutex);
      return -1;
    }

    fsync(storage_engine.wal_fd);
    storage_engine.wal_buffer_pos = 0;
  }

  pthread_mutex_unlock(&storage_engine.wal_mutex);
  return 0;
}

buffer_entry_t *find_buffer_entry(uint32_t page_id)
{
  uint32_t hash = hash_page_id(page_id);
  uint32_t entry_idx = storage_engine.hash_table[hash];

  while (entry_idx != UINT32_MAX)
  {
    buffer_entry_t *entry = &storage_engine.buffer_pool[entry_idx];
    if (entry->page_id == page_id)
    {
      return entry;
    }

    entry_idx = entry->hash_next;
  }

  return NULL;
}

buffer_entry_t *allocate_buffer_entry(uint32_t page_id)
{
  pthread_mutex_lock(&storage_engine.buffer_mutex);

  // Find least recently used unpinned page
  buffer_entry_t *victim = NULL;
  struct timespec oldest_time = {0};

  for (size_t i = 0; i < BUFFER_POOL_SIZE; i++)
  {
    buffer_entry_t *entry = &storage_engine.buffer_pool[i];

    if (!entry->pinned && atomic_load(&entry->ref_count) == 0)
    {
      if (!victim || entry->last_access.tv_sec < oldest_time.tv_sec ||
          (entry->last_access.tv_sec == oldest_time.tv_sec &&
           entry->last_access.tv_nsec < oldest_time.tv_nsec))
      {
        victim = entry;
        oldest_time = entry->last_access;
      }
    }
  }

  if (!victim)
  {
    pthread_mutex_unlock(&storage_engine.buffer_mutex);
    return NULL; // Buffer pool full
  }

  // Evict victim if dirty
  if (victim->dirty && victim->page)
  {
    // Write page to disk
    off_t offset = victim->page_id * PAGE_SIZE;
    if (pwrite(storage_engine.data_fd, victim->page, PAGE_SIZE, offset) != 0)
    {
      pthread_mutex_unlock(&storage_engine.buffer_mutex);
      return NULL;
    }
    atomic_fetch_add(&storage_engine.stats.pages_written, 1);
    victim->dirty = false;
  }

  // Remove from hash table
  if (victim->page_id != 0)
  {
    uint32_t hash = hash_page_id(page_id);
    uint32_t *current = &storage_engine.hash_table[hash];

    while (*current != UINT32_MAX)
    {
      if (*current == (victim - storage_engine.buffer_pool))
      {
        *current = victim->hash_next;
        break;
      }
      current = &storage_engine.buffer_pool[*current].hash_next;
    }
  }

  // Initialize new entry
  victim->page_id = page_id;
  victim->dirty = false;
  victim->pinned = false;
  atomic_store(&victim->ref_count, 1);
  clock_gettime(CLOCK_MONOTONIC, &victim->last_access);

  // Add to hash table
  uint32_t hash = hash_page_id(page_id);
  victim->hash_next = storage_engine.hash_table[hash];
  storage_engine.hash_table[hash] = victim - storage_engine.buffer_pool;

  pthread_mutex_unlock(&storage_engine.buffer_mutex);

  return victim;
}

btree_page_t *get_page(uint32_t page_id, lock_type_t lock_type)
{
  // Check buffer pool first
  buffer_entry_t *entry = find_buffer_entry(page_id);

  if (entry)
  {
    atomic_fetch_add(&entry->ref_count, 1);
    clock_gettime(CLOCK_MONOTONIC, &entry->last_access);
    atomic_fetch_add(&storage_engine.stats.cache_hits, 1);

    // Acquire page lock
    if (lock_type == LOCK_SHARED)
    {
      pthread_rwlock_rdlock(&entry->page_lock);
    }
    else if (lock_type == LOCK_EXCLUSIVE)
    {
      pthread_rwlock_wrlock(&entry->page_lock);
    }

    return entry->page;
  }

  // Page not in buffer pool - allocate new entry
  entry = allocate_buffer_entry(page_id);
  if (!entry)
  {
    return NULL;
  }

  atomic_fetch_add(&storage_engine.stats.cache_misses, 1);

  if (!entry->page)
  {
    entry->page = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
    if (!entry->page)
    {
      atomic_fetch_sub(&entry->ref_count, 1);
      return NULL;
    }
  }

  // Read page from disk
  off_t offset = page_id * PAGE_SIZE;
  if (pread(storage_engine.data_fd, entry->page, PAGE_SIZE, offset) != PAGE_SIZE)
  {
    // Page doesn´t exist - initialize new page
    memset(entry->page, 0, PAGE_SIZE);
    entry->page->header.page_id = page_id;
    entry->page->header.page_type = PAGE_TYPE_LEAF;
    entry->page->header.free_space = PAGE_SIZE - sizeof(page_header_t);
    entry->dirty = true;
  }

  atomic_fetch_add(&storage_engine.stats.pages_read, 1);

  // Verify checksum if enabled
  if (storage_engine.config.enable_checksums)
  {
    uint32_t stored_checksum = entry->page->header.checksum;
    entry->page->header.checksum = 0;
    uint32_t calculated_checksum = calculate_checksum(entry->page, PAGE_SIZE);
    entry->page->header.checksum = stored_checksum;

    if (stored_checksum != 0 && stored_checksum != calculated_checksum)
    {
      printf("Checksum mismatch for page %u\n", page_id);
      atomic_fetch_sub(&entry->ref_count, 1);
      return NULL;
    }
  }

  // Acquire page lock
  if (lock_type == LOCK_SHARED)
  {
    pthread_rwlock_rdlock(&entry->page_lock);
  }
  else if (lock_type == LOCK_EXCLUSIVE)
  {
    pthread_rwlock_wrlock(&entry->page_lock);
  }

  return entry->page;
}

void release_page(uint32_t page_id, lock_type_t lock_type)
{
  buffer_entry_t *entry = find_buffer_entry(page_id);
  if (!entry)
  {
    return;
  }

  if (lock_type == LOCK_SHARED || lock_type == LOCK_EXCLUSIVE)
  {
    pthread_rwlock_unlock(&entry->page_lock);
  }

  atomic_fetch_sub(&entry->ref_count, 1);
}

void mark_page_dirty(uint32_t page_id)
{
  buffer_entry_t *entry = find_buffer_entry(page_id);
  if (entry)
  {
    entry->dirty = true;

    // Update LSN
    if (storage_engine.config.enable_wal)
    {
      entry->page->header.lsn = storage_engine.next_lsn - 1;
    }

    // Update checksum
    if (storage_engine.config.enable_checksums)
    {
      entry->page->header.checksum = 0;
      entry->page->header.checksum = calculate_checksum(entry->page, PAGE_SIZE);
    }
  }
}

uint32_t allocate_page(void)
{
  pthread_mutex_lock(&storage_engine.free_page_mutex);

  uint32_t page_id;

  if (storage_engine.free_page_count > 0)
  {
    // Reuse a free page
    page_id = storage_engine.free_pages[--storage_engine.free_page_count];
  }
  else
  {
    page_id = storage_engine.next_page_id++;
  }

  pthread_mutex_unlock(&storage_engine.free_page_mutex);

  return page_id;
}

void deallocate_page(uint32_t page_id)
{
  pthread_mutex_lock(&storage_engine.free_page_mutex);

  // Grow free page array if necessary
  if (storage_engine.free_page_count >= storage_engine.free_page_capacity)
  {
    size_t new_capacity = storage_engine.free_page_capacity * 2;
    if (new_capacity == 0)
    {
      new_capacity = 1024;
    }

    uint32_t *new_array = realloc(storage_engine.free_pages, new_capacity * sizeof(uint32_t));
    if (new_array)
    {
      storage_engine.free_pages = new_array;
      storage_engine.free_page_capacity = new_capacity;
    }
  }

  if (storage_engine.free_page_count < storage_engine.free_page_capacity)
  {
    storage_engine.free_pages[storage_engine.free_page_count++] = page_id;
  }

  pthread_mutex_unlock(&storage_engine.free_page_mutex);
}

int compare_keys(const char *key1, size_t len1, const char *key2, size_t len2)
{
  size_t min_len = len1 < len2 ? len1 : len2;
  int result = memcmp(key1, key2, min_len);

  if (result == 0)
  {
    if (len1 < len2)
      return -1;
    if (len1 > len2)
      return 1;
    return 0;
  }

  return result;
}

kv_pair_t *get_kv_pair(btree_page_t *page, int index)
{
  if (index < 0 || index >= page->header.key_count)
  {
    return NULL;
  }

  // Find the key-value pair at the given index
  char *data_ptr = page->data;

  for (int i = 0; i <= index; i++)
  {
    if (i == index)
    {
      return (kv_pair_t *)data_ptr;
    }

    kv_pair_t *kv = (kv_pair_t *)data_ptr;
    data_ptr += sizeof(kv_pair_t) + kv->key_length + kv->value_length;
  }

  return NULL;
}

int find_key_position(btree_page_t *page, const char *key, size_t key_length)
{
  int left = 0;
  int right = page->header.key_count - 1;

  while (left <= right)
  {
    int mid = (left + right) / 2;
    kv_pair_t *kv = get_kv_pair(page, mid);

    if (!kv)
    {
      break;
    }

    const char *kv_key = kv->data;
    int cmp = compare_keys(key, key_length, kv_key, kv->key_length);

    if (cmp == 0)
    {
      return mid; // Exact match
    }
    else if (cmp < 0)
    {
      right = mid - 1;
    }
    else
    {
      left = mid + 1;
    }
  }

  return left;
}

int insert_kv_pair(
    btree_page_t *page,
    int position,
    const char *key,
    size_t key_length,
    const char *value,
    size_t value_length,
    uint32_t child_page_id)
{
  size_t pair_size = sizeof(kv_pair_t) + key_length + value_length;

  if (page->header.free_space < pair_size)
  {
    return -1; // Not enough size
  }

  // Find insertion point
  char *insert_ptr = page->data;
  for (int i = 0; i < position; i++)
  {
    kv_pair_t *kv = (kv_pair_t *)insert_ptr;
    insert_ptr += sizeof(kv_pair_t) + kv->key_length + kv->value_length;
  }

  // Calculate space neede to move existing data
  char *end_ptr = page->data + (PAGE_SIZE - sizeof(page_header_t)) + page->header.free_space;
  size_t move_size = end_ptr - insert_ptr;

  // Move existing data to make room
  if (move_size > 0)
  {
    memmove(insert_ptr + pair_size, insert_ptr, move_size);
  }

  // Move existing new key-value pair
  kv_pair_t *new_kv = (kv_pair_t *)insert_ptr;
  new_kv->key_length = key_length;
  new_kv->value_length = value_length;
  new_kv->child_page_id = child_page_id;

  memcpy(new_kv->data, key, key_length);
  memcpy(new_kv->data + key_length, value, value_length);

  page->header.key_count++;
  page->header.free_space -= pair_size;

  return 0;
}

int delete_kv_pair(btree_page_t *page, int position)
{
  if (position < 0 || position >= page->header.key_count)
  {
    return -1;
  }

  kv_pair_t *kv = get_kv_pair(page, position);
  if (!kv)
  {
    return -1;
  }

  size_t pair_size = sizeof(kv_pair_t) + kv->key_length + kv->value_length;

  // Calculate data to move
  char *delete_ptr = (char *)kv;
  char *next_ptr = delete_ptr + pair_size;
  char *end_ptr = page->data + (PAGE_SIZE - sizeof(page_header_t) - page->header.free_space);
  size_t move_size = end_ptr - next_ptr;

  // Move data to close the gap
  if (move_size > 0)
  {
    memmove(delete_ptr, next_ptr, move_size);
  }

  page->header.key_count--;
  page->header.free_space += pair_size;

  return 0;
}

transaction_t *begin_transaction(void)
{
  transaction_t *txn = malloc(sizeof(transaction_t));
  if (!txn)
  {
    return NULL;
  }

  memset(txn, 0, sizeof(*txn));

  pthread_mutex_lock(&storage_engine.txn_mutex);

  txn->txn_id = storage_engine.next_txn_id++;
  txn->state = TXN_STATE_ACTIVE;
  txn->start_time = time(NULL);
  txn->start_lsn = storage_engine.next_lsn;

  pthread_mutex_init(&txn->lock_mutex, NULL);

  // Add to active transactions list
  txn->next = storage_engine.active_transactions;
  storage_engine.active_transactions = txn;

  pthread_mutex_unlock(&storage_engine.txn_mutex);

  printf("Transaction %lu started\n", txn->txn_id);

  return txn;
}

int commit_transaction(transaction_t *txn)
{
  if (!txn || txn->state != TXN_STATE_ACTIVE)
  {
    return -1;
  }

  pthread_mutex_lock(&txn->lock_mutex);

  // Write commit record to WAL
  if (storage_engine.config.enable_wal)
  {
    write_wal_record(txn->txn_id, WAL_COMMIT, 0, NULL, 0);
    flush_wal_buffer();
  }

  txn->state = TXN_STATE_COMMITED;
  txn->commit_time = time(NULL);
  txn->commit_lsn = storage_engine.next_lsn - 1;

  // Release all locks
  lock_entry_t *lock = txn->locks;
  while (lock)
  {
    lock_entry_t *next_lock = lock->next_in_txn;
    free(lock);
    lock = next_lock;
  }
  txn->locks = NULL;

  pthread_mutex_unlock(&txn->lock_mutex);

  atomic_fetch_add(&storage_engine.stats.transactions_committed, 1);

  printf("Transaction %lu committed\n", txn->txn_id);

  return 0;
}

int abort_transaction(transaction_t *txn)
{
  if (!txn || txn->state != TXN_STATE_ACTIVE)
  {
    return -1;
  }

  pthread_mutex_lock(&txn->lock_mutex);

  // Apply undo operations in reverse order
  undo_entry_t *undo = txn->undo_log;
  while (undo)
  {
    // TODO: Implement undo logic here
    // THis would restore the old values

    undo_entry_t *next_undo = undo->next;
    free(undo->key_data);
    free(undo->old_value_data);
    free(undo);
    undo = next_undo;
  }
  txn->undo_log = NULL;

  // Write abort record to WAL
  if (storage_engine.config.enable_wal)
  {
    write_wal_record(txn->txn_id, WAL_ABORT, 0, NULL, 0);
  }

  txn->state = TXN_STATE_ABORTED;

  // Release al llocks
  lock_entry_t *lock = txn->locks;
  while (lock)
  {
    lock_entry_t *next_lock = lock->next_in_txn;
    free(lock);
    lock = next_lock;
  }
  txn->locks = NULL;

  pthread_mutex_unlock(&txn->lock_mutex);

  atomic_fetch_add(&storage_engine.stats.transactions_aborted, 1);

  printf("Transaction %lu aborted\n", txn->txn_id);

  return 0;
}

// High-level database operations
int db_insert(transaction_t *txn, const char *key, size_t key_length, const char *value, size_t value_length)
{
  if (!txn || txn->state != TXN_STATE_ACTIVE)
  {
    return -1;
  }

  uint32_t page_id = storage_engine.root_page_id;
  btree_page_t *page = get_page(page_id, LOCK_EXCLUSIVE);
  if (!page)
  {
    return -1;
  }

  while (page->header.page_type == PAGE_TYPE_INTERNAL)
  {
    int pos = find_key_position(page, key, key_length);
    kv_pair_t *kv = get_kv_pair(page, pos);

    uint32_t child_page_id = kv ? kv->child_page_id : page->header.next_page_id;

    release_page(page_id, LOCK_EXCLUSIVE);
    page_id = child_page_id;
    page = get_page(page_id, LOCK_EXCLUSIVE);

    if (!page)
    {
      return -1;
    }
  }

  // Check if the key already exists
  int pos = find_key_position(page, key, key_length);
  kv_pair_t *existing = get_kv_pair(page, pos);

  if (existing && compare_keys(key, key_length, existing->data, existing->key_length) == 0)
  {
    release_page(page_id, LOCK_EXCLUSIVE);
    return -1; // Key Already exists
  }

  // Write WAL record
  if (storage_engine.config.enable_wal)
  {
    char wal_data[MAX_KEY_SIZE + MAX_VALUE_SIZE + 8];
    size_t wal_size = 0;

    memcpy(wal_data + wal_size, &key_length, sizeof(key_length));
    wal_size += sizeof(key_length);

    memcpy(wal_data + wal_size, &value_length, sizeof(value_length));
    wal_size += sizeof(value_length);

    memcpy(wal_data + wal_size, key, key_length);
    wal_size += key_length;

    memcpy(wal_data + wal_size, value, value_length);
    wal_size += value_length;

    write_wal_record(txn->txn_id, WAL_INSERT, page_id, wal_data, wal_size);
  }

  // Insert key-value pair
  if (insert_kv_pair(page, pos, key, key_length, value, value_length, 0) == 0)
  {
    mark_page_dirty(page_id);
    txn->stats.rows_inserted++;

    printf("Inserted key-value pair in transaction %lu\n", txn->txn_id);
  }

  release_page(page_id, LOCK_EXCLUSIVE);

  return 0;
}

int db_search(transaction_t *txn, const char *key, size_t key_length, char *value, size_t *value_length)
{
  if (!txn || txn->state != TXN_STATE_ACTIVE)
  {
    return -1;
  }

  uint32_t page_id = storage_engine.root_page_id;
  btree_page_t *page = get_page(page_id, LOCK_EXCLUSIVE);
  if (!page)
  {
    return -1;
  }

  while (page->header.page_type == PAGE_TYPE_INTERNAL)
  {
    int pos = find_key_position(page, key, key_length);
    kv_pair_t *kv = get_kv_pair(page, pos);

    uint32_t child_page_id = kv ? kv->child_page_id : page->header.next_page_id;

    release_page(page_id, LOCK_EXCLUSIVE);
    page_id = child_page_id;
    page = get_page(page_id, LOCK_EXCLUSIVE);

    if (!page)
    {
      return -1;
    }
  }

  // Check if the key already exists
  int pos = find_key_position(page, key, key_length);
  kv_pair_t *kv = get_kv_pair(page, pos);

  if (kv && compare_keys(key, key_length, kv->data, kv->key_length) == 0)
  {
    // Found the key
    const char *kv_value = kv->data + kv->key_length;
    size_t copy_length = kv->value_length < *value_length ? kv->value_length : *value_length;

    memcpy(value, kv_value, copy_length);
    *value_length = kv->value_length;

    release_page(page_id, LOCK_SHARED);

    printf("Found key in transaction %lu\n", txn->txn_id);
    return 0;
  }

  release_page(page_id, LOCK_SHARED);
  return -1; // Key not found
}

int db_update(transaction_t *txn, const char *key, size_t key_length, const char *new_value, size_t new_value_length)
{
  if (!txn || txn->state != TXN_STATE_ACTIVE)
  {
    return -1;
  }

  // Find the key first (similar to search)
  uint32_t page_id = storage_engine.root_page_id;
  btree_page_t *page = get_page(page_id, LOCK_EXCLUSIVE);
  if (!page)
  {
    return -1;
  }

  // Navigate to leaf page
  while (page->header.page_type == PAGE_TYPE_INTERNAL)
  {
    int pos = find_key_position(page, key, key_length);
    kv_pair_t *kv = get_kv_pair(page, pos);

    uint32_t child_page_id = kv ? kv->child_page_id : page->header.next_page_id;

    release_page(page_id, LOCK_EXCLUSIVE);
    page_id = child_page_id;
    page = get_page(page_id, LOCK_EXCLUSIVE);

    if (!page)
    {
      return -1;
    }
  }

  // Find key in leaf page
  int pos = find_key_position(page, key, key_length);
  kv_pair_t *kv = get_kv_pair(page, pos);

  if (!kv || compare_keys(key, key_length, kv->data, kv->key_length) != 0)
  {
    release_page(page_id, LOCK_EXCLUSIVE);
    return -1; // Key not found
  }

  // Save old value for undo log
  char *old_value = malloc(kv->key_length);
  if (old_value)
  {
    memcpy(old_value, kv->data + kv->key_length, kv->value_length);

    undo_entry_t *undo = malloc(sizeof(undo_entry_t));
    if (undo)
    {
      undo->operation = WAL_UPDATE;
      undo->page_id = page_id;
      undo->slot_id = pos;
      undo->key_length = key_length;
      undo->old_value_length = kv->value_length;
      undo->key_data = malloc(key_length);
      undo->old_value_data = old_value;

      if (undo->key_data)
      {
        memcpy(undo->key_data, key, key_length);
      }

      undo->next = txn->undo_log;
      txn->undo_log = undo;
      txn->undo_count++;
    }
  }

  // Write WAL record
  if (storage_engine.config.enable_wal)
  {
    char wal_data[MAX_KEY_SIZE + MAX_VALUE_SIZE * 2 + 16];
    size_t wal_size = 0;

    memcpy(wal_data + wal_size, &key_length, sizeof(key_length));
    wal_size += sizeof(key_length);

    uint16_t old_value_length = kv->value_length;
    memcpy(wal_data + wal_size, &old_value_length, sizeof(old_value_length));
    wal_size += sizeof(old_value_length);

    memcpy(wal_data + wal_size, &new_value_length, sizeof(new_value_length));
    wal_size += sizeof(new_value_length);

    memcpy(wal_data + wal_size, key, key_length);
    wal_size += sizeof(key_length);

    memcpy(wal_data + wal_size, kv->data + kv->key_length, old_value_length);
    wal_size += old_value_length;

    memcpy(wal_data + wal_size, new_value, new_value_length);
    wal_size += new_value_length;

    write_wal_record(txn->txn_id, WAL_UPDATE, page_id, wal_data, wal_size);
  }

  // Update the alue (simplified - assumes same size)
  if (kv->value_length == new_value_length)
  {
    memcpy(kv->data + kv->key_length, new_value, new_value_length);
    mark_page_dirty(page_id);
    txn->stats.rows_updated++;

    printf("Updated key in transaction %lu\n", txn->txn_id);
  }

  release_page(page_id, LOCK_EXCLUSIVE);

  return 0;
}

int db_delete(transaction_t *txn, const char *key, size_t key_length)
{
  if (!txn || txn->state != TXN_STATE_ACTIVE)
  {
    return -1;
  }

  // Find the key first (similar to search)
  uint32_t page_id = storage_engine.root_page_id;
  btree_page_t *page = get_page(page_id, LOCK_EXCLUSIVE);
  if (!page)
  {
    return -1;
  }

  // Navigate to leaf page
  while (page->header.page_type == PAGE_TYPE_INTERNAL)
  {
    int pos = find_key_position(page, key, key_length);
    kv_pair_t *kv = get_kv_pair(page, pos);

    uint32_t child_page_id = kv ? kv->child_page_id : page->header.next_page_id;

    release_page(page_id, LOCK_EXCLUSIVE);
    page_id = child_page_id;
    page = get_page(page_id, LOCK_EXCLUSIVE);

    if (!page)
    {
      return -1;
    }
  }

  // Find key in leaf page
  int pos = find_key_position(page, key, key_length);
  kv_pair_t *kv = get_kv_pair(page, pos);

  if (!kv || compare_keys(key, key_length, kv->data, kv->key_length) != 0)
  {
    release_page(page_id, LOCK_EXCLUSIVE);
    return -1; // Key not found
  }

  // Write WAL record
  if (storage_engine.config.enable_wal)
  {
    char wal_data[MAX_KEY_SIZE + MAX_VALUE_SIZE + 8];
    size_t wal_size = 0;

    memcpy(wal_data + wal_size, &key_length, sizeof(key_length));
    wal_size += sizeof(key_length);

    uint16_t old_value_length = kv->value_length;
    memcpy(wal_data + wal_size, &old_value_length, sizeof(old_value_length));
    wal_size += sizeof(old_value_length);

    memcpy(wal_data + wal_size, key, key_length);
    wal_size += sizeof(key_length);

    memcpy(wal_data + wal_size, kv->data + kv->key_length, old_value_length);
    wal_size += old_value_length;

    write_wal_record(txn->txn_id, WAL_UPDATE, page_id, wal_data, wal_size);
  }

  // Delete the key-value pair
  if (delete_kv_pair(page, pos) == 0)
  {
    mark_page_dirty(page_id);
    txn->stats.rows_deleted++;

    printf("Deleted key in transaction %lu\n", txn->txn_id);
  }

  release_page(page_id, LOCK_EXCLUSIVE);

  return 0;
}
