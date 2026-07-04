#include "lockfree.h"

static hazard_pointer_record_t hazard_pointer_table[MAX_THREAD];
static _Atomic(hazard_pointer_record_t *) hazard_pointer_head = NULL;

// Thread-local hazard pointer record
static __thread hazard_pointer_record_t *local_hazard_record = NULL;

hazard_pointer_record_t *get_hazard_pointer_record(void)
{
  return NULL;
}

void set_hazard_pointer(int index, void *ptr)
{
}

void clear_hazard_pointer(int index)
{
}

bool is_hazard_pointer(void *ptr)
{
  return false;
}

lockfree_queue_t *lockfree_queue_create(void)
{
  return NULL;
}

bool lock_free_queue_enqueue(lockfree_queue_t *queue, void *data)
{
  return false;
}

bool lock_free_queue_dequeue(lockfree_queue_t *queue, void **data)
{
  return false;
}

lockfree_hashtable_t *lockfree_hashtable_create(void)
{
  return NULL;
}

bool lockfree_hashtable_insert(lockfree_hashtable_t *table, uintptr_t key, void *value)
{
  return false;
}

bool lockfree_hashtable_lookup(lockfree_hashtable_t *table, uintptr_t key, void **value)
{
  return false;
}

bool lockfree_skiplist_insert(lockfree_skiplist_t *list, long key, void *value)
{
  return false;
}

bool lockfree_skiplist_search(lockfree_skiplist_t *list, long key, void **value)
{
  return false;
}
