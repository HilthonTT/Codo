#ifndef LOCKFREE_H
#define LOCKFREE_H

#include <stdatomic.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#define MAX_THREAD 64
#define MAX_HAZARD_POINTERS 8

#define HASH_TABLE_SIZE 1024
#define HASH_LOAD_FACTOR 0.75

#define MAX_LEVEL 16

typedef struct hazard_pointer
{
  _Atomic(void *) pointer;
  atomic_bool active;
} hazard_pointer_t;

typedef struct hazard_pointer_record
{
  hazard_pointer_t pointers[MAX_HAZARD_POINTERS];
  atomic_bool active;
  pthread_t thread_id;
} hazard_pointer_record_t;

// Lock-free queue with hazard pointers
typedef struct queue_node
{
  _Atomic(void *) data;
  _Atomic(struct queue_node *) next;
} queue_node_t;

typedef struct
{
  _Atomic(queue_node_t *) head;
  _Atomic(queue_node_t *) tail;
  atomic_size_t size;
} lockfree_queue_t;

typedef struct hash_node
{
  atomic_uintptr_t key;
  _Atomic(void *) value;
  _Atomic(struct hash_node *) next;
  atomic_bool deleted;
} hash_node_t;

typedef struct
{
  _Atomic(hash_node_t *) buckets[HASH_TABLE_SIZE];
  atomic_size_t size;
  atomic_size_t capacity;
} lockfree_hashtable_t;

typedef struct skip_node
{
  atomic_long key;
  _Atomic(void *) value;
  atomic_int level;
  _Atomic(struct skip_node *) forward[MAX_LEVEL];
  atomic_bool deleted;
} skip_node_t;

typedef struct
{
  skip_node_t *header;
  atomic_int max_level;
  atomic_size_t size;
} lockfree_skiplist_t;

hazard_pointer_record_t *get_hazard_pointer_record(void);
void set_hazard_pointer(int index, void *ptr);
void clear_hazard_pointer(int index);
bool is_hazard_pointer(void *ptr);

lockfree_queue_t *lockfree_queue_create(void);
bool lock_free_queue_enqueue(lockfree_queue_t *queue, void *data);
bool lock_free_queue_dequeue(lockfree_queue_t *queue, void **data);

lockfree_hashtable_t *lockfree_hashtable_create(void);
bool lockfree_hashtable_insert(lockfree_hashtable_t *table, uintptr_t key, void *value);
bool lockfree_hashtable_lookup(lockfree_hashtable_t *table, uintptr_t key, void **value);

bool lockfree_skiplist_insert(lockfree_skiplist_t *list, long key, void *value);
bool lockfree_skiplist_search(lockfree_skiplist_t *list, long key, void **value);

#endif
