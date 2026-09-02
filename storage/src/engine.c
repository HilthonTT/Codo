// Engine lifecycle: the singleton definition, file/buffer-pool/WAL setup and
// teardown, checkpointing, and the statistics dump.

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "storage_internal.h"

storage_engine_t g_storage = {0};

int perform_checkpoint(void)
{
  printf("Starting checkpoint...\n");

  atomic_fetch_add(&g_storage.stats.checkpoints_performed, 1);

  flush_wal_buffer();

  pthread_mutex_lock(&g_storage.buffer_mutex);

  for (size_t i = 0; i < BUFFER_POOL_SIZE; i++)
  {
    buffer_entry_t *entry = &g_storage.buffer_pool[i];

    if (entry->dirty && entry->page)
    {
      off_t offset = (off_t)entry->page_id * PAGE_SIZE;
      if (pwrite(g_storage.data_fd, entry->page, PAGE_SIZE, offset) == PAGE_SIZE)
      {
        entry->dirty = false;
        atomic_fetch_add(&g_storage.stats.pages_written, 1);
      }
    }
  }

  pthread_mutex_unlock(&g_storage.buffer_mutex);

  fsync(g_storage.data_fd);

  write_wal_record(0, WAL_CHECKOUT, 0, NULL, 0);
  flush_wal_buffer();

  g_storage.last_checkpoint_lsn = atomic_load(&g_storage.next_lsn) - 1;

  printf("Checkpoint completed (LSN: %lu)\n", g_storage.last_checkpoint_lsn);

  return 0;
}

// Point every hash bucket at "empty" and give each buffer-pool slot its page
// lock. Returns 0, or -1 when the hash table cannot be allocated.
static int init_buffer_pool(void)
{
  g_storage.hash_table_size = BUFFER_POOL_SIZE * 2;
  g_storage.hash_table = malloc(g_storage.hash_table_size * sizeof(uint32_t));
  if (!g_storage.hash_table)
  {
    return -1;
  }

  for (size_t i = 0; i < g_storage.hash_table_size; i++)
  {
    g_storage.hash_table[i] = UINT32_MAX;
  }

  for (size_t i = 0; i < BUFFER_POOL_SIZE; i++)
  {
    buffer_entry_t *entry = &g_storage.buffer_pool[i];
    pthread_rwlock_init(&entry->page_lock, NULL);
    entry->hash_next = UINT32_MAX;
  }

  pthread_mutex_init(&g_storage.buffer_mutex, NULL);
  pthread_mutex_init(&g_storage.free_page_mutex, NULL);
  pthread_mutex_init(&g_storage.txn_mutex, NULL);
  pthread_mutex_init(&g_storage.wal_mutex, NULL);
  pthread_rwlock_init(&g_storage.tree_lock, NULL);
  return 0;
}

// Recover the page allocator from the data file's extent and return the number
// of pages already in the file. The allocator is not persisted anywhere, so
// without this every restart would start handing out page 1 again -- the root
// -- and the first split after a restart would write a new node straight over
// live data. Rounding up covers a torn final page: the id it occupies is
// treated as taken.
//
// Pages freed by deallocate_page() before the restart are not recovered and
// simply leak; reclaiming them needs the free list on disk, which belongs with
// the WAL replay work below.
static uint32_t recover_page_allocator(void)
{
  struct stat data_st;
  uint32_t pages_in_file = 0;
  if (fstat(g_storage.data_fd, &data_st) == 0 && data_st.st_size > 0)
  {
    pages_in_file = (uint32_t)(((uint64_t)data_st.st_size + PAGE_SIZE - 1) / PAGE_SIZE);
  }

  g_storage.next_page_id = pages_in_file > g_storage.root_page_id
                               ? pages_in_file
                               : g_storage.root_page_id + 1;
  return pages_in_file;
}

int init_storage_engine(const char *data_file, const char *wal_file)
{
  memset(&g_storage, 0, sizeof(g_storage));

  g_storage.config.enable_checksums = true;
  g_storage.config.enable_wal = true;
  g_storage.config.checkpoint_interval = 10000;
  g_storage.config.wal_segment_size = 64 * 1024 * 1024;
  g_storage.config.buffer_pool_hit_ratio_target = 0.95;

  g_storage.data_fd = open(data_file, O_RDWR | O_CREAT, 0644);
  if (g_storage.data_fd < 0)
  {
    perror("open data file");
    return -1;
  }
  g_storage.data_filename = strdup(data_file);

  g_storage.wal_fd = open(wal_file, O_RDWR | O_CREAT | O_APPEND, 0644);
  if (g_storage.wal_fd < 0)
  {
    perror("open WAL file");
    goto fail_data_file;
  }
  g_storage.wal_filename = strdup(wal_file);

  g_storage.wal_buffer = malloc(WAL_BUFFER_SIZE);
  if (!g_storage.wal_buffer)
  {
    goto fail_wal_file;
  }

  if (init_buffer_pool() != 0)
  {
    goto fail_wal_buffer;
  }

  g_storage.next_txn_id = 1;
  g_storage.next_lsn = 1;
  g_storage.root_page_id = 1;

  uint32_t pages_in_file = recover_page_allocator();

  // TODO(durability): WAL crash-recovery replay is NOT implemented. On restart
  // we do not scan the WAL to redo committed changes / undo uncommitted ones,
  // so any committed writes that were logged but not yet checkpointed to the
  // data file are lost after a crash. Eviction/checkpoint honour the
  // write-ahead ordering (WAL is flushed before dirty pages are written back),
  // but full recovery still needs a replay pass here.

  if (pages_in_file == 0)
  {
    btree_page_t *root_page = get_page(g_storage.root_page_id, LOCK_EXCLUSIVE);
    if (root_page)
    {
      root_page->header.page_type = PAGE_TYPE_LEAF;
      mark_page_dirty(g_storage.root_page_id);
      release_page(g_storage.root_page_id, LOCK_EXCLUSIVE);
    }
  }

  printf("Storage engine initialized\n");
  printf("Data file: %s (%u page(s), next page id %u)\n", data_file, pages_in_file,
         g_storage.next_page_id);
  printf("WAL file: %s\n", wal_file);

  return 0;

fail_wal_buffer:
  free(g_storage.wal_buffer);
fail_wal_file:
  close(g_storage.wal_fd);
  free(g_storage.wal_filename);
fail_data_file:
  close(g_storage.data_fd);
  free(g_storage.data_filename);
  return -1;
}

void cleanup_storage_engine(void)
{
  perform_checkpoint();

  if (g_storage.data_fd >= 0)
  {
    close(g_storage.data_fd);
  }

  if (g_storage.wal_fd >= 0)
  {
    close(g_storage.wal_fd);
  }

  free(g_storage.data_filename);
  free(g_storage.wal_filename);
  free(g_storage.wal_buffer);
  free(g_storage.hash_table);
  free(g_storage.free_pages);

  for (size_t i = 0; i < BUFFER_POOL_SIZE; i++)
  {
    buffer_entry_t *entry = &g_storage.buffer_pool[i];
    if (entry->page)
    {
      free(entry->page);
    }
    pthread_rwlock_destroy(&entry->page_lock);
  }

  pthread_rwlock_destroy(&g_storage.tree_lock);
}
