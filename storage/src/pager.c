#define _GNU_SOURCE

// Pager: the buffer pool (hash-table lookup, LRU eviction, pinning and
// per-page rwlocks), page IO against the data file, free-page management and
// page checksums. Everything above this layer manipulates pages purely in
// memory via get_page/release_page.

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "storage_internal.h"

uint32_t hash_page_id(uint32_t page_id)
{
  // Simple hash function
  return page_id % g_storage.hash_table_size;
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

buffer_entry_t *find_buffer_entry(uint32_t page_id)
{
  uint32_t hash = hash_page_id(page_id);
  uint32_t entry_idx = g_storage.hash_table[hash];

  while (entry_idx != UINT32_MAX)
  {
    buffer_entry_t *entry = &g_storage.buffer_pool[entry_idx];
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
    buffer_entry_t *entry = &g_storage.buffer_pool[i];

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
      ssize_t w = pwrite(g_storage.data_fd,
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
    atomic_fetch_add(&g_storage.stats.pages_written, 1);
    victim->dirty = false;
  }

  // Remove from hash table
  if (victim->page_id != 0)
  {
    uint32_t hash = hash_page_id(victim->page_id);
    uint32_t *current = &g_storage.hash_table[hash];

    while (*current != UINT32_MAX)
    {
      if (*current == (uint32_t)(victim - g_storage.buffer_pool))
      {
        *current = victim->hash_next;
        break;
      }
      current = &g_storage.buffer_pool[*current].hash_next;
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
  pthread_mutex_lock(&g_storage.buffer_mutex);

  // Check buffer pool first
  buffer_entry_t *entry = find_buffer_entry(page_id);

  if (entry)
  {
    atomic_fetch_add(&entry->ref_count, 1);
    clock_gettime(CLOCK_MONOTONIC, &entry->last_access);
    atomic_fetch_add(&g_storage.stats.cache_hits, 1);

    pthread_mutex_unlock(&g_storage.buffer_mutex);

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
    pthread_mutex_unlock(&g_storage.buffer_mutex);
    return NULL;
  }

  atomic_fetch_add(&g_storage.stats.cache_misses, 1);

  if (!entry->page)
  {
    entry->page = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
    if (!entry->page)
    {
      atomic_fetch_sub(&entry->ref_count, 1);
      pthread_mutex_unlock(&g_storage.buffer_mutex);
      return NULL;
    }
  }

  // Read page from disk. Cast page_id to off_t before the multiply so the
  // offset is computed in 64 bits.
  off_t offset = (off_t)page_id * PAGE_SIZE;
  ssize_t bytes_read = pread(g_storage.data_fd, entry->page, PAGE_SIZE, offset);
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
    if (fstat(g_storage.data_fd, &st) == 0)
    {
      file_size = st.st_size;
    }

    if (bytes_read < 0 || offset < file_size)
    {
      atomic_fetch_sub(&entry->ref_count, 1);
      pthread_mutex_unlock(&g_storage.buffer_mutex);
      return NULL;
    }

    // Page lies beyond the end of file -> initialize a new (fresh) page.
    memset(entry->page, 0, PAGE_SIZE);
    entry->page->header.page_id = page_id;
    entry->page->header.page_type = PAGE_TYPE_LEAF;
    entry->page->header.free_space = PAGE_SIZE - sizeof(page_header_t);
    entry->dirty = true;
  }

  atomic_fetch_add(&g_storage.stats.pages_read, 1);

  // Verify checksum if enabled
  if (g_storage.config.enable_checksums)
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
      pthread_mutex_unlock(&g_storage.buffer_mutex);
      return NULL;
    }
  }

  // Publish the fully-loaded entry into the hash table.
  uint32_t hash = hash_page_id(page_id);
  entry->hash_next = g_storage.hash_table[hash];
  g_storage.hash_table[hash] = (uint32_t)(entry - g_storage.buffer_pool);

  pthread_mutex_unlock(&g_storage.buffer_mutex);

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
  pthread_mutex_lock(&g_storage.buffer_mutex);
  buffer_entry_t *entry = find_buffer_entry(page_id);
  pthread_mutex_unlock(&g_storage.buffer_mutex);
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
  pthread_mutex_lock(&g_storage.buffer_mutex);
  buffer_entry_t *entry = find_buffer_entry(page_id);
  pthread_mutex_unlock(&g_storage.buffer_mutex);
  if (entry)
  {
    entry->dirty = true;

    // Update LSN
    if (g_storage.config.enable_wal)
    {
      entry->page->header.lsn = atomic_load(&g_storage.next_lsn) - 1;
    }

    // Update checksum
    if (g_storage.config.enable_checksums)
    {
      entry->page->header.checksum = 0;
      entry->page->header.checksum = calculate_checksum(entry->page, PAGE_SIZE);
    }
  }
}

uint32_t allocate_page(void)
{
  pthread_mutex_lock(&g_storage.free_page_mutex);

  uint32_t page_id;

  if (g_storage.free_page_count > 0)
  {
    // Reuse a free page
    page_id = g_storage.free_pages[--g_storage.free_page_count];
  }
  else
  {
    page_id = g_storage.next_page_id++;
  }

  pthread_mutex_unlock(&g_storage.free_page_mutex);

  return page_id;
}

void deallocate_page(uint32_t page_id)
{
  pthread_mutex_lock(&g_storage.free_page_mutex);

  // Grow free page array if necessary
  if (g_storage.free_page_count >= g_storage.free_page_capacity)
  {
    size_t new_capacity = g_storage.free_page_capacity * 2;
    if (new_capacity == 0)
    {
      new_capacity = 1024;
    }

    uint32_t *new_array = realloc(g_storage.free_pages, new_capacity * sizeof(uint32_t));
    if (new_array)
    {
      g_storage.free_pages = new_array;
      g_storage.free_page_capacity = new_capacity;
    }
  }

  if (g_storage.free_page_count < g_storage.free_page_capacity)
  {
    g_storage.free_pages[g_storage.free_page_count++] = page_id;
  }

  pthread_mutex_unlock(&g_storage.free_page_mutex);
}
