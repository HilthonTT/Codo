// High-level key-value API: descend the tree from the root to the target
// leaf, then delegate to the page-level operations in btree.c. Mutations log
// a WAL record (via wal.c) before touching the page, and every touched page
// is pinned/locked through the pager.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage_internal.h"

// A root-to-node chain of page ids, recorded during a descent so a split can
// walk back up to the parent that must hold the new separator.
typedef struct
{
  uint32_t ids[BTREE_MAX_DEPTH];
  int len;
} btree_path_t;

// Descend from the root toward `key`, recording every page id visited. Pages
// are pinned only long enough to read the child pointer and are never locked:
// this runs exclusively under tree_lock, so no other thread can be reshaping
// the tree or reading it while we do. Returns 0 with path->ids[path->len-1]
// holding the target leaf.
static int descend_path(const char *key, size_t key_length, btree_path_t *path)
{
  path->len = 0;
  uint32_t page_id = g_storage.root_page_id;

  for (int level = 0; level < BTREE_MAX_DEPTH; level++)
  {
    btree_page_t *page = get_page(page_id, LOCK_NONE);
    if (!page)
    {
      return -1;
    }
    path->ids[path->len++] = page_id;

    if (page->header.page_type != PAGE_TYPE_INTERNAL)
    {
      release_page(page_id, LOCK_NONE);
      return 0; // reached the leaf
    }

    int pos = find_key_position(page, key, key_length);
    kv_pair_t *kv = get_kv_pair(page, pos);
    uint32_t child_page_id = kv ? kv->child_page_id : page->header.next_page_id;
    release_page(page_id, LOCK_NONE);

    // A child pointer of 0, or one that loops back on itself, means the page
    // is corrupt; refuse rather than spin.
    if (child_page_id == 0 || child_page_id == page_id)
    {
      return -1;
    }
    page_id = child_page_id;
  }

  return -1; // deeper than any real tree -> treat as corrupt
}

// Give the root a child so it can be split like any other node: its contents
// move into a freshly allocated page and the root becomes an empty internal
// node whose rightmost pointer is that page. The root's id never changes,
// which is what lets the engine hard-code root_page_id.
static int grow_root(uint32_t *moved_id)
{
  uint32_t left_id = allocate_page();
  if (left_id == 0)
  {
    return -1;
  }

  btree_page_t *root = get_page(g_storage.root_page_id, LOCK_EXCLUSIVE);
  if (!root)
  {
    return -1;
  }
  btree_page_t *left = get_page(left_id, LOCK_EXCLUSIVE);
  if (!left)
  {
    release_page(g_storage.root_page_id, LOCK_EXCLUSIVE);
    return -1;
  }

  // The copy carries the header too, so if the root was already internal the
  // new child inherits its rightmost pointer and separator set intact.
  memcpy(left, root, PAGE_SIZE);
  left->header.page_id = left_id;

  const size_t capacity = btree_page_capacity();
  memset(root->data, 0, capacity);
  root->header.page_type = PAGE_TYPE_INTERNAL;
  root->header.key_count = 0;
  root->header.free_space = (uint16_t)capacity;
  root->header.next_page_id = left_id;
  root->header.prev_page_id = 0;

  mark_page_dirty(left_id);
  mark_page_dirty(g_storage.root_page_id);
  release_page(left_id, LOCK_EXCLUSIVE);
  release_page(g_storage.root_page_id, LOCK_EXCLUSIVE);

  *moved_id = left_id;
  return 0;
}

