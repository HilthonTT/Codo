#ifndef LRU_CACHE_H
#define LRU_CACHE_H

// Fixed-capacity LRU cache: a chained hash table for O(1) lookup plus a
// doubly-linked recency list. The list head is the least recently used entry
// (the eviction victim) and the tail is the most recently used.
//
// The cache owns its keys and values -- put/fill copy the caller's buffers, so
// a handler may fill from a stack buffer and let it go out of scope. Every
// operation is safe to call from multiple threads.

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct lru_node
{
    char *key;
    char *value;
    size_t value_length;
    struct lru_node *prev;      // recency list: towards the LRU end
    struct lru_node *next;      // recency list: towards the MRU end
    struct lru_node *hash_next; // bucket chain
} lru_node_t;

typedef struct
{
    int capacity;
    lru_node_t **array;
} lru_table_t;

typedef struct
{
    int size;
    int capacity;
    lru_node_t *head; // least recently used
    lru_node_t *tail; // most recently used
} lru_list_t;

typedef struct
{
    lru_table_t *table;
    lru_list_t *list;
    pthread_mutex_t lock;
    _Atomic uint64_t generation;
    _Atomic uint64_t hits;
    _Atomic uint64_t misses;
    _Atomic uint64_t evictions;
} lru_cache_t;

// capacity is the maximum number of entries. Returns NULL on a non-positive
// capacity or on allocation failure.
lru_cache_t *lru_cache_create(int capacity);
void lru_cache_destroy(lru_cache_t *cache);

// On a hit, copies up to *value_length bytes into value and sets *value_length
// to the stored value's full length -- the same contract as db_search(), so a
// cached read and a storage read are interchangeable at the call site. Returns
// false on a miss, leaving value and *value_length untouched.
bool lru_get(lru_cache_t *cache, const char *key, char *value, size_t *value_length);

// Authoritative write: insert or replace. Call it *after* the write to the
// backing store has committed. Returns 0 on success, -1 on allocation failure.
int lru_put(lru_cache_t *cache, const char *key, const char *value, size_t value_length);

// Drop key. Call after a committed delete. Returns true if an entry was removed.
bool lru_invalidate(lru_cache_t *cache, const char *key);

// Snapshot the generation counter *before* reading the backing store on a miss,
// then hand it back to lru_fill(). Both lru_put() and lru_invalidate() bump the
// counter, so a fill whose snapshot is stale gets discarded instead of
// resurrecting a value that a concurrent writer has already superseded.
uint64_t lru_generation(lru_cache_t *cache);

// Speculative fill from a backing-store read. Returns 0 if the value was
// cached, 1 if it was dropped because a write landed while the read was in
// flight, -1 on allocation failure.
int lru_fill(lru_cache_t *cache, const char *key, const char *value,
             size_t value_length, uint64_t generation);

// Snapshot the counters. Any out pointer may be NULL.
void lru_stats(lru_cache_t *cache, uint64_t *hits, uint64_t *misses,
               uint64_t *evictions, uint64_t *size);

#endif
