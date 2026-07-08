#define _GNU_SOURCE

// Transaction lifecycle: begin/commit/abort. A transaction is linked into the
// engine's active list for its whole life; commit and abort release its locks,
// write the corresponding WAL record and unlink it. The handle itself is owned
// (and freed) by the caller -- see storage.h.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage_internal.h"

transaction_t *begin_transaction(void)
{
  transaction_t *txn = malloc(sizeof(transaction_t));
  if (!txn)
  {
    return NULL;
  }

  memset(txn, 0, sizeof(*txn));

  pthread_mutex_lock(&g_storage.txn_mutex);

  txn->txn_id = g_storage.next_txn_id++;
  txn->state = TXN_STATE_ACTIVE;
  txn->start_time = time(NULL);
  txn->start_lsn = atomic_load(&g_storage.next_lsn);

  pthread_mutex_init(&txn->lock_mutex, NULL);

  // Add to active transactions list
  txn->next = g_storage.active_transactions;
  g_storage.active_transactions = txn;

  pthread_mutex_unlock(&g_storage.txn_mutex);

  printf("Transaction %lu started\n", txn->txn_id);

  return txn;
}

// Unlink txn from the engine's active list. Must be called before the caller
// free()s the handle, otherwise the global list would hold a dangling pointer
// (any later traversal, e.g. print_storage_statistics, would be a
// use-after-free).
static void unlink_transaction(transaction_t *txn)
{
  pthread_mutex_lock(&g_storage.txn_mutex);
  transaction_t **pp = &g_storage.active_transactions;
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
  pthread_mutex_unlock(&g_storage.txn_mutex);
}

// Free every lock entry held by txn. Caller holds txn->lock_mutex.
static void release_transaction_locks(transaction_t *txn)
{
  lock_entry_t *lock = txn->locks;
  while (lock)
  {
    lock_entry_t *next_lock = lock->next_in_txn;
    free(lock);
    lock = next_lock;
  }
  txn->locks = NULL;
}

int commit_transaction(transaction_t *txn)
{
  if (!txn || txn->state != TXN_STATE_ACTIVE)
  {
    return -1;
  }

  pthread_mutex_lock(&txn->lock_mutex);

  // Write commit record to WAL
  if (g_storage.config.enable_wal)
  {
    write_wal_record(txn->txn_id, WAL_COMMIT, 0, NULL, 0);
    flush_wal_buffer();
  }

  txn->state = TXN_STATE_COMMITED;
  txn->commit_time = time(NULL);
  txn->commit_lsn = atomic_load(&g_storage.next_lsn) - 1;

  release_transaction_locks(txn);

  pthread_mutex_unlock(&txn->lock_mutex);

  unlink_transaction(txn);

  atomic_fetch_add(&g_storage.stats.transactions_committed, 1);

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
    // This would restore the old values

    undo_entry_t *next_undo = undo->next;
    free(undo->key_data);
    free(undo->old_value_data);
    free(undo);
    undo = next_undo;
  }
  txn->undo_log = NULL;

  // Write abort record to WAL
  if (g_storage.config.enable_wal)
  {
    write_wal_record(txn->txn_id, WAL_ABORT, 0, NULL, 0);
  }

  txn->state = TXN_STATE_ABORTED;

  release_transaction_locks(txn);

  pthread_mutex_unlock(&txn->lock_mutex);

  unlink_transaction(txn);

  atomic_fetch_add(&g_storage.stats.transactions_aborted, 1);

  printf("Transaction %lu aborted\n", txn->txn_id);

  return 0;
}
