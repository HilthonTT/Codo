#define _GNU_SOURCE

// Write-ahead log: record append with 8-byte-aligned framing, buffered in
// memory and flushed (write + fsync) either when the buffer fills or when a
// caller needs durability (commit, checkpoint, dirty-page eviction).

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "storage_internal.h"

uint64_t allocate_lsn(void)
{
  return atomic_fetch_add(&g_storage.next_lsn, 1);
}

int write_wal_record(uint64_t txn_id, wal_record_type_t type, uint32_t page_id, const void *data, size_t data_length)
{
  pthread_mutex_lock(&g_storage.wal_mutex);

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
    pthread_mutex_unlock(&g_storage.wal_mutex);
    return -1;
  }

  // Check if we need to flush the buffer
  if (g_storage.wal_buffer_pos + record_size > WAL_BUFFER_SIZE)
  {
    // Write buffer to disk with EINTR/partial-write retry. On failure we must
    // leave wal_buffer_pos unchanged so the next flush does not re-append the
    // already-written prefix (which would duplicate records).
    size_t total_written = 0;
    while (total_written < g_storage.wal_buffer_pos)
    {
      ssize_t bytes_written = write(g_storage.wal_fd,
                                    g_storage.wal_buffer + total_written,
                                    g_storage.wal_buffer_pos - total_written);
      if (bytes_written < 0)
      {
        if (errno == EINTR)
          continue;
        pthread_mutex_unlock(&g_storage.wal_mutex);
        return -1;
      }
      if (bytes_written == 0)
      {
        pthread_mutex_unlock(&g_storage.wal_mutex);
        return -1;
      }
      total_written += (size_t)bytes_written;
    }

    g_storage.wal_buffer_pos = 0;
    fsync(g_storage.wal_fd);
  }

  // Create WAL record in the WAL buffer (not the page buffer pool).
  wal_record_t *record = (wal_record_t *)(g_storage.wal_buffer + g_storage.wal_buffer_pos);
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

  g_storage.wal_buffer_pos += record_size;
  atomic_fetch_add(&g_storage.stats.wal_records_written, 1);

  pthread_mutex_unlock(&g_storage.wal_mutex);

  return 0;
}

int flush_wal_buffer(void)
{
  pthread_mutex_lock(&g_storage.wal_mutex);

  if (g_storage.wal_buffer_pos > 0)
  {
    size_t total_written = 0;
    while (total_written < g_storage.wal_buffer_pos)
    {
      ssize_t bytes_written = write(g_storage.wal_fd,
                                    g_storage.wal_buffer + total_written,
                                    g_storage.wal_buffer_pos - total_written);
      if (bytes_written < 0)
      {
        if (errno == EINTR)
          continue;
        pthread_mutex_unlock(&g_storage.wal_mutex);
        return -1;
      }
      if (bytes_written == 0)
      {
        pthread_mutex_unlock(&g_storage.wal_mutex);
        return -1;
      }
      total_written += (size_t)bytes_written;
    }

    fsync(g_storage.wal_fd);
    g_storage.wal_buffer_pos = 0;
  }

  pthread_mutex_unlock(&g_storage.wal_mutex);
  return 0;
}
