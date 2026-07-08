#ifndef STORAGE_H
#define STORAGE_H

// Public API of the embedded B-tree storage engine (libstorage.a).
//
// The engine is a process-wide singleton: init_storage_engine() opens the data
// and WAL files, cleanup_storage_engine() checkpoints and closes them. All
// reads and writes go through a transaction handle obtained from
// begin_transaction(). The handle is opaque here; callers own it and must
// free() it after commit_transaction()/abort_transaction().
//
// Internals (pages, buffer pool, WAL records, locking) live in
// storage/src/storage_internal.h and are not part of this contract.

#include <stddef.h>

// Hard limits for a single key / value, enforced by db_insert/db_update.
#define MAX_KEY_SIZE 256
#define MAX_VALUE_SIZE 1024

// Opaque transaction handle (definition in storage_internal.h).
typedef struct transaction transaction_t;

// Engine lifecycle. init returns 0 on success, -1 on failure. cleanup runs a
// final checkpoint so committed data survives the restart.
int init_storage_engine(const char *data_file, const char *wal_file);
void cleanup_storage_engine(void);

// Transaction lifecycle. commit/abort release the transaction's locks and
// unlink it from the engine; the caller frees the handle afterwards.
transaction_t *begin_transaction(void);
int commit_transaction(transaction_t *txn);
int abort_transaction(transaction_t *txn);

// Key-value operations. All return 0 on success, -1 on failure (key missing,
// duplicate insert, size limits exceeded, page full, ...).
int db_insert(transaction_t *txn, const char *key, size_t key_length,
              const char *value, size_t value_length);
// On success copies up to *value_length bytes into value and sets
// *value_length to the stored value's full length.
int db_search(transaction_t *txn, const char *key, size_t key_length,
              char *value, size_t *value_length);
// In-place update; only supports a new value of identical length (no
// page-split / relocation). Callers needing a resize do delete + insert.
int db_update(transaction_t *txn, const char *key, size_t key_length,
              const char *new_value, size_t new_value_length);
int db_delete(transaction_t *txn, const char *key, size_t key_length);

// Iterate every key-value pair in key order. The callback returns non-zero to
// stop iteration early. Returns 0 on success, -1 on error.
typedef int (*db_scan_callback_t)(const char *key, size_t key_length,
                                  const char *value, size_t value_length,
                                  void *ctx);
int db_scan(transaction_t *txn, db_scan_callback_t callback, void *ctx);

// Flush the WAL and all dirty pages to disk and record a checkpoint.
int perform_checkpoint(void);

// Dump buffer-pool / transaction / WAL counters to stdout.
void print_storage_statistics(void);

#endif
