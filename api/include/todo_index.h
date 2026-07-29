#ifndef TODO_INDEX_H
#define TODO_INDEX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "storage.h"

// Owner of the todo key space in the shared btree, and of the secondary index
// that makes "list one user's todos" cheap.
//
// Two kinds of key live in this namespace:
//
//   <id>                     a todo row -- a bare decimal id, whose value is
//                            the canonical todo JSON
//   idx:user:<uid>:<id>      an index entry -- one per todo, with an empty
//                            value (everything it carries is in the key)
//
// Both ids in an index key are zero-padded to a fixed width, so the btree's
// lexicographic order is numeric order and one user's entries form a single
// contiguous range. Listing a user's todos is then a prefix scan over that
// range plus a lookup per row in the requested page, instead of a full-table
// scan that reads -- and throws away -- every other user's rows.
//
// The index is redundant state: it can only be wrong, never authoritative. The
// engine's abort path has no undo yet (storage/src/txn.c), so a crash between
// the row write and the entry write can leave the two disagreeing.
// todo_index_reconcile() rebuilds it from the rows at startup, and is also
// where the id counter gets its seed, so the repair costs no extra scan.

// Longest key this module builds, index entries included.
#define TODO_KEY_MAX 64

// Render the row key for a todo id. Returns the key length, or -1 if out_size
// is too small.
int todo_row_key(char *out, size_t out_size, uint64_t id);

// True when `key` is a todo row key (a non-empty, non-overflowing run of
// decimal digits), in which case *id_out receives the id. Other record types
// share the btree -- user accounts under "user:<name>", index entries under
// "idx:" -- so every scan over todos has to filter with this.
bool todo_row_key_parse(const char *key, size_t key_length, uint64_t *id_out);

// Render an index key, or the prefix selecting every entry of one user.
// Both return the length written, or -1 if out_size is too small.
int todo_index_key(char *out, size_t out_size, uint64_t user_id, uint64_t todo_id);
int todo_index_prefix(char *out, size_t out_size, uint64_t user_id);

// True when `key` is an index entry, in which case the owner and todo ids are
// written to the out params.
bool todo_index_key_parse(const char *key, size_t key_length,
                          uint64_t *user_id_out, uint64_t *todo_id_out);

// Add / remove the entry for one todo. Both take the caller's transaction so
// the entry commits with the row it describes. Return 0 on success, -1 on
// failure (including "already there" / "not there", which db_insert and
// db_delete both report as -1).
int todo_index_insert(transaction_t *txn, uint64_t user_id, uint64_t todo_id);
int todo_index_delete(transaction_t *txn, uint64_t user_id, uint64_t todo_id);

// Ascending list of todo ids owned by a user, filled by todo_index_lookup().
// The caller frees it with todo_id_list_free().
typedef struct
{
  uint64_t *ids;
  size_t count;
  size_t capacity;
} todo_id_list_t;

// Collect the ids of every todo owned by user_id, in ascending id order, via a
// prefix scan of the index. Returns 0 on success, -1 on failure (the list is
// left empty and needs no freeing on failure, though freeing it is safe).
int todo_index_lookup(transaction_t *txn, uint64_t user_id, todo_id_list_t *out);
void todo_id_list_free(todo_id_list_t *list);

// Rebuild the index from the todo rows: add the entries that are missing, drop
// the ones whose row is gone or whose owner no longer matches. Also reports the
// highest todo id seen, which seeds the id counter. Returns 0 when the index is
// fully in step with the rows, -1 otherwise.
//
// Call once at startup, before serving.
int todo_index_reconcile(uint64_t *max_todo_id_out);

// False when the index could not be brought in step with the rows -- most
// plausibly because the page filled up, since a page split is not implemented.
// Listing falls back to a full scan while this is false rather than answering
// from an index that would silently hide todos.
bool todo_index_available(void);

#endif
