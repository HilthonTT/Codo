#ifndef BTREE_STORAGE_H
#define BTREE_STORAGE_H

#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include <stdbool.h>

#define PAGE_SIZE 4096
#define MAX_KEY_SIZE 256
#define MAX_VALUE_SIZE 1024
#define MAX_PAGES 1000000
#define BTREE_ORDER 128
#define BUFFER_POOL_SIZE 10000
#define WAL_BUFFER_SIZE (1024 * 1024)

// Page types
typedef enum {
  PAGE_TYPE_LEAF = 1,
  PAGE_TYPE_INTERNAL = 2,
  PAGE_TYPE_OVERFLOW = 3,
  PAGE_TYPE_FREE = 4,
} page_type_t;

// Lock types
typedef enum {
  LOCK_NONE = 0,
  LOCK_SHARED = 1,
  LOCK_EXCLUSIVE = 2,
  LOCK_UPDATE = 3,
} lock_type_t;

// Transaction types
typedef enum {
  TXN_STATE_ACTIVE,
  TXN_STATE_COMMITED,
  TXN_STATE_ABORTED,
  TXN_STATE_PREPARED,
} transaction_state_t;

// WAL record types
typedef enum {
  WAL_INSERT = 1,
  WAL_UPDATE = 2,
  WAL_DELETE = 3,
  WAL_COMMIT = 4,
  WAL_ABORT = 5,
  WAL_CHECKOUT = 6,
} wal_record_type_t;

// Page header structure
typedef struct {
  uint32_t page_id;
  page_type_t page_type;
  uint16_t key_count;
  uint16_t free_space;
  uint32_t parent_page_id;
  uint32_t next_page_id;
  uint32_t prev_page_id;
  uint64_t lsn; // Log Sequence Number
  uint32_t checksum;
  char reserved[32];
} page_header_t;

// Key-value pair structure
typedef struct {
  uint16_t key_length;
  uint16_t value_length;
  uint32_t child_page_id; // For internal node
  char data[]; // Key followed by value
} kv_pair_t;

// B-tree page structure
typedef struct {
  page_header_t header;
  char data[PAGE_SIZE - sizeof(page_header_t)];
} btree_page_t;

// Buffer pool entry
typedef struct {
  btree_page_t* page;
  uint32_t page_id;
  bool dirty;
  bool pinned;
  _Atomic int ref_count;
  pthread_rwlock_t page_lock;
  struct timespec last_access;
  uint32_t hash_next; // For hash table chaining
} buffer_entry_t;

// Transaction structure
typedef struct transaction {
  uint64_t txn_id;
  transaction_state_t state;
  uint64_t start_lsn;
  uint64_t commit_lsn;
  time_t start_time;
  time_t commit_time;

  // Lock table
  struct lock_entry* locks;
  pthread_mutex_t lock_mutex;

  // Undo log
  struct undo_entry* undo_log;
  size_t undo_count;

  // Statistics
  struct {
    uint64_t pages_read;
    uint64_t pages_written;
    uint64_t rows_inserted;
    uint64_t rows_updated;
    uint64_t rows_deleted;
  } stats;

  struct transaction* next;
} transaction_t;

// Lock entry
typedef struct lock_entry {
  uint32_t page_id;
  uint64_t key_hash;
  lock_type_t lock_type;
  transaction_t* owner;
  struct lock_entry* next_in_txn;
  struct lock_entry* next_in_table;
} lock_entry_t;

// Undo log entry
typedef struct undo_entry {
  wal_record_type_t operation;
  uint32_t page_id;
  uint16_t slot_id;
  uint16_t key_length;
  uint16_t old_value_length;
  char* key_data;
  char* old_value_data;
  struct undo_entry* next;
} undo_entry_t;

// WAL record
typedef struct {
  uint64_t lsn;
  uint64_t txn_id;
  wal_record_type_t type;
  uint32_t page_id;
  uint32_t data_length;
  uint32_t checksum;
  char data[];
} wal_record_t;

// Storage engine context
typedef struct {
  // File management
  int data_fd;
  int wal_fd;
  char *data_filename;
  char *wal_filename;

  // Buffer pool
  buffer_entry_t buffer_pool[BUFFER_POOL_SIZE];
  uint32_t* hash_table;
  size_t hash_table_size;
  pthread_mutex_t buffer_mutex;

  // Free page management
  uint32_t* free_pages;
  size_t free_page_count;
  size_t free_page_capacity;
  uint32_t next_page_id;
  pthread_mutex_t free_page_mutex;

  // Transaction management
  transaction_t* active_transactions;
  uint64_t next_txn_id;
  pthread_mutex_t txn_mutex;

  // Lock table
  lock_entry_t** lock_table;
  size_t lock_table_size;
  pthread_mutex_t lock_table_mutex;

  // WAL management
  uint8_t* wal_buffer;
  size_t wal_buffer_pos;
  uint64_t next_lsn;
  uint64_t last_checkpoint_lsn;
  pthread_mutex_t wal_mutex;

  // Root page
  uint32_t root_page_id;

  // Statistics
  struct {
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
  struct {
    bool enable_checksums;
    bool enable_wal;
    bool enable_compression;
    size_t checkpoint_interval;
    size_t wal_segment_size;
    double buffer_pool_hit_ratio_target;
  } config;

} storage_engine_t;

// Utility functions
uint64_t hash_key(const char *key, size_t length);
uint32_t hash_page_id(uint32_t page_id);
uint32_t calculate_checksum(const void* data, size_t length);
uint64_t allocate_lsn(void);

int write_wal_record(uint64_t txn_id, wal_record_type_t type, uint32_t page_id, const void* data, size_t data_length);
int flush_wal_buffer(void);
buffer_entry_t* find_buffer_entry(uint32_t page_id);
buffer_entry_t* allocate_buffer_entry(uint32_t page_id);
btree_page_t* get_page(uint32_t page_id, lock_type_t lock_type);

#endif