#define _GNU_SOURCE

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

  // Flush WAL buffer
  flush_wal_buffer();

  // Write all dirty pages to disk
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

  // Sync data file
  fsync(g_storage.data_fd);

  // Write checkpoint record
  write_wal_record(0, WAL_CHECKOUT, 0, NULL, 0);
  flush_wal_buffer();

  g_storage.last_checkpoint_lsn = atomic_load(&g_storage.next_lsn) - 1;

  printf("Checkpoint completed (LSN: %lu)\n", g_storage.last_checkpoint_lsn);

  return 0;
}

void print_storage_statistics(void)
{
  printf("\n=== Storage Engine Statistics ===\n");

  printf("Buffer Pool:\n");
  printf("  Pages read: %lu\n", atomic_load(&g_storage.stats.pages_read));
  printf("  Pages written: %lu\n", atomic_load(&g_storage.stats.pages_written));
  printf("  Cache hits: %lu\n", atomic_load(&g_storage.stats.cache_hits));
  printf("  Cache misses: %lu\n", atomic_load(&g_storage.stats.cache_misses));

  uint64_t total_accesses = atomic_load(&g_storage.stats.cache_hits) +
                            atomic_load(&g_storage.stats.cache_misses);
  if (total_accesses > 0)
  {
    double hit_ratio = (double)atomic_load(&g_storage.stats.cache_hits) / total_accesses;
    printf("  Cache hit ratio: %.2f%%\n", hit_ratio * 100.0);
  }

  printf("\nTransactions:\n");
  printf("  Committed: %lu\n", atomic_load(&g_storage.stats.transactions_committed));
  printf("  Aborted: %lu\n", atomic_load(&g_storage.stats.transactions_aborted));

  printf("\nWAL:\n");
  printf("  Records written: %lu\n", atomic_load(&g_storage.stats.wal_records_written));
  printf("  Checkpoints: %lu\n", atomic_load(&g_storage.stats.checkpoints_performed));
  printf("  Next LSN: %lu\n", atomic_load(&g_storage.next_lsn));
  printf("  Last checkpoint LSN: %lu\n", g_storage.last_checkpoint_lsn);

  printf("\nActive Transactions:\n");
  pthread_mutex_lock(&g_storage.txn_mutex);
  transaction_t *txn = g_storage.active_transactions;
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
  pthread_mutex_unlock(&g_storage.txn_mutex);

  printf("=================================\n");
}

int init_storage_engine(const char *data_file, const char *wal_file)
{
  memset(&g_storage, 0, sizeof(g_storage));

  // Configuration
  g_storage.config.enable_checksums = true;
  g_storage.config.enable_wal = true;
  g_storage.config.checkpoint_interval = 10000;
  g_storage.config.wal_segment_size = 64 * 1024 * 1024;
  g_storage.config.buffer_pool_hit_ratio_target = 0.95;

  // Open data file
  g_storage.data_fd = open(data_file, O_RDWR | O_CREAT, 0644);
  if (g_storage.data_fd < 0)
  {
    perror("open data file");
    return -1;
  }

  g_storage.data_filename = strdup(data_file);

  // Open WAL file
  g_storage.wal_fd = open(wal_file, O_RDWR | O_CREAT | O_APPEND, 0644);
  if (g_storage.wal_fd < 0)
  {
    perror("open WAL file");
    close(g_storage.data_fd);
    free(g_storage.data_filename);
    return -1;
  }

  g_storage.wal_filename = strdup(wal_file);

  // Initialize WAL buffer
  g_storage.wal_buffer = malloc(WAL_BUFFER_SIZE);
  if (!g_storage.wal_buffer)
  {
    close(g_storage.data_fd);
    close(g_storage.wal_fd);
    free(g_storage.data_filename);
    free(g_storage.wal_filename);
    return -1;
  }

  // Initialize hash table for buffer pool
  g_storage.hash_table_size = BUFFER_POOL_SIZE * 2;
  g_storage.hash_table = malloc(g_storage.hash_table_size * sizeof(uint32_t));
  if (!g_storage.hash_table)
  {
    close(g_storage.data_fd);
    close(g_storage.wal_fd);
    free(g_storage.data_filename);
    free(g_storage.wal_filename);
    free(g_storage.wal_buffer);
    return -1;
  }

  for (size_t i = 0; i < g_storage.hash_table_size; i++)
  {
    g_storage.hash_table[i] = UINT32_MAX;
  }

  // Initialize buffer pool
  for (size_t i = 0; i < BUFFER_POOL_SIZE; i++)
  {
    buffer_entry_t *entry = &g_storage.buffer_pool[i];
    pthread_rwlock_init(&entry->page_lock, NULL);
    entry->hash_next = UINT32_MAX;
  }

  // Initialize lock table
  g_storage.lock_table_size = 10007; // Prime number
  g_storage.lock_table = calloc(g_storage.lock_table_size, sizeof(lock_entry_t *));
  if (!g_storage.lock_table)
  {
    close(g_storage.data_fd);
    close(g_storage.wal_fd);
    free(g_storage.data_filename);
    free(g_storage.wal_filename);
    free(g_storage.wal_buffer);
    free(g_storage.hash_table);
    for (size_t i = 0; i < BUFFER_POOL_SIZE; i++)
    {
      pthread_rwlock_destroy(&g_storage.buffer_pool[i].page_lock);
    }
    return -1;
  }

  // Initialize mutexes
  pthread_mutex_init(&g_storage.buffer_mutex, NULL);
  pthread_mutex_init(&g_storage.free_page_mutex, NULL);
  pthread_mutex_init(&g_storage.txn_mutex, NULL);
  pthread_mutex_init(&g_storage.lock_table_mutex, NULL);
  pthread_mutex_init(&g_storage.wal_mutex, NULL);

  // Initialize counters
  g_storage.next_txn_id = 1;
  g_storage.next_lsn = 1;
  g_storage.next_page_id = 1;
  g_storage.root_page_id = 1;

  // TODO(durability): WAL crash-recovery replay is NOT implemented. On restart
  // we do not scan the WAL to redo committed changes / undo uncommitted ones,
  // so any committed writes that were logged but not yet checkpointed to the
  // data file are lost after a crash. Eviction/checkpoint honour the
  // write-ahead ordering (WAL is flushed before dirty pages are written back),
  // but full recovery still needs a replay pass here.

  // Initialize root page if file is empty
  struct stat st;
  if (fstat(g_storage.data_fd, &st) == 0 && st.st_size == 0)
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
  printf("Data file: %s\n", data_file);
  printf("WAL file: %s\n", wal_file);

  return 0;
}

void cleanup_storage_engine(void)
{
  // Perform final checkpoint
  perform_checkpoint();

  // Close files
  if (g_storage.data_fd >= 0)
  {
    close(g_storage.data_fd);
  }

  if (g_storage.wal_fd >= 0)
  {
    close(g_storage.wal_fd);
  }

  // Free memory
  free(g_storage.data_filename);
  free(g_storage.wal_filename);
  free(g_storage.wal_buffer);
  free(g_storage.hash_table);
  free(g_storage.free_pages);
  free(g_storage.lock_table);

  // Cleanup buffer pool
  for (size_t i = 0; i < BUFFER_POOL_SIZE; i++)
  {
    buffer_entry_t *entry = &g_storage.buffer_pool[i];
    if (entry->page)
    {
      free(entry->page);
    }
    pthread_rwlock_destroy(&entry->page_lock);
  }
}
