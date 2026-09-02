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
#include <zlib.h>

#include "storage_internal.h"

uint32_t hash_page_id(uint32_t page_id)
{
  return page_id % g_storage.hash_table_size;
}

uint32_t calculate_checksum(const void *data, size_t length)
{
  // Real CRC32 (from zlib, which the build already links for gzip). The
  // previous hand-rolled "(checksum << 1) ^ byte" shifted every earlier byte
  // out of a 32-bit accumulator within 32 iterations, so the result of a 4 KiB
  // page depended only on its last ~32 bytes -- corruption anywhere else
  // verified clean. Page images are compared byte-for-byte here, so the extra
  // cost per page load is negligible next to the pread it follows.
  //
  // NOTE: this changes the bytes stored in page_header_t.checksum, so a data
  // file written before this change will fail verification on load. There is
  // no format version to negotiate against, so such a file must be recreated.
  uint32_t crc = (uint32_t)crc32(0L, Z_NULL, 0);
  return (uint32_t)crc32(crc, (const Bytef *)data, (uInt)length);
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

// Take the caller's requested page lock. Safe to call without buffer_mutex
// once the entry is pinned.
static void acquire_page_lock(buffer_entry_t *entry, lock_type_t lock_type)
{
  if (lock_type == LOCK_SHARED)
  {
    pthread_rwlock_rdlock(&entry->page_lock);
  }
  else if (lock_type == LOCK_EXCLUSIVE)
  {
    pthread_rwlock_wrlock(&entry->page_lock);
  }
}

// Read page_id into the entry's frame, or initialize a fresh page when the id
// lies beyond the end of the data file. Returns 0 on success, -1 when the page
// should already exist but could not be read.
static int load_page_from_disk(buffer_entry_t *entry, uint32_t page_id)
{
  // Cast page_id to off_t before the multiply so the offset is computed in
  // 64 bits.
  off_t offset = (off_t)page_id * PAGE_SIZE;
  ssize_t bytes_read = pread(g_storage.data_fd, entry->page, PAGE_SIZE, offset);
  if (bytes_read == PAGE_SIZE)
  {
    return 0;
  }

  // Distinguish a legitimately fresh page (beyond the current end of file) from
  // a read error / short read on a page that should already exist. In the
  // latter case we must NOT zero-and-keep the page: a later eviction would
  // pwrite the empty page over real on-disk data. Fail instead and leave the
  // on-disk data intact.
  off_t file_size = 0;
  struct stat st;
  if (fstat(g_storage.data_fd, &st) == 0)
  {
    file_size = st.st_size;
  }
  if (bytes_read < 0 || offset < file_size)
  {
    return -1;
  }

  memset(entry->page, 0, PAGE_SIZE);
  entry->page->header.page_id = page_id;
  entry->page->header.page_type = PAGE_TYPE_LEAF;
  entry->page->header.free_space = PAGE_SIZE - sizeof(page_header_t);
  entry->dirty = true;
  return 0;
}

// Verify a freshly-read page against its stored CRC32. A stored value of 0
// means the page was never checksummed, which is accepted.
static bool page_checksum_valid(btree_page_t *page, uint32_t page_id)
{
  uint32_t stored = page->header.checksum;
  page->header.checksum = 0;
  uint32_t calculated = calculate_checksum(page, PAGE_SIZE);
  page->header.checksum = stored;

  if (stored == 0 || stored == calculated)
  {
    return true;
  }

  fprintf(stderr,
          "storage: checksum mismatch on page %u (stored %08x, computed "
          "%08x). The page is corrupt, or the data file predates the "
          "switch to CRC32 -- an older file must be recreated.\n",
          page_id, stored, calculated);
  return false;
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

  buffer_entry_t *entry = find_buffer_entry(page_id);
  if (entry)
  {
    atomic_fetch_add(&entry->ref_count, 1);
    clock_gettime(CLOCK_MONOTONIC, &entry->last_access);
    atomic_fetch_add(&g_storage.stats.cache_hits, 1);

    pthread_mutex_unlock(&g_storage.buffer_mutex);

    acquire_page_lock(entry, lock_type);
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
      goto fail_pinned;
    }
  }

  if (load_page_from_disk(entry, page_id) != 0)
  {
    goto fail_pinned;
  }

  atomic_fetch_add(&g_storage.stats.pages_read, 1);

  if (g_storage.config.enable_checksums && !page_checksum_valid(entry->page, page_id))
  {
    goto fail_pinned;
  }

  // Publish the fully-loaded entry into the hash table.
  uint32_t hash = hash_page_id(page_id);
  entry->hash_next = g_storage.hash_table[hash];
  g_storage.hash_table[hash] = (uint32_t)(entry - g_storage.buffer_pool);

  pthread_mutex_unlock(&g_storage.buffer_mutex);

  acquire_page_lock(entry, lock_type);
  return entry->page;

fail_pinned:
  // The entry was never linked into the hash table, so releasing its pin
  // returns the slot to the free pool and the page can never be served from
  // the cache on a later get_page.
  atomic_fetch_sub(&entry->ref_count, 1);
  pthread_mutex_unlock(&g_storage.buffer_mutex);
  return NULL;
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

    if (g_storage.config.enable_wal)
    {
      entry->page->header.lsn = atomic_load(&g_storage.next_lsn) - 1;
    }

    if (g_storage.config.enable_checksums)
    {
      entry->page->header.checksum = 0;
      entry->page->header.checksum = calculate_checksum(entry->page, PAGE_SIZE);
    }
  }
}

// Page allocation. This was removed once as dead code -- correctly, at a time
// when nothing ever needed a second page because the B-tree could not split.
// btree_split_node() calls it for every new node, so it is load-bearing again.
uint32_t allocate_page(void)
{
  pthread_mutex_lock(&g_storage.free_page_mutex);

  uint32_t page_id;

  if (g_storage.free_page_count > 0)
  {
    page_id = g_storage.free_pages[--g_storage.free_page_count];
  }
  else if (g_storage.next_page_id < MAX_PAGES)
  {
    page_id = g_storage.next_page_id++;
  }
  else
  {
    // Out of address space. 0 is the failure sentinel (it is also the leaf
    // chain's terminator, so it is never a real page id).
    page_id = 0;
  }

  pthread_mutex_unlock(&g_storage.free_page_mutex);

  return page_id;
}

void deallocate_page(uint32_t page_id)
{
  pthread_mutex_lock(&g_storage.free_page_mutex);

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
