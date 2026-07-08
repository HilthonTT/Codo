#define _GNU_SOURCE

// High-level key-value API: descend the tree from the root to the target
// leaf, then delegate to the page-level operations in btree.c. Mutations log
// a WAL record (via wal.c) before touching the page, and every touched page
// is pinned/locked through the pager.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage_internal.h"

// Descend from the root to the leaf that owns `key`, taking `lock_type` on
// every page visited. Returns the pinned+locked leaf and stores its id in
// *page_id_out, or NULL on failure (all pins released).
static btree_page_t *descend_to_leaf(const char *key, size_t key_length,
                                     lock_type_t lock_type, uint32_t *page_id_out)
{
  uint32_t page_id = g_storage.root_page_id;
  btree_page_t *page = get_page(page_id, lock_type);
  if (!page)
  {
    return NULL;
  }

  // TODO(concurrency): descending internal nodes with LOCK_EXCLUSIVE
  // serializes all writers; switching internal-node descent to LOCK_SHARED
  // (crab-latching) would improve concurrency.
  while (page->header.page_type == PAGE_TYPE_INTERNAL)
  {
    int pos = find_key_position(page, key, key_length);
    kv_pair_t *kv = get_kv_pair(page, pos);

    uint32_t child_page_id = kv ? kv->child_page_id : page->header.next_page_id;

    release_page(page_id, lock_type);
    page_id = child_page_id;
    page = get_page(page_id, lock_type);

    if (!page)
    {
      return NULL;
    }
  }

  *page_id_out = page_id;
  return page;
}

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

  uint32_t page_id;
  btree_page_t *page = descend_to_leaf(key, key_length, LOCK_EXCLUSIVE, &page_id);
  if (!page)
  {
    return -1;
  }

  // Check if the key already exists
  int pos = find_key_position(page, key, key_length);
  kv_pair_t *existing = get_kv_pair(page, pos);

  if (existing && compare_keys(key, key_length, existing->data, existing->key_length) == 0)
  {
    release_page(page_id, LOCK_EXCLUSIVE);
    return -1; // Key already exists
  }

  // Write WAL record. The payload prefixes two size_t length fields, so the
  // buffer must reserve sizeof(size_t)*2 for them (not 8).
  if (g_storage.config.enable_wal)
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

  uint32_t page_id;
  btree_page_t *page = descend_to_leaf(key, key_length, LOCK_SHARED, &page_id);
  if (!page)
  {
    return -1;
  }

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

  uint32_t page_id;
  btree_page_t *page = descend_to_leaf(key, key_length, LOCK_EXCLUSIVE, &page_id);
  if (!page)
  {
    return -1;
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
  if (g_storage.config.enable_wal)
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

  uint32_t page_id;
  btree_page_t *page = descend_to_leaf(key, key_length, LOCK_EXCLUSIVE, &page_id);
  if (!page)
  {
    return -1;
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
  if (g_storage.config.enable_wal)
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

  uint32_t page_id = g_storage.root_page_id;
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
