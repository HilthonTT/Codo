#define _GNU_SOURCE

// Key layout for the todo namespace plus maintenance of the owner index. See
// api/include/todo_index.h for the two key shapes and why the index is treated
// as repairable state rather than as the truth.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json_util.h"
#include "todo_index.h"

#define INDEX_PREFIX "idx:user:"
#define INDEX_PREFIX_LEN (sizeof(INDEX_PREFIX) - 1)

// Digits in UINT64_MAX. Ids are padded to this width so lexicographic order
// (all the btree knows) is numeric order.
#define INDEX_ID_WIDTH 20

// "idx:user:<uid>:" -- the prefix a scan of one user's entries matches.
#define INDEX_USER_PREFIX_LEN (INDEX_PREFIX_LEN + INDEX_ID_WIDTH + 1)
#define INDEX_KEY_LEN (INDEX_USER_PREFIX_LEN + INDEX_ID_WIDTH)

_Static_assert(INDEX_KEY_LEN < TODO_KEY_MAX,
               "index key must fit in a TODO_KEY_MAX buffer with its NUL");
_Static_assert(INDEX_KEY_LEN <= MAX_KEY_SIZE,
               "index key must fit the btree key limit");

// Cleared when reconcile cannot make the index match the rows; see
// todo_index_available().
static bool g_index_ready = false;

int todo_row_key(char *out, size_t out_size, uint64_t id)
{
  int n = snprintf(out, out_size, "%llu", (unsigned long long)id);
  if (n < 0 || (size_t)n >= out_size)
  {
    return -1;
  }
  return n;
}

bool todo_row_key_parse(const char *key, size_t key_length, uint64_t *id_out)
{
  if (!key || key_length == 0 || key_length > INDEX_ID_WIDTH)
  {
    return false;
  }

  uint64_t id = 0;
  for (size_t i = 0; i < key_length; i++)
  {
    if (key[i] < '0' || key[i] > '9')
    {
      return false;
    }

    // A 20-digit run can still exceed UINT64_MAX, so check before multiplying.
    uint64_t digit = (uint64_t)(key[i] - '0');
    if (id > (UINT64_MAX - digit) / 10)
    {
      return false;
    }
    id = id * 10 + digit;
  }

  if (id_out)
  {
    *id_out = id;
  }
  return true;
}

int todo_index_key(char *out, size_t out_size, uint64_t user_id, uint64_t todo_id)
{
  int n = snprintf(out, out_size, INDEX_PREFIX "%0*llu:%0*llu",
                   INDEX_ID_WIDTH, (unsigned long long)user_id,
                   INDEX_ID_WIDTH, (unsigned long long)todo_id);
  if (n < 0 || (size_t)n >= out_size)
  {
    return -1;
  }
  return n;
}

int todo_index_prefix(char *out, size_t out_size, uint64_t user_id)
{
  int n = snprintf(out, out_size, INDEX_PREFIX "%0*llu:",
                   INDEX_ID_WIDTH, (unsigned long long)user_id);
  if (n < 0 || (size_t)n >= out_size)
  {
    return -1;
  }
  return n;
}

bool todo_index_key_parse(const char *key, size_t key_length,
                          uint64_t *user_id_out, uint64_t *todo_id_out)
{
  if (!key || key_length != INDEX_KEY_LEN ||
      memcmp(key, INDEX_PREFIX, INDEX_PREFIX_LEN) != 0 ||
      key[INDEX_USER_PREFIX_LEN - 1] != ':')
  {
    return false;
  }

  // Both halves are exactly INDEX_ID_WIDTH digits, which todo_row_key_parse
  // validates (and rejects if padding was tampered with into non-digits).
  return todo_row_key_parse(key + INDEX_PREFIX_LEN, INDEX_ID_WIDTH, user_id_out) &&
         todo_row_key_parse(key + INDEX_USER_PREFIX_LEN, INDEX_ID_WIDTH, todo_id_out);
}

int todo_index_insert(transaction_t *txn, uint64_t user_id, uint64_t todo_id)
{
  char key[TODO_KEY_MAX];
  int key_len = todo_index_key(key, sizeof(key), user_id, todo_id);
  if (key_len < 0)
  {
    return -1;
  }

  // Empty value: the key carries the whole entry.
  return db_insert(txn, key, (size_t)key_len, "", 0);
}

