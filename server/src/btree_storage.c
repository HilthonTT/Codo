#define _GNU_SOURCE

#include <sys/types.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <errno.h>

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

  // The record is the full wal_record_t header followed by data_length payload
  // bytes -- NOT just the type field. Using sizeof(wal_record_type_t) here made
  // successive records overlap and land on unaligned offsets (the header has
  // uint64_t fields), corrupting the WAL and tripping UBSan.
  size_t content_size = sizeof(wal_record_t) + data_length;
  // Round the stride up to 8 bytes so each record's header stays aligned
  // (wal_buffer is malloc'd and wal_buffer_pos starts at 0).
  size_t record_size = (content_size + 7u) & ~(size_t)7u;

  // Guard against records that cannot fit even in a freshly flushed buffer.
  // Without this the memcpy below would overflow the fixed WAL_BUFFER_SIZE
  // heap buffer for a large data_length.
  if (record_size > WAL_BUFFER_SIZE)
  {
    pthread_mutex_unlock(&storage_engine.wal_mutex);
    return -1;
  }

  // Check if we need to flush the buffer
  if (storage_engine.wal_buffer_pos + record_size > WAL_BUFFER_SIZE)
  {
    // Write buffer to disk with EINTR/partial-write retry. On failure we must
    // leave wal_buffer_pos unchanged so the next flush does not re-append the
    // already-written prefix (which would duplicate records).
    size_t total_written = 0;
    while (total_written < storage_engine.wal_buffer_pos)
    {
      ssize_t bytes_written = write(storage_engine.wal_fd,
                                    storage_engine.wal_buffer + total_written,
                                    storage_engine.wal_buffer_pos - total_written);
      if (bytes_written < 0)
      {
        if (errno == EINTR)
          continue;
        pthread_mutex_unlock(&storage_engine.wal_mutex);
        return -1;
      }
      if (bytes_written == 0)
      {
        pthread_mutex_unlock(&storage_engine.wal_mutex);
        return -1;
      }
      total_written += (size_t)bytes_written;
    }

    storage_engine.wal_buffer_pos = 0;
    fsync(storage_engine.wal_fd);
  }

  // Create WAL record in the WAL buffer (not the page buffer pool).
  wal_record_t *record = (wal_record_t *)(storage_engine.wal_buffer + storage_engine.wal_buffer_pos);
  record->lsn = allocate_lsn();
  record->txn_id = txn_id;
  record->type = type;
  record->page_id = page_id;
  record->data_length = data_length;

  if (data && data_length > 0)
  {
    memcpy(record->data, data, data_length);
  }

  // Zero any alignment padding so we never flush uninitialized bytes to disk.
  if (record_size > content_size)
  {
    memset((uint8_t *)record + content_size, 0, record_size - content_size);
  }

  // Checksum the real content only (padding excluded), matching what a reader
  // would recompute over header+payload.
  record->checksum = calculate_checksum(record, content_size - sizeof(record->checksum));

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
    size_t total_written = 0;
    while (total_written < storage_engine.wal_buffer_pos)
    {
      ssize_t bytes_written = write(storage_engine.wal_fd,
                                    storage_engine.wal_buffer + total_written,
                                    storage_engine.wal_buffer_pos - total_written);
      if (bytes_written < 0)
      {
        if (errno == EINTR)
          continue;
        pthread_mutex_unlock(&storage_engine.wal_mutex);
        return -1;
      }
      if (bytes_written == 0)
      {
        pthread_mutex_unlock(&storage_engine.wal_mutex);
        return -1;
      }
      total_written += (size_t)bytes_written;
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
  // NOTE: the caller MUST already hold buffer_mutex. This selects and evicts an
  // LRU victim and initializes it for page_id, but deliberately does NOT insert
  // the entry into the hash table -- get_page publishes it only after the page
  // has been fully loaded and checksum-verified. Keeping the whole
  // lookup -> pin / allocate -> load sequence under buffer_mutex prevents the
  // evictor from reclaiming a just-found entry, stops two threads from both
  // creating an entry for the same page, and guarantees no other thread ever
  // observes a half-loaded page in the hash table.

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
    return NULL; // Buffer pool full
  }

  // Evict victim if dirty
  if (victim->dirty && victim->page)
  {
    // Write-ahead invariant: flush the WAL to disk before writing the dirty
    // page back, so the log records covering these changes are durable first.
    flush_wal_buffer();

    // Write page to disk (retry on partial writes / EINTR). Cast page_id to
    // off_t *before* the multiply so the offset is computed in 64 bits.
    off_t offset = (off_t)victim->page_id * PAGE_SIZE;
    size_t total_written = 0;
    while (total_written < PAGE_SIZE)
    {
      ssize_t w = pwrite(storage_engine.data_fd,
                         (const char *)victim->page + total_written,
                         PAGE_SIZE - total_written,
                         offset + total_written);
      if (w < 0)
      {
        if (errno == EINTR)
          continue;
        return NULL;
      }
      if (w == 0)
      {
        return NULL;
      }
      total_written += (size_t)w;
    }
    atomic_fetch_add(&storage_engine.stats.pages_written, 1);
    victim->dirty = false;
  }

  // Remove from hash table
  if (victim->page_id != 0)
  {
    uint32_t hash = hash_page_id(victim->page_id);
    uint32_t *current = &storage_engine.hash_table[hash];

    while (*current != UINT32_MAX)
    {
      if (*current == (uint32_t)(victim - storage_engine.buffer_pool))
      {
        *current = victim->hash_next;
        break;
      }
      current = &storage_engine.buffer_pool[*current].hash_next;
    }
  }

  // Initialize new entry. It is NOT yet linked into the hash table: get_page
  // links it only after the page load + checksum succeed.
  victim->page_id = page_id;
  victim->dirty = false;
  victim->pinned = false;
  atomic_store(&victim->ref_count, 1);
  victim->hash_next = UINT32_MAX;
  clock_gettime(CLOCK_MONOTONIC, &victim->last_access);

  return victim;
}