// Publish a freshly split pair of nodes in their parent: `sep` (the highest
// key still in `node_id`) starts routing to node_id, and whichever pointer
// used to reach node_id is repointed at its new right half.
static int insert_separator(uint32_t node_id, const char *sep, size_t sep_len,
                            uint32_t right_id, int depth)
{
  if (depth <= 0)
  {
    return -1;
  }

  // Re-descend rather than trusting a recorded path: a cascading split may
  // have moved node_id under a different parent since. `sep` is node_id's
  // high key, so it routes to exactly this node.
  btree_path_t path;
  if (descend_path(sep, sep_len, &path) != 0)
  {
    return -1;
  }
  int level = -1;
  for (int i = 0; i < path.len; i++)
  {
    if (path.ids[i] == node_id)
    {
      level = i;
      break;
    }
  }
  if (level <= 0)
  {
    return -1; // not found, or still the root -- caller should have grown it
  }
  uint32_t parent_id = path.ids[level - 1];

  btree_page_t *parent = get_page(parent_id, LOCK_EXCLUSIVE);
  if (!parent)
  {
    return -1;
  }

  // Find the pointer that currently reaches node_id: either a separator entry
  // or the rightmost child.
  int old_pos = -1;
  for (int i = 0; i < parent->header.key_count; i++)
  {
    kv_pair_t *kv = get_kv_pair(parent, i);
    if (!kv)
    {
      release_page(parent_id, LOCK_EXCLUSIVE);
      return -1;
    }
    if (kv->child_page_id == node_id)
    {
      old_pos = i;
      break;
    }
  }
  if (old_pos < 0 && parent->header.next_page_id != node_id)
  {
    release_page(parent_id, LOCK_EXCLUSIVE);
    return -1; // parent does not actually point at node_id
  }

  // sep is lower than the separator that used to cover node_id, so it belongs
  // immediately before it -- or at the end when node_id was the rightmost.
  int insert_pos = (old_pos >= 0) ? old_pos : parent->header.key_count;

  if (insert_kv_pair(parent, insert_pos, sep, sep_len, NULL, 0, node_id) != 0)
  {
    // Parent is full. Split it and start over: that split may have moved
    // node_id under the parent's new right sibling, so the retry re-descends.
    release_page(parent_id, LOCK_EXCLUSIVE);
    if (btree_split_node(parent_id, depth - 1) != 0)
    {
      return -1;
    }
    return insert_separator(node_id, sep, sep_len, right_id, depth - 1);
  }

  if (old_pos >= 0)
  {
    // The old entry shifted right by one; it now covers the upper half.
    kv_pair_t *moved = get_kv_pair(parent, old_pos + 1);
    if (!moved)
    {
      release_page(parent_id, LOCK_EXCLUSIVE);
      return -1;
    }
    moved->child_page_id = right_id;
  }
  else
  {
    parent->header.next_page_id = right_id;
  }

  mark_page_dirty(parent_id);
  release_page(parent_id, LOCK_EXCLUSIVE);
  return 0;
}

int btree_split_node(uint32_t node_id, int depth)
{
  if (depth <= 0)
  {
    return -1;
  }

  if (node_id == g_storage.root_page_id)
  {
    uint32_t moved = 0;
    if (grow_root(&moved) != 0)
    {
      return -1;
    }
    node_id = moved; // now an ordinary child of the root
  }

  uint32_t right_id = allocate_page();
  if (right_id == 0)
  {
    return -1;
  }

  btree_page_t *node = get_page(node_id, LOCK_EXCLUSIVE);
  if (!node)
  {
    return -1;
  }
  btree_page_t *right = get_page(right_id, LOCK_EXCLUSIVE);
  if (!right)
  {
    release_page(node_id, LOCK_EXCLUSIVE);
    return -1;
  }

  // allocate_page may hand back a recycled id whose buffer still holds the old
  // page, so start from a known-empty page rather than trusting get_page's
  // beyond-EOF initialization.
  memset(right, 0, PAGE_SIZE);
  right->header.free_space = (uint16_t)btree_page_capacity();

  char sep[MAX_KEY_SIZE];
  size_t sep_len = 0;
  int rc = btree_split_page(node, right, right_id, sep, sizeof(sep), &sep_len);
  if (rc == 0)
  {
    mark_page_dirty(node_id);
    mark_page_dirty(right_id);
  }
  release_page(right_id, LOCK_EXCLUSIVE);
  release_page(node_id, LOCK_EXCLUSIVE);

  if (rc != 0)
  {
    deallocate_page(right_id);
    return -1;
  }

  return insert_separator(node_id, sep, sep_len, right_id, depth - 1);
}

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

  // TODO(concurrency): writers are serialized by tree_lock (see
  // storage_internal.h) and this descent additionally takes LOCK_EXCLUSIVE on
  // every internal node it passes through. Latch-coupling the descent -- take
  // the child's latch before dropping the parent's -- would let both go.
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
  if (key_length == 0 || key_length > MAX_KEY_SIZE || value_length > MAX_VALUE_SIZE)
  {
    return -1;
  }

  // Structural writer: an insert may split pages and rewire parents, so it
  // excludes every other tree operation for its duration.
  pthread_rwlock_wrlock(&g_storage.tree_lock);

  int rc = -1;

  // Try to place the pair; when the target leaf is full, split it and descend
  // again -- the key now belongs to one of the two halves. Each pass either
  // succeeds, fails outright, or makes the tree one level roomier, so the
  // bound is a formality.
  for (int attempt = 0; attempt < BTREE_MAX_DEPTH; attempt++)
  {
    btree_path_t path;
    if (descend_path(key, key_length, &path) != 0 || path.len == 0)
    {
      break;
    }
    uint32_t page_id = path.ids[path.len - 1];

    btree_page_t *page = get_page(page_id, LOCK_EXCLUSIVE);
    if (!page)
    {
      break;
    }

    // Check if the key already exists
    int pos = find_key_position(page, key, key_length);
    kv_pair_t *existing = get_kv_pair(page, pos);

    if (existing && compare_keys(key, key_length, existing->data, existing->key_length) == 0)
    {
      release_page(page_id, LOCK_EXCLUSIVE);
      break; // Key already exists
    }

    size_t pair_size = sizeof(kv_pair_t) + key_length + value_length;
    if (page->header.free_space < pair_size)
    {
      // Leaf is full. Split it (growing a new root if this leaf *is* the
      // root) and retry the descent.
      release_page(page_id, LOCK_EXCLUSIVE);
      if (btree_split_node(page_id, BTREE_MAX_DEPTH) != 0)
      {
        break;
      }
      continue;
    }

    // There is room, so log before touching the page: write-ahead ordering
    // means the record must be durable before the change it describes.
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

    if (insert_kv_pair(page, pos, key, key_length, value, value_length, 0) == 0)
    {
      mark_page_dirty(page_id);
      txn->stats.rows_inserted++;
      rc = 0;
    }

    release_page(page_id, LOCK_EXCLUSIVE);
    break;
  }

  pthread_rwlock_unlock(&g_storage.tree_lock);
  return rc;
}