int todo_index_delete(transaction_t *txn, uint64_t user_id, uint64_t todo_id)
{
  char key[TODO_KEY_MAX];
  int key_len = todo_index_key(key, sizeof(key), user_id, todo_id);
  if (key_len < 0)
  {
    return -1;
  }

  return db_delete(txn, key, (size_t)key_len);
}

// ---- Lookup -----------------------------------------------------------------

static int id_list_push(todo_id_list_t *list, uint64_t id)
{
  if (list->count == list->capacity)
  {
    size_t new_capacity = list->capacity ? list->capacity * 2 : 32;
    uint64_t *grown = realloc(list->ids, new_capacity * sizeof(*grown));
    if (!grown)
    {
      return -1;
    }
    list->ids = grown;
    list->capacity = new_capacity;
  }

  list->ids[list->count++] = id;
  return 0;
}

void todo_id_list_free(todo_id_list_t *list)
{
  if (!list)
  {
    return;
  }
  free(list->ids);
  list->ids = NULL;
  list->count = 0;
  list->capacity = 0;
}

typedef struct
{
  todo_id_list_t *list;
  uint64_t user_id;
  bool error;
} lookup_ctx_t;

static int lookup_scan_cb(const char *key, size_t key_length,
                          const char *value, size_t value_length, void *ctx)
{
  (void)value;
  (void)value_length;
  lookup_ctx_t *lookup = (lookup_ctx_t *)ctx;

  // The prefix already restricts the range to this user; re-parsing keeps a
  // malformed key in that range from being read as an id.
  uint64_t user_id = 0;
  uint64_t todo_id = 0;
  if (!todo_index_key_parse(key, key_length, &user_id, &todo_id) ||
      user_id != lookup->user_id)
  {
    return 0;
  }

  if (id_list_push(lookup->list, todo_id) != 0)
  {
    lookup->error = true;
    return 1; // Stop the scan
  }
  return 0;
}

int todo_index_lookup(transaction_t *txn, uint64_t user_id, todo_id_list_t *out)
{
  if (!out)
  {
    return -1;
  }

  memset(out, 0, sizeof(*out));

  char prefix[TODO_KEY_MAX];
  int prefix_len = todo_index_prefix(prefix, sizeof(prefix), user_id);
  if (prefix_len < 0)
  {
    return -1;
  }

  lookup_ctx_t lookup = {.list = out, .user_id = user_id, .error = false};

  if (db_scan_prefix(txn, prefix, (size_t)prefix_len, lookup_scan_cb, &lookup) != 0 ||
      lookup.error)
  {
    todo_id_list_free(out);
    return -1;
  }

  return 0;
}

// ---- Reconcile --------------------------------------------------------------

// One (owner, todo) pair. Reconciling is a set difference over these: the pairs
// the rows call for, against the pairs the index currently holds.
typedef struct
{
  uint64_t user_id;
  uint64_t todo_id;
} owner_pair_t;

typedef struct
{
  owner_pair_t *items;
  size_t count;
  size_t capacity;
} pair_list_t;

static int pair_list_push(pair_list_t *list, uint64_t user_id, uint64_t todo_id)
{
  if (list->count == list->capacity)
  {
    size_t new_capacity = list->capacity ? list->capacity * 2 : 64;
    owner_pair_t *grown = realloc(list->items, new_capacity * sizeof(*grown));
    if (!grown)
    {
      return -1;
    }
    list->items = grown;
    list->capacity = new_capacity;
  }

  list->items[list->count].user_id = user_id;
  list->items[list->count].todo_id = todo_id;
  list->count++;
  return 0;
}

static int pair_compare(const void *a, const void *b)
{
  const owner_pair_t *pa = (const owner_pair_t *)a;
  const owner_pair_t *pb = (const owner_pair_t *)b;

  if (pa->user_id != pb->user_id)
  {
    return pa->user_id < pb->user_id ? -1 : 1;
  }
  if (pa->todo_id != pb->todo_id)
  {
    return pa->todo_id < pb->todo_id ? -1 : 1;
  }
  return 0;
}

typedef struct
{
  pair_list_t wanted; // (owner, id) the todo rows call for
  pair_list_t have;   // (owner, id) the index currently holds
  uint64_t max_todo_id;
  bool error;
} reconcile_ctx_t;

