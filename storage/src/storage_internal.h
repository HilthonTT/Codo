#ifndef STORAGE_INTERNAL_H
#define STORAGE_INTERNAL_H

// Private data structures and cross-file helpers of the storage engine.
// Nothing outside storage/src may include this header; the public contract is
// storage/include/storage.h. The engine is split by concern:
//
//   engine.c  singleton definition, init/cleanup, checkpoint, statistics
//   wal.c     LSN allocation, WAL record append + flush (write-ahead ordering)
//   pager.c   buffer pool (hash lookup, LRU eviction, pin/lock), page IO,
//             free-page management, checksums
//   btree.c   page-level key operations (search, insert, delete within a page)
//   txn.c     transaction lifecycle (begin/commit/abort)
//   db.c      public CRUD/scan API: tree descent on top of the layers above

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "storage.h"

#define PAGE_SIZE 4096
#define MAX_PAGES 1000000
#define BTREE_ORDER 128
#define BUFFER_POOL_SIZE 10000
#define WAL_BUFFER_SIZE (1024 * 1024)
// Deepest root-to-leaf path a descent or split will follow. A 4 KiB internal
// page holds at least 15 separators, so a real tree never comes close; the
// limit is a backstop that turns a corrupt page cycle into an error return
// instead of an infinite loop.
#define BTREE_MAX_DEPTH 32

// Page types
typedef enum
{
  PAGE_TYPE_LEAF = 1,
  PAGE_TYPE_INTERNAL = 2,
  PAGE_TYPE_OVERFLOW = 3,
  PAGE_TYPE_FREE = 4,
} page_type_t;

// Lock types
typedef enum
{
  LOCK_NONE = 0,
  LOCK_SHARED = 1,
  LOCK_EXCLUSIVE = 2,
} lock_type_t;

// Transaction states
typedef enum
{
  TXN_STATE_ACTIVE,
  TXN_STATE_COMMITED,
  TXN_STATE_ABORTED,
} transaction_state_t;

// WAL record types
typedef enum
{
  WAL_INSERT = 1,
  WAL_UPDATE = 2,
  WAL_DELETE = 3,
  WAL_COMMIT = 4,
  WAL_ABORT = 5,
  WAL_CHECKOUT = 6,
} wal_record_type_t;

// Page header structure
typedef struct
{
  uint32_t page_id;
  page_type_t page_type;
  uint16_t key_count;
  uint16_t free_space;
  uint32_t reserved0; // was parent_page_id; never maintained. Kept so the
                      // on-disk page layout is unchanged.
  uint32_t next_page_id;
  uint32_t prev_page_id;
  uint64_t lsn; // Log Sequence Number
  uint32_t checksum;
  char reserved[32];
} page_header_t;

// Key-value pair structure.
//
// These are packed back-to-back into a page's data area at variable offsets
// (each pair spans sizeof(kv_pair_t) + key_length + value_length bytes), so a
// pair almost never lands on a 4-byte boundary. Accessing the uint32_t
// child_page_id through such a pointer is undefined behaviour (and faults on
// strict-alignment CPUs), which UBSan flags. Marking the struct packed tells
// the compiler these accesses may be unaligned so it emits safe byte-wise
// loads/stores. The layout is unchanged -- {u16,u16,u32} is already 8 bytes
// with no padding -- so the on-disk format is identical.
typedef struct __attribute__((packed))
{
  uint16_t key_length;
  uint16_t value_length;
  uint32_t child_page_id; // For internal node
  char data[];            // Key followed by value
} kv_pair_t;

// B-tree page structure
typedef struct
{
  page_header_t header;
  char data[PAGE_SIZE - sizeof(page_header_t)];
} btree_page_t;

// Buffer pool entry
typedef struct
{
  btree_page_t *page;
  uint32_t page_id;
  bool dirty;
  bool pinned;
  _Atomic int ref_count;
  pthread_rwlock_t page_lock;
  struct timespec last_access;
  uint32_t hash_next; // For hash table chaining
} buffer_entry_t;

// Transaction structure (public alias: transaction_t, opaque in storage.h)
struct transaction
{
  uint64_t txn_id;
  transaction_state_t state;
  uint64_t start_lsn;
  uint64_t commit_lsn;
  time_t start_time;
  time_t commit_time;

  // Statistics
  struct
  {
    uint64_t pages_read;
    uint64_t pages_written;
    uint64_t rows_inserted;
    uint64_t rows_updated;
    uint64_t rows_deleted;
  } stats;

  struct transaction *next;
};

// WAL record
typedef struct
{
  uint64_t lsn;
  uint64_t txn_id;
  wal_record_type_t type;
  uint32_t page_id;
  uint32_t data_length;
  uint32_t checksum;
  char data[];
} wal_record_t;