static int db_search_locked(transaction_t *txn, const char *key, size_t key_length, char *value, size_t *value_length)
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

static int db_update_locked(transaction_t *txn, const char *key, size_t key_length, const char *new_value, size_t new_value_length)
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

static int db_delete_locked(transaction_t *txn, const char *key, size_t key_length)
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

// Shared implementation of db_scan / db_scan_prefix: visit every pair whose key
// starts with `prefix`, in key order. An empty prefix visits the whole tree.
//
// Both ends of the range fall out of the key ordering. compare_keys() breaks a
// tie on the shared bytes in favour of the shorter key, so `prefix` itself sorts
// before every key that extends it: find_key_position() lands exactly on the
// first match, and the first key that fails the prefix test ends the range.
//
// Caller holds tree_lock (see the public wrappers at the bottom of this file).
static int scan_range(transaction_t *txn, const char *prefix, size_t prefix_length,
                      db_scan_callback_t callback, void *ctx)
{
  if (!txn || txn->state != TXN_STATE_ACTIVE || !callback)
  {
    return -1;
  }

  if (prefix_length > MAX_KEY_SIZE)
  {
    return -1;
  }

  // With an empty prefix this follows the first child pointer at every level,
  // i.e. descends to the leftmost leaf.
  uint32_t page_id;
  btree_page_t *page = descend_to_leaf(prefix, prefix_length, LOCK_SHARED, &page_id);
  if (!page)
  {
    return -1;
  }

  int start = find_key_position(page, prefix, prefix_length);

  // Walk the linked list of leaf pages from that position on.
  while (page)
  {
    for (int i = start; i < (int)page->header.key_count; i++)
    {
      kv_pair_t *kv = get_kv_pair(page, i);
      if (!kv)
      {
        break;
      }

      const char *key = kv->data;
      const char *value = kv->data + kv->key_length;

      if (prefix_length > 0 &&
          (kv->key_length < prefix_length ||
           memcmp(key, prefix, prefix_length) != 0))
      {
        release_page(page_id, LOCK_SHARED);
        return 0; // Past the end of the prefix range
      }

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
    start = 0; // Only the first leaf starts mid-page
  }

  return 0;
}

// ---- Public entry points -------------------------------------------------
//
// Everything below takes tree_lock shared. These operations never change the
// tree's shape -- they land on a single leaf (or walk the leaf chain) and rely
// on the pager's per-page rwlocks for content safety -- so any number of them
// may run at once, but none may run while an insert is splitting pages
// underneath them.

int db_search(transaction_t *txn, const char *key, size_t key_length,
              char *value, size_t *value_length)
{
  pthread_rwlock_rdlock(&g_storage.tree_lock);
  int rc = db_search_locked(txn, key, key_length, value, value_length);
  pthread_rwlock_unlock(&g_storage.tree_lock);
  return rc;
}

int db_update(transaction_t *txn, const char *key, size_t key_length,
              const char *new_value, size_t new_value_length)
{
  pthread_rwlock_rdlock(&g_storage.tree_lock);
  int rc = db_update_locked(txn, key, key_length, new_value, new_value_length);
  pthread_rwlock_unlock(&g_storage.tree_lock);
  return rc;
}

int db_delete(transaction_t *txn, const char *key, size_t key_length)
{
  pthread_rwlock_rdlock(&g_storage.tree_lock);
  int rc = db_delete_locked(txn, key, key_length);
  pthread_rwlock_unlock(&g_storage.tree_lock);
  return rc;
}

int db_scan(transaction_t *txn, db_scan_callback_t callback, void *ctx)
{
  pthread_rwlock_rdlock(&g_storage.tree_lock);
  int rc = scan_range(txn, "", 0, callback, ctx);
  pthread_rwlock_unlock(&g_storage.tree_lock);
  return rc;
}

int db_scan_prefix(transaction_t *txn, const char *prefix, size_t prefix_length,
                   db_scan_callback_t callback, void *ctx)
{
  if (prefix_length > 0 && !prefix)
  {
    return -1;
  }

  pthread_rwlock_rdlock(&g_storage.tree_lock);
  int rc = scan_range(txn, prefix ? prefix : "", prefix_length, callback, ctx);
  pthread_rwlock_unlock(&g_storage.tree_lock);
  return rc;
}