static int reconcile_scan_cb(const char *key, size_t key_length,
                             const char *value, size_t value_length, void *ctx)
{
  reconcile_ctx_t *r = (reconcile_ctx_t *)ctx;

  uint64_t todo_id = 0;
  uint64_t user_id = 0;

  if (todo_row_key_parse(key, key_length, &todo_id))
  {
    if (todo_id > r->max_todo_id)
    {
      r->max_todo_id = todo_id;
    }

    // Reading the owner out of the stored JSON is the one place this module
    // looks inside a value. A row with no user_id predates accounts and belongs
    // to nobody, so it gets no entry -- matching the ownership check the
    // handlers apply, which never matches such a row either.
    if (json_get_uint64(value, value_length, "user_id", &user_id) &&
        pair_list_push(&r->wanted, user_id, todo_id) != 0)
    {
      r->error = true;
      return 1;
    }
    return 0;
  }

  if (todo_index_key_parse(key, key_length, &user_id, &todo_id) &&
      pair_list_push(&r->have, user_id, todo_id) != 0)
  {
    r->error = true;
    return 1;
  }

  // Anything else (user account records, future namespaces) is not ours.
  return 0;
}

int todo_index_reconcile(uint64_t *max_todo_id_out)
{
  reconcile_ctx_t r;
  memset(&r, 0, sizeof(r));

  g_index_ready = false;

  transaction_t *txn = begin_transaction();
  if (!txn)
  {
    fprintf(stderr, "todo index: cannot open a transaction to reconcile\n");
    return -1;
  }

  int rc = db_scan(txn, reconcile_scan_cb, &r);
  commit_transaction(txn);
  free(txn);

  if (max_todo_id_out)
  {
    *max_todo_id_out = r.max_todo_id;
  }

  if (rc != 0 || r.error)
  {
    fprintf(stderr, "todo index: scan failed, listings will fall back to a full scan\n");
    free(r.wanted.items);
    free(r.have.items);
    return -1;
  }

  // Both sides sorted the same way turns the set difference into one merge. An
  // empty side has a NULL base, which qsort may not be handed even for a count
  // of zero.
  if (r.wanted.count > 0)
  {
    qsort(r.wanted.items, r.wanted.count, sizeof(*r.wanted.items), pair_compare);
  }
  if (r.have.count > 0)
  {
    qsort(r.have.items, r.have.count, sizeof(*r.have.items), pair_compare);
  }

  size_t inserted = 0;
  size_t deleted = 0;
  size_t failed = 0;

  // Opened on the first repair, so a healthy index costs no write at all.
  transaction_t *repair = NULL;

  size_t i = 0;
  size_t j = 0;
  while (i < r.wanted.count || j < r.have.count)
  {
    int cmp;
    if (i >= r.wanted.count)
    {
      cmp = 1; // Only leftovers in the index: orphans to drop
    }
    else if (j >= r.have.count)
    {
      cmp = -1; // Only leftovers in the rows: entries to add
    }
    else
    {
      cmp = pair_compare(&r.wanted.items[i], &r.have.items[j]);
    }

    if (cmp == 0)
    {
      i++;
      j++;
      continue; // Already in step
    }

    if (!repair)
    {
      repair = begin_transaction();
      if (!repair)
      {
        failed++;
        break;
      }
    }

    if (cmp < 0)
    {
      // A todo with no entry: invisible to an index-backed listing until now.
      if (todo_index_insert(repair, r.wanted.items[i].user_id,
                            r.wanted.items[i].todo_id) == 0)
      {
        inserted++;
      }
      else
      {
        failed++;
      }
      i++;
    }
    else
    {
      // An entry whose row is gone, or whose owner changed -- a changed owner
      // shows up as both an orphan here and a missing entry above.
      if (todo_index_delete(repair, r.have.items[j].user_id,
                            r.have.items[j].todo_id) == 0)
      {
        deleted++;
      }
      else
      {
        failed++;
      }
      j++;
    }
  }

  if (repair)
  {
    commit_transaction(repair);
    free(repair);
  }

  free(r.wanted.items);
  free(r.have.items);

  if (inserted || deleted)
  {
    printf("todo index: reconciled (%zu entries added, %zu dropped)\n",
           inserted, deleted);
  }

  if (failed)
  {
    // Most plausibly a full page: the index roughly doubles the space a todo
    // takes and a page split is not implemented. Serving listings from a
    // partial index would silently hide todos, so stay on the full scan.
    fprintf(stderr,
            "todo index: %zu entries could not be written (page full?) -- "
            "listings fall back to a full scan\n",
            failed);
    return -1;
  }

  g_index_ready = true;
  return 0;
}

bool todo_index_available(void)
{
  return g_index_ready;
}