// Storage engine context
typedef struct
{
  // File management
  int data_fd;
  int wal_fd;
  char *data_filename;
  char *wal_filename;

  // Buffer pool
  buffer_entry_t buffer_pool[BUFFER_POOL_SIZE];
  uint32_t *hash_table;
  size_t hash_table_size;
  pthread_mutex_t buffer_mutex;

  // Free page management. Removed once as dead state, and restored here
  // because btree_split_node() allocates a page per new node. next_page_id is
  // recovered from the data file's extent at startup (see init_storage_engine);
  // the free list is in-memory only, so ids freed before a restart are not
  // reused after one.
  uint32_t *free_pages;
  size_t free_page_count;
  size_t free_page_capacity;
  uint32_t next_page_id;
  pthread_mutex_t free_page_mutex;

  // Transaction management
  transaction_t *active_transactions;
  uint64_t next_txn_id;
  pthread_mutex_t txn_mutex;

  // WAL management
  uint8_t *wal_buffer;
  size_t wal_buffer_pos;
  _Atomic uint64_t next_lsn;
  uint64_t last_checkpoint_lsn;
  pthread_mutex_t wal_mutex;

  // Root page
  uint32_t root_page_id;

  // Guards the *shape* of the tree (which page is a node's parent/child/
  // sibling), as opposed to the contents of any one page, which the pager's
  // per-page rwlocks already protect. A split rewires several pages at once,
  // and a reader descending through that rewiring could follow a pointer that
  // is briefly stale, so structural writers take this exclusively and everyone
  // else takes it shared. Coarse but correct; the finer-grained fix is
  // latch-coupling (crab-latching) the descent, which also retires the
  // TODO(concurrency) in db.c.
  pthread_rwlock_t tree_lock;

  // Statistics
  struct
  {
    _Atomic uint64_t pages_read;
    _Atomic uint64_t pages_written;
    _Atomic uint64_t cache_hits;
    _Atomic uint64_t cache_misses;
    _Atomic uint64_t transactions_committed;
    _Atomic uint64_t transactions_aborted;
    _Atomic uint64_t wal_records_written;
    _Atomic uint64_t checkpoints_performed;
  } stats;

  // Configuration
  struct
  {
    bool enable_checksums;
    bool enable_wal;
    bool enable_compression;
    size_t checkpoint_interval;
    size_t wal_segment_size;
    double buffer_pool_hit_ratio_target;
  } config;

} storage_engine_t;

// The process-wide engine singleton, defined in engine.c.
extern storage_engine_t g_storage;

// wal.c
uint64_t allocate_lsn(void);
int write_wal_record(uint64_t txn_id, wal_record_type_t type, uint32_t page_id,
                     const void *data, size_t data_length);
int flush_wal_buffer(void);

// pager.c
uint32_t hash_page_id(uint32_t page_id);
uint32_t calculate_checksum(const void *data, size_t length);
buffer_entry_t *find_buffer_entry(uint32_t page_id);
buffer_entry_t *allocate_buffer_entry(uint32_t page_id);
btree_page_t *get_page(uint32_t page_id, lock_type_t lock_type);
void release_page(uint32_t page_id, lock_type_t lock_type);
void mark_page_dirty(uint32_t page_id);
uint32_t allocate_page(void);
void deallocate_page(uint32_t page_id);

// btree.c
int compare_keys(const char *key1, size_t len1, const char *key2, size_t len2);
kv_pair_t *get_kv_pair(btree_page_t *page, int index);
int find_key_position(btree_page_t *page, const char *key, size_t key_length);
int insert_kv_pair(
    btree_page_t *page,
    int position,
    const char *key,
    size_t key_length,
    const char *value,
    size_t value_length,
    uint32_t child_page_id);
int delete_kv_pair(btree_page_t *page, int position);
// Bytes of a page available to kv pairs (the page minus its header).
size_t btree_page_capacity(void);
// Bytes currently occupied by `page`'s pairs, or SIZE_MAX if free_space is
// corrupt.
size_t btree_used_bytes(btree_page_t *page);
// Move the upper half of `src`'s entries into the empty page `dst` (which will
// live at `dst_page_id`) and write the separator key that routes to `src` into
// sep_out. Pure page surgery -- no engine state is touched, so the caller owns
// publishing `dst` into the parent. Returns 0 on success, -1 if `src` cannot be
// split (too few entries, or corrupt).
int btree_split_page(btree_page_t *src, btree_page_t *dst, uint32_t dst_page_id,
                     char *sep_out, size_t sep_out_size, size_t *sep_len_out);

// db.c
// Split `node_id` in two, publishing the new right sibling in its parent (and
// growing a new root when `node_id` is the root). The caller must hold
// tree_lock exclusively and hold no page lock. `depth` bounds the recursion
// when the split cascades upward. Returns 0 on success.
int btree_split_node(uint32_t node_id, int depth);

#endif