btree_page_t *get_page(uint32_t page_id, lock_type_t lock_type)
{
  // The whole lookup + pin (hit path) or allocate + load + publish (miss path)
  // runs under buffer_mutex. This closes the eviction races: a found entry is
  // pinned before the lock drops (so the evictor cannot reclaim it), two
  // threads missing the same page cannot both create an entry (the second one
  // finds the first), and an entry is not visible in the hash table until its
  // page is fully loaded and checksum-verified.
  pthread_mutex_lock(&storage_engine.buffer_mutex);

  // Check buffer pool first
  buffer_entry_t *entry = find_buffer_entry(page_id);

  if (entry)
  {
    atomic_fetch_add(&entry->ref_count, 1);
    clock_gettime(CLOCK_MONOTONIC, &entry->last_access);
    atomic_fetch_add(&storage_engine.stats.cache_hits, 1);

    pthread_mutex_unlock(&storage_engine.buffer_mutex);

    // Acquire page lock (safe without buffer_mutex: the entry is pinned)
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

  // Page not in buffer pool - allocate new entry (buffer_mutex held).
  entry = allocate_buffer_entry(page_id);
  if (!entry)
  {
    pthread_mutex_unlock(&storage_engine.buffer_mutex);
    return NULL;
  }

  atomic_fetch_add(&storage_engine.stats.cache_misses, 1);

  if (!entry->page)
  {
    entry->page = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
    if (!entry->page)
    {
      atomic_fetch_sub(&entry->ref_count, 1);
      pthread_mutex_unlock(&storage_engine.buffer_mutex);
      return NULL;
    }
  }

  // Read page from disk. Cast page_id to off_t before the multiply so the
  // offset is computed in 64 bits.
  off_t offset = (off_t)page_id * PAGE_SIZE;
  ssize_t bytes_read = pread(storage_engine.data_fd, entry->page, PAGE_SIZE, offset);
  if (bytes_read != PAGE_SIZE)
  {
    // Distinguish a legitimately fresh page (beyond the current end of file)
    // from a read error / short read on a page that should already exist. In
    // the latter case we must NOT zero-and-keep the page: a later eviction
    // would pwrite the empty page over real on-disk data. Fail instead and
    // leave the on-disk data intact. The entry is not yet published in the
    // hash table, so releasing its pin returns the slot to the free pool.
    off_t file_size = 0;
    struct stat st;
    if (fstat(storage_engine.data_fd, &st) == 0)
    {
      file_size = st.st_size;
    }

    if (bytes_read < 0 || offset < file_size)
    {
      atomic_fetch_sub(&entry->ref_count, 1);
      pthread_mutex_unlock(&storage_engine.buffer_mutex);
      return NULL;
    }

    // Page lies beyond the end of file -> initialize a new (fresh) page.
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
      // The entry was never linked into the hash table, so a corrupt page is
      // simply discarded here (ref_count back to 0) and can never be served
      // from the cache on a later get_page.
      atomic_fetch_sub(&entry->ref_count, 1);
      pthread_mutex_unlock(&storage_engine.buffer_mutex);
      return NULL;
    }
  }

  // Publish the fully-loaded entry into the hash table.
  uint32_t hash = hash_page_id(page_id);
  entry->hash_next = storage_engine.hash_table[hash];
  storage_engine.hash_table[hash] = (uint32_t)(entry - storage_engine.buffer_pool);

  pthread_mutex_unlock(&storage_engine.buffer_mutex);

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
  // Look up under buffer_mutex so the hash chain is not rewired by a concurrent
  // evictor while we traverse it. The entry is pinned by the caller, so it will
  // still be present.
  pthread_mutex_lock(&storage_engine.buffer_mutex);
  buffer_entry_t *entry = find_buffer_entry(page_id);
  pthread_mutex_unlock(&storage_engine.buffer_mutex);
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
  // Look up under buffer_mutex (see release_page). The caller holds the page's
  // write lock and a pin, so the entry cannot be evicted here.
  pthread_mutex_lock(&storage_engine.buffer_mutex);
  buffer_entry_t *entry = find_buffer_entry(page_id);
  pthread_mutex_unlock(&storage_engine.buffer_mutex);
  if (entry)
  {
    entry->dirty = true;

    // Update LSN
    if (storage_engine.config.enable_wal)
    {
      entry->page->header.lsn = atomic_load(&storage_engine.next_lsn) - 1;
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

  // Find the key-value pair at the given index. The on-disk key/value lengths
  // are untrusted, so validate that every pair (header + key + value) stays
  // within the page's data area before dereferencing it -- a corrupt page must
  // not cause an out-of-bounds read.
  const size_t data_capacity = PAGE_SIZE - sizeof(page_header_t);
  char *data_ptr = page->data;
  size_t offset = 0;

  for (int i = 0; i <= index; i++)
  {
    // Need at least the fixed-size header to read the lengths.
    if (offset + sizeof(kv_pair_t) > data_capacity)
    {
      return NULL; // Corrupt page
    }

    kv_pair_t *kv = (kv_pair_t *)data_ptr;

    if (i == index)
    {
      // Also verify this pair's payload fits before handing it back.
      size_t pair_size = sizeof(kv_pair_t) + kv->key_length + kv->value_length;
      if (offset + pair_size > data_capacity)
      {
        return NULL; // Corrupt page
      }
      return kv;
    }

    size_t pair_size = sizeof(kv_pair_t) + kv->key_length + kv->value_length;
    if (pair_size > data_capacity - offset)
    {
      return NULL; // Corrupt page
    }

    data_ptr += pair_size;
    offset += pair_size;
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

  // Guard against a corrupt free_space that would place end_ptr out of bounds.
  const size_t data_capacity = PAGE_SIZE - sizeof(page_header_t);
  if (page->header.free_space > data_capacity)
  {
    return -1; // Corrupt page
  }
  size_t used = data_capacity - page->header.free_space;

  // Find insertion point, validating each traversed pair stays within the
  // used region (untrusted on-disk lengths).
  char *insert_ptr = page->data;
  size_t insert_offset = 0;
  for (int i = 0; i < position; i++)
  {
    if (insert_offset + sizeof(kv_pair_t) > used)
    {
      return -1; // Corrupt page
    }
    kv_pair_t *kv = (kv_pair_t *)insert_ptr;
    size_t sz = sizeof(kv_pair_t) + kv->key_length + kv->value_length;
    if (sz > used - insert_offset)
    {
      return -1; // Corrupt page
    }
    insert_ptr += sz;
    insert_offset += sz;
  }

  // Calculate space needed to move existing data. end_ptr is the end of the
  // currently-used region (capacity minus free space), so we shift only the
  // pairs that sit after the insertion point.
  char *end_ptr = page->data + used;
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

  // Guard against a corrupt free_space that would make end_ptr precede
  // next_ptr, which would underflow move_size into a huge memmove.
  const size_t data_capacity = PAGE_SIZE - sizeof(page_header_t);
  if (page->header.free_space > data_capacity)
  {
    return -1; // Corrupt page
  }
  size_t used = data_capacity - page->header.free_space;

  // Calculate data to move
  char *delete_ptr = (char *)kv;
  char *next_ptr = delete_ptr + pair_size;
  char *end_ptr = page->data + used;
  if (next_ptr > end_ptr)
  {
    return -1; // Corrupt page
  }
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
  txn->start_lsn = atomic_load(&storage_engine.next_lsn);

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
  txn->commit_lsn = atomic_load(&storage_engine.next_lsn) - 1;

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

  // Unlink from the active_transactions list before the caller free()s the
  // txn, otherwise the global list would hold a dangling pointer (any later
  // traversal, e.g. print_storage_statistics, would be a use-after-free).
  pthread_mutex_lock(&storage_engine.txn_mutex);
  transaction_t **pp = &storage_engine.active_transactions;
  while (*pp)
  {
    if (*pp == txn)
    {
      *pp = txn->next;
      break;
    }
    pp = &(*pp)->next;
  }
  txn->next = NULL;
  pthread_mutex_unlock(&storage_engine.txn_mutex);

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

  // Unlink from the active_transactions list before the caller free()s the
  // txn (see commit_transaction) to avoid a dangling pointer / use-after-free.
  pthread_mutex_lock(&storage_engine.txn_mutex);
  transaction_t **pp = &storage_engine.active_transactions;
  while (*pp)
  {
    if (*pp == txn)
    {
      *pp = txn->next;
      break;
    }
    pp = &(*pp)->next;
  }
  txn->next = NULL;
  pthread_mutex_unlock(&storage_engine.txn_mutex);

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

  // Validate lengths up front so the on-page pair and the WAL payload can never
  // exceed their fixed-size buffers.
  if (key_length > MAX_KEY_SIZE || value_length > MAX_VALUE_SIZE)
  {
    return -1;
  }

  // TODO(concurrency): descending internal nodes with LOCK_EXCLUSIVE
  // serializes all writers; switching internal-node descent to LOCK_SHARED
  // (crab-latching) would improve concurrency.
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

  // Write WAL record. The payload prefixes two size_t length fields, so the
  // buffer must reserve sizeof(size_t)*2 for them (not 8).
  if (storage_engine.config.enable_wal)
  {
    char wal_data[MAX_KEY_SIZE + MAX_VALUE_SIZE + sizeof(size_t) * 2];
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

  // Insert key-value pair. NOTE: B-tree page-split is not implemented, so a
  // leaf's capacity is bounded by a single page. If insert_kv_pair reports the
  // page is full (-1) we must propagate the failure rather than silently
  // returning success (which would lose the row and report a false 201).
  if (insert_kv_pair(page, pos, key, key_length, value, value_length, 0) != 0)
  {
    release_page(page_id, LOCK_EXCLUSIVE);
    return -1;
  }

  mark_page_dirty(page_id);
  txn->stats.rows_inserted++;

  printf("Inserted key-value pair in transaction %lu\n", txn->txn_id);

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
  char *old_value = malloc(kv->value_length);
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

    // Advance by key_length (the actual key bytes just copied), not by
    // sizeof(key_length) -- otherwise the value bytes land at the wrong offset.
    memcpy(wal_data + wal_size, key, key_length);
    wal_size += key_length;

    memcpy(wal_data + wal_size, kv->data + kv->key_length, old_value_length);
    wal_size += old_value_length;

    memcpy(wal_data + wal_size, new_value, new_value_length);
    wal_size += new_value_length;

    write_wal_record(txn->txn_id, WAL_UPDATE, page_id, wal_data, wal_size);
  }

  // Update the value in place. NOTE: this only supports a same-size update --
  // variable-length in-place resize (which could need a page-split) is not
  // implemented. If the new value differs in size we cannot update in place,
  // so propagate a failure instead of silently dropping the change.
  if (kv->value_length != new_value_length)
  {
    release_page(page_id, LOCK_EXCLUSIVE);
    return -1;
  }

  memcpy(kv->data + kv->key_length, new_value, new_value_length);
  mark_page_dirty(page_id);
  txn->stats.rows_updated++;

  printf("Updated key in transaction %lu\n", txn->txn_id);

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

  // Write WAL record. Buffer sized for the size_t + uint16_t length prefix
  // plus a full key and value.
  if (storage_engine.config.enable_wal)
  {
    char wal_data[MAX_KEY_SIZE + MAX_VALUE_SIZE + sizeof(size_t) + sizeof(uint16_t)];
    size_t wal_size = 0;

    memcpy(wal_data + wal_size, &key_length, sizeof(key_length));
    wal_size += sizeof(key_length);

    uint16_t old_value_length = kv->value_length;
    memcpy(wal_data + wal_size, &old_value_length, sizeof(old_value_length));
    wal_size += sizeof(old_value_length);

    // Advance by key_length (the actual key bytes), not sizeof(key_length),
    // so the old value lands at the correct offset.
    memcpy(wal_data + wal_size, key, key_length);
    wal_size += key_length;

    memcpy(wal_data + wal_size, kv->data + kv->key_length, old_value_length);
    wal_size += old_value_length;

    // This is a delete: log a WAL_DELETE record, not WAL_UPDATE.
    write_wal_record(txn->txn_id, WAL_DELETE, page_id, wal_data, wal_size);
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

int db_scan(transaction_t *txn, db_scan_callback_t callback, void *ctx)
{
  if (!txn || txn->state != TXN_STATE_ACTIVE || !callback)
  {
    return -1;
  }

  uint32_t page_id = storage_engine.root_page_id;
  btree_page_t *page = get_page(page_id, LOCK_SHARED);
  if (!page)
  {
    return -1;
  }

  // Descend to the leftmost leaf following the first child pointer.
  while (page->header.page_type == PAGE_TYPE_INTERNAL)
  {
    kv_pair_t *kv = get_kv_pair(page, 0);
    uint32_t child_page_id = kv ? kv->child_page_id : page->header.next_page_id;

    release_page(page_id, LOCK_SHARED);
    page_id = child_page_id;
    page = get_page(page_id, LOCK_SHARED);

    if (!page)
    {
      return -1;
    }
  }

  // Walk the linked list of leaf pages, visiting every key-value pair.
  while (page)
  {
    for (int i = 0; i < (int)page->header.key_count; i++)
    {
      kv_pair_t *kv = get_kv_pair(page, i);
      if (!kv)
      {
        break;
      }

      const char *key = kv->data;
      const char *value = kv->data + kv->key_length;

      if (callback(key, kv->key_length, value, kv->value_length, ctx) != 0)
      {
        release_page(page_id, LOCK_SHARED);
        return 0; // Caller requested early stop
      }
    }

    uint32_t next_page_id = page->header.next_page_id;
    release_page(page_id, LOCK_SHARED);

    if (next_page_id == 0)
    {
      break;
    }

    page_id = next_page_id;
    page = get_page(page_id, LOCK_SHARED);
  }

  return 0;
}

int perform_checkpoint(void)
{
  printf("Starting checkpoint...\n");

  atomic_fetch_add(&storage_engine.stats.checkpoints_performed, 1);

  // Flush WAL buffer
  flush_wal_buffer();

  // Write all dirty pages to disk
  pthread_mutex_lock(&storage_engine.buffer_mutex);

  for (size_t i = 0; i < BUFFER_POOL_SIZE; i++)
  {
    buffer_entry_t *entry = &storage_engine.buffer_pool[i];

    if (entry->dirty && entry->page)
    {
      off_t offset = (off_t)entry->page_id * PAGE_SIZE;
      if (pwrite(storage_engine.data_fd, entry->page, PAGE_SIZE, offset) == PAGE_SIZE)
      {
        entry->dirty = false;
        atomic_fetch_add(&storage_engine.stats.pages_written, 1);
      }
    }
  }

  pthread_mutex_unlock(&storage_engine.buffer_mutex);

  // Sync data file
  fsync(storage_engine.data_fd);

  // Write checkpoint record
  write_wal_record(0, WAL_CHECKOUT, 0, NULL, 0);
  flush_wal_buffer();

  storage_engine.last_checkpoint_lsn = atomic_load(&storage_engine.next_lsn) - 1;

  printf("Checkpoint completed (LSN: %lu)\n", storage_engine.last_checkpoint_lsn);

  return 0;
}

void print_storage_statistics(void)
{
  printf("\n=== Storage Engine Statistics ===\n");

  printf("Buffer Pool:\n");
  printf("  Pages read: %lu\n", atomic_load(&storage_engine.stats.pages_read));
  printf("  Pages written: %lu\n", atomic_load(&storage_engine.stats.pages_written));
  printf("  Cache hits: %lu\n", atomic_load(&storage_engine.stats.cache_hits));
  printf("  Cache misses: %lu\n", atomic_load(&storage_engine.stats.cache_misses));

  uint64_t total_accesses = atomic_load(&storage_engine.stats.cache_hits) +
                            atomic_load(&storage_engine.stats.cache_misses);
  if (total_accesses > 0)
  {
    double hit_ratio = (double)atomic_load(&storage_engine.stats.cache_hits) / total_accesses;
    printf("  Cache hit ratio: %.2f%%\n", hit_ratio * 100.0);
  }

  printf("\nTransactions:\n");
  printf("  Committed: %lu\n", atomic_load(&storage_engine.stats.transactions_committed));
  printf("  Aborted: %lu\n", atomic_load(&storage_engine.stats.transactions_aborted));

  printf("\nWAL:\n");
  printf("  Records written: %lu\n", atomic_load(&storage_engine.stats.wal_records_written));
  printf("  Checkpoints: %lu\n", atomic_load(&storage_engine.stats.checkpoints_performed));
  printf("  Next LSN: %lu\n", atomic_load(&storage_engine.next_lsn));
  printf("  Last checkpoint LSN: %lu\n", storage_engine.last_checkpoint_lsn);

  printf("\nActive Transactions:\n");
  pthread_mutex_lock(&storage_engine.txn_mutex);
  transaction_t *txn = storage_engine.active_transactions;
  int count = 0;
  while (txn)
  {
    printf("  TXN %lu: inserts=%lu, updates=%lu, deletes=%lu\n",
           txn->txn_id, txn->stats.rows_inserted,
           txn->stats.rows_updated, txn->stats.rows_deleted);
    txn = txn->next;
    count++;
  }
  printf("  Total active: %d\n", count);
  pthread_mutex_unlock(&storage_engine.txn_mutex);

  printf("=================================\n");
}

int init_storage_engine(const char *data_file, const char *wal_file)
{
  memset(&storage_engine, 0, sizeof(storage_engine));

  // Configuration
  storage_engine.config.enable_checksums = true;
  storage_engine.config.enable_wal = true;
  storage_engine.config.checkpoint_interval = 10000;
  storage_engine.config.wal_segment_size = 64 * 1024 * 1024;
  storage_engine.config.buffer_pool_hit_ratio_target = 0.95;

  // Open data file
  storage_engine.data_fd = open(data_file, O_RDWR | O_CREAT, 0644);
  if (storage_engine.data_fd < 0)
  {
    perror("open data file");
    return -1;
  }

  storage_engine.data_filename = strdup(data_file);

  // Open WAL file
  storage_engine.wal_fd = open(wal_file, O_RDWR | O_CREAT | O_APPEND, 0644);
  if (storage_engine.wal_fd < 0)
  {
    perror("open WAL file");
    close(storage_engine.data_fd);
    free(storage_engine.data_filename);
    return -1;
  }

  storage_engine.wal_filename = strdup(wal_file);

  // Initialize WAL buffer
  storage_engine.wal_buffer = malloc(WAL_BUFFER_SIZE);
  if (!storage_engine.wal_buffer)
  {
    close(storage_engine.data_fd);
    close(storage_engine.wal_fd);
    free(storage_engine.data_filename);
    free(storage_engine.wal_filename);
    return -1;
  }

  // initialize hash table for buffer pool

  storage_engine.hash_table_size = BUFFER_POOL_SIZE * 2;
  storage_engine.hash_table = malloc(storage_engine.hash_table_size * sizeof(uint32_t));
  if (!storage_engine.hash_table)
  {
    close(storage_engine.data_fd);
    close(storage_engine.wal_fd);
    free(storage_engine.data_filename);
    free(storage_engine.wal_filename);
    free(storage_engine.wal_buffer);
    return -1;
  }

  for (size_t i = 0; i < storage_engine.hash_table_size; i++)
  {
    storage_engine.hash_table[i] = UINT32_MAX;
  }

  // Initialize buffer pool
  for (size_t i = 0; i < BUFFER_POOL_SIZE; i++)
  {
    buffer_entry_t *entry = &storage_engine.buffer_pool[i];
    pthread_rwlock_init(&entry->page_lock, NULL);
    entry->hash_next = UINT32_MAX;
  }

  // Initialize lock table
  storage_engine.lock_table_size = 10007; // Prime number
  storage_engine.lock_table = calloc(storage_engine.lock_table_size, sizeof(lock_entry_t *));
  if (!storage_engine.lock_table)
  {
    close(storage_engine.data_fd);
    close(storage_engine.wal_fd);
    free(storage_engine.data_filename);
    free(storage_engine.wal_filename);
    free(storage_engine.wal_buffer);
    free(storage_engine.hash_table);
    for (size_t i = 0; i < BUFFER_POOL_SIZE; i++)
    {
      pthread_rwlock_destroy(&storage_engine.buffer_pool[i].page_lock);
    }
    return -1;
  }

  // Initialize mutexes
  pthread_mutex_init(&storage_engine.buffer_mutex, NULL);
  pthread_mutex_init(&storage_engine.free_page_mutex, NULL);
  pthread_mutex_init(&storage_engine.txn_mutex, NULL);
  pthread_mutex_init(&storage_engine.lock_table_mutex, NULL);
  pthread_mutex_init(&storage_engine.wal_mutex, NULL);

  // Initialize counters
  storage_engine.next_txn_id = 1;
  storage_engine.next_lsn = 1;
  storage_engine.next_page_id = 1;
  storage_engine.root_page_id = 1;

  // TODO(durability): WAL crash-recovery replay is NOT implemented. On restart
  // we do not scan the WAL to redo committed changes / undo uncommitted ones,
  // so any committed writes that were logged but not yet checkpointed to the
  // data file are lost after a crash. Eviction/checkpoint honour the
  // write-ahead ordering (WAL is flushed before dirty pages are written back),
  // but full recovery still needs a replay pass here.

  // Initialize root page if file is empty
  struct stat st;
  if (fstat(storage_engine.data_fd, &st) == 0 && st.st_size == 0)
  {
    btree_page_t *root_page = get_page(storage_engine.root_page_id, LOCK_EXCLUSIVE);
    if (root_page)
    {
      root_page->header.page_type = PAGE_TYPE_LEAF;
      mark_page_dirty(storage_engine.root_page_id);
      release_page(storage_engine.root_page_id, LOCK_EXCLUSIVE);
    }
  }

  printf("Storage engine initialized\n");
  printf("Data file: %s\n", data_file);
  printf("WAL file: %s\n", wal_file);

  return 0;
}

void cleanup_storage_engine(void)
{
  // Perform final checkpoint
  perform_checkpoint();

  // Close files
  if (storage_engine.data_fd >= 0)
  {
    close(storage_engine.data_fd);
  }

  if (storage_engine.wal_fd >= 0)
  {
    close(storage_engine.wal_fd);
  }

  // Free memory
  free(storage_engine.data_filename);
  free(storage_engine.wal_filename);
  free(storage_engine.wal_buffer);
  free(storage_engine.hash_table);
  free(storage_engine.free_pages);
  free(storage_engine.lock_table);

  // Cleanup buffer pool
  for (size_t i = 0; i < BUFFER_POOL_SIZE; i++)
  {
    buffer_entry_t *entry = &storage_engine.buffer_pool[i];
    if (entry->page)
    {
      free(entry->page);
    }
    pthread_rwlock_destroy(&entry->page_lock);
  }
}

// WARNING: this handler is async-signal-UNSAFE. It calls printf, the full
// cleanup_storage_engine path (malloc/free, pthread ops, fsync) and exit(),
// none of which are async-signal-safe. It must NOT be registered with
// signal()/sigaction() as-is -- doing so can deadlock or corrupt state if a
// signal arrives mid-allocation or while a mutex is held. A safe version would
// only set a volatile sig_atomic_t flag and let the main loop do the work.
// (Deliberately left unregistered; see the commented-out main().)
void signal_handler(int sig)
{
  if (sig == SIGINT || sig == SIGTERM)
  {
    printf("\nReceived signal %d, shutting down storage engine...\n", sig);
    cleanup_storage_engine();
    exit(0);
  }
  else if (sig == SIGUSR1)
  {
    print_storage_statistics();
  }
  else if (sig == SIGUSR2)
  {
    perform_checkpoint();
  }
}

// Test and demonstration
void test_storage_engine(void)
{
  printf("Testing storage engine...\n");

  // Create some test transactions
  transaction_t *txn1 = begin_transaction();
  transaction_t *txn2 = begin_transaction();

  if (txn1 && txn2)
  {
    // Test insertions
    db_insert(txn1, "key1", 4, "value1", 6);
    db_insert(txn1, "key2", 4, "value2", 6);
    db_insert(txn2, "key3", 4, "value3", 6);

    // Test searches
    char value[MAX_VALUE_SIZE];
    size_t value_length = sizeof(value);

    if (db_search(txn1, "key1", 4, value, &value_length) == 0)
    {
      printf("Found key1: %.*s\n", (int)value_length, value);
    }

    // Test updates
    db_update(txn1, "key1", 4, "updated_value1", 14);

    // Test deletion
    db_delete(txn2, "key3", 4);

    // Commit transactions
    commit_transaction(txn1);
    commit_transaction(txn2);

    free(txn1);
    free(txn2);
  }

  // Perform checkpoint
  perform_checkpoint();

  printf("Storage engine test completed\n");
}

// Main function
// int main(int argc, char* argv[])
// {
//     const char* data_file = "database.db";
//     const char* wal_file = "database.wal";

//     if (argc > 1) {
//         data_file = argv[1];
//     }

//     if (argc > 2) {
//         wal_file = argv[2];
//     }

//     // Set up signal handlers
//     signal(SIGINT, signal_handler);
//     signal(SIGTERM, signal_handler);
//     signal(SIGUSR1, signal_handler);
//     signal(SIGUSR2, signal_handler);

//     printf("Advanced Database Storage Engine\n");

//     // Initialize storage engine
//     if (init_storage_engine(data_file, wal_file) != 0) {
//         fprintf(stderr, "Failed to initialize storage engine\n");
//         return 1;
//     }

//     // Run tests
//     test_storage_engine();

//     printf("Storage engine running...\n");
//     printf("Send SIGUSR1 for statistics, SIGUSR2 for checkpoint, SIGINT to exit\n");

//     // Main loop
//     while (1) {
//         sleep(5);

//         // Automatic checkpoint if needed
//         if (storage_engine.next_lsn - storage_engine.last_checkpoint_lsn >
//             storage_engine.config.checkpoint_interval) {
//             perform_checkpoint();
//         }
//     }

//     cleanup_storage_engine();
//     return 0;
// }