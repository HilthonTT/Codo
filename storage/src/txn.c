// Transaction lifecycle: begin/commit/abort. A transaction is linked into the
// engine's active list for its whole life; commit and abort write the
// corresponding WAL record and unlink it. The handle itself is owned (and
// freed) by the caller -- see storage.h.
//
// A transaction is a WAL-record boundary and nothing more: there is no lock
// manager and no undo log, so it neither isolates concurrent readers/writers
// nor rolls anything back. Isolation comes from the pager's per-page rwlocks
// plus tree_lock. Both belong with the WAL replay work in engine.c.

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

  txn->next = g_storage.active_transactions;
  g_storage.active_transactions = txn;

  pthread_mutex_unlock(&g_storage.txn_mutex);

  printf("Transaction %lu started\n", txn->txn_id);

  return txn;
}

// Unlink txn from the engine's active list. Must be called before the caller
// free()s the handle, otherwise the global list would hold a dangling pointer
// and any later traversal of it would be a use-after-free.
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

int commit_transaction(transaction_t *txn)
{
  if (!txn || txn->state != TXN_STATE_ACTIVE)
  {
    return -1;
  }

  if (g_storage.config.enable_wal)
  {
    write_wal_record(txn->txn_id, WAL_COMMIT, 0, NULL, 0);
    flush_wal_buffer();
  }

  txn->state = TXN_STATE_COMMITED;
  txn->commit_time = time(NULL);
  txn->commit_lsn = atomic_load(&g_storage.next_lsn) - 1;

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

  // Write abort record to WAL. Note that aborting does NOT roll back changes
  // already applied to pages -- there is no undo log to replay. Callers use
  // abort only to close out a transaction that never wrote.
  if (g_storage.config.enable_wal)
  {
    write_wal_record(txn->txn_id, WAL_ABORT, 0, NULL, 0);
  }

  txn->state = TXN_STATE_ABORTED;

  unlink_transaction(txn);

  atomic_fetch_add(&g_storage.stats.transactions_aborted, 1);

  printf("Transaction %lu aborted\n", txn->txn_id);

  return 0;
}
