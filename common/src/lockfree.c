#define _GNU_SOURCE

#include <stdlib.h>
#include <limits.h>

#include "lockfree.h"

static hazard_pointer_record_t hazard_pointer_table[MAX_THREADS];

// Thread-local hazard pointer record
static __thread hazard_pointer_record_t *local_hazard_record = NULL;

// LIMITATION: a hazard-pointer record is claimed lazily on first use by a thread
// and is never released when that thread exits -- there is no thread-exit hook
// wired up here. A long-lived process that spawns and joins more than
// MAX_THREADS distinct threads over its lifetime will therefore eventually run
// out of slots (get_hazard_pointer_record returns NULL). A full fix needs a
// destructor registered via pthread_key_create / pthread_setspecific (or an
// explicit "retire this thread's record" call). This module currently has no
// callers, so the limitation is documented rather than fixed to avoid adding an
// unused teardown path.
hazard_pointer_record_t *get_hazard_pointer_record(void)
{
  if (local_hazard_record)
  {
    return local_hazard_record;
  }

  for (int i = 0; i < MAX_THREADS; i++)
  {
    if (!atomic_load(&hazard_pointer_table[i].active))
    {
      bool expected = false;
      if (atomic_compare_exchange_strong(&hazard_pointer_table[i].active, &expected, true))
      {
        hazard_pointer_table[i].thread_id = pthread_self();
        local_hazard_record = &hazard_pointer_table[i];

        // Initialize hazard pointers
        for (int j = 0; j < MAX_HAZARD_POINTERS; j++)
        {
          atomic_store(&hazard_pointer_table[i].pointers[j].pointer, NULL);
          atomic_store(&hazard_pointer_table[i].pointers[j].active, false);
        }

        return local_hazard_record;
      }
    }
  }

  return NULL; // No available slots
}

void set_hazard_pointer(int index, void *ptr)
{
  hazard_pointer_record_t *record = get_hazard_pointer_record();
  if (record && index < MAX_HAZARD_POINTERS)
  {
    atomic_store(&record->pointers[index].pointer, ptr);
    atomic_store(&record->pointers[index].active, true);
  }
}

void clear_hazard_pointer(int index)
{
  hazard_pointer_record_t *record = get_hazard_pointer_record();
  if (record && index < MAX_HAZARD_POINTERS)
  {
    atomic_store(&record->pointers[index].pointer, NULL);
    atomic_store(&record->pointers[index].active, false);
  }
}

bool is_hazard_pointer(void *ptr)
{
  for (int i = 0; i < MAX_THREADS; i++)
  {
    if (atomic_load(&hazard_pointer_table[i].active))
    {
      for (int j = 0; j < MAX_HAZARD_POINTERS; j++)
      {
        if (atomic_load(&hazard_pointer_table[i].pointers[j].active) &&
            atomic_load(&hazard_pointer_table[i].pointers[j].pointer) == ptr)
        {
          return true;
        }
      }
    }
  }

  return false;
}

lockfree_queue_t *lockfree_queue_create(void)
{
  lockfree_queue_t *queue = malloc(sizeof(lockfree_queue_t));
  if (!queue)
  {
    return NULL;
  }

  queue_node_t *dummy = malloc(sizeof(queue_node_t));
  if (!dummy)
  {
    free(queue);
    return NULL;
  }

  atomic_store(&dummy->data, NULL);
  atomic_store(&dummy->next, NULL);

  atomic_store(&queue->head, dummy);
  atomic_store(&queue->tail, dummy);
  atomic_store(&queue->size, 0);

  return queue;
}

bool lock_free_queue_enqueue(lockfree_queue_t *queue, void *data)
{
  queue_node_t *new_node = malloc(sizeof(queue_node_t));
  if (!new_node)
  {
    return false;
  }

  atomic_store(&new_node->data, data);
  atomic_store(&new_node->next, NULL);

  while (true)
  {
    queue_node_t *tail = atomic_load(&queue->tail);
    set_hazard_pointer(0, tail);

    // Verify tail is still valid
    if (tail != atomic_load(&queue->tail))
    {
      continue;
    }

    queue_node_t *next = atomic_load(&tail->next);

    if (tail == atomic_load(&queue->tail))
    {
      if (next == NULL)
      {
        // Try to link new node at the end of the list
        if (atomic_compare_exchange_weak(&tail->next, &next, new_node))
        {
          // Try to swing tail to the new node
          atomic_compare_exchange_weak(&queue->tail, &tail, new_node);
          atomic_fetch_add(&queue->size, 1);
          clear_hazard_pointer(0);
          return true;
        }
      }
      else
      {
        // Try to swing tail to the next node
        atomic_compare_exchange_weak(&queue->tail, &tail, next);
      }
    }
  }

  return false;
}

// Dequeue operation
bool lockfree_queue_dequeue(lockfree_queue_t *queue, void **data)
{
  while (true)
  {
    queue_node_t *head = atomic_load(&queue->head);
    set_hazard_pointer(0, head);

    // Verify head is still valid
    if (head != atomic_load(&queue->head))
    {
      continue;
    }

    queue_node_t *tail = atomic_load(&queue->tail);
    queue_node_t *next = atomic_load(&head->next);
    set_hazard_pointer(1, next);

    if (head == atomic_load(&queue->head))
    {
      if (head == tail)
      {
        if (next == NULL)
        {
          // Queue is empty
          clear_hazard_pointer(0);
          clear_hazard_pointer(1);
          return false;
        }
        // Try to swing tail to next node
        atomic_compare_exchange_weak(&queue->tail, &tail, next);
      }
      else
      {
        if (next == NULL)
        {
          continue;
        }

        // Read data before CAS
        *data = atomic_load(&next->data);

        // Try to swing head to next node
        if (atomic_compare_exchange_weak(&queue->head, &head, next))
        {
          atomic_fetch_sub(&queue->size, 1);

          // Clear our OWN hazard pointer to head first: HP[0] still points at
          // head here, so is_hazard_pointer(head) would otherwise always match
          // this very thread and the node would never be freed (guaranteed
          // leak). After clearing it, the scan only sees OTHER threads.
          //
          // NOTE: reclamation is best-effort. Without a retire list this
          // check-then-free is still racy (another thread could publish a hazard
          // pointer to head between the scan and the free), but this at least
          // removes the guaranteed self-match leak.
          clear_hazard_pointer(0);
          if (!is_hazard_pointer(head))
          {
            free(head);
          }

          clear_hazard_pointer(1);
          return true;
        }
      }
    }
  }
}

static size_t hash_function(uintptr_t key)
{
  key ^= key >> 16;
  key *= 0x85ebca6b;
  key ^= key >> 13;
  key *= 0xc2b2ae35;
  key ^= key >> 16;
  return key % HASH_TABLE_SIZE;
}

// Create lock-free hash table
lockfree_hashtable_t *lockfree_hashtable_create(void)
{
  lockfree_hashtable_t *table = malloc(sizeof(lockfree_hashtable_t));
  if (!table)
    return NULL;

  for (int i = 0; i < HASH_TABLE_SIZE; i++)
  {
    atomic_store(&table->buckets[i], NULL);
  }

  atomic_store(&table->size, 0);
  atomic_store(&table->capacity, HASH_TABLE_SIZE);

  return table;
}

// Insert key-value pair
bool lockfree_hashtable_insert(lockfree_hashtable_t *table, uintptr_t key, void *value)
{
  size_t bucket = hash_function(key);

  hash_node_t *new_node = malloc(sizeof(hash_node_t));
  if (!new_node)
    return false;

  atomic_store(&new_node->key, key);
  atomic_store(&new_node->value, value);
  atomic_store(&new_node->deleted, false);

  while (true)
  {
    hash_node_t *head = atomic_load(&table->buckets[bucket]);
    atomic_store(&new_node->next, head);

    if (atomic_compare_exchange_weak(&table->buckets[bucket], &head, new_node))
    {
      atomic_fetch_add(&table->size, 1);
      return true;
    }
  }
}

// Lookup value by key
bool lockfree_hashtable_lookup(lockfree_hashtable_t *table, uintptr_t key, void **value)
{
  size_t bucket = hash_function(key);

  hash_node_t *current = atomic_load(&table->buckets[bucket]);
  set_hazard_pointer(0, current);

  // Walk the bucket chain. The previous revalidation compared every traversed
  // node to the bucket HEAD, so any node past the first always mismatched, reset
  // the walk back to the head, and spun forever. Insertions only ever prepend a
  // new head, so following next pointers reaches every node exactly once; we
  // simply traverse to the end. (Hazard-pointer protection remains best-effort.)
  while (current)
  {
    if (!atomic_load(&current->deleted) &&
        atomic_load(&current->key) == key)
    {
      *value = atomic_load(&current->value);
      clear_hazard_pointer(0);
      return true;
    }

    current = atomic_load(&current->next);
    set_hazard_pointer(0, current);
  }

  clear_hazard_pointer(0);
  return false;
}

// Random level generation
static int random_level(void)
{
  // rand() is not thread-safe (shared global state). Use rand_r with a
  // per-thread seed so concurrent inserters don't race on the RNG.
  static __thread unsigned int rng_seed = 0;
  if (rng_seed == 0)
  {
    rng_seed = (unsigned int)(uintptr_t)&rng_seed ^
               (unsigned int)(uintptr_t)pthread_self();
    if (rng_seed == 0)
    {
      rng_seed = 2463534242u;
    }
  }

  int level = 1;
  while ((rand_r(&rng_seed) & 0x1) && level < MAX_LEVEL)
  {
    level++;
  }
  return level;
}

// Create skip list
lockfree_skiplist_t *lockfree_skiplist_create(void)
{
  lockfree_skiplist_t *list = malloc(sizeof(lockfree_skiplist_t));
  if (!list)
    return NULL;

  list->header = malloc(sizeof(skip_node_t));
  if (!list->header)
  {
    free(list);
    return NULL;
  }

  atomic_store(&list->header->key, LONG_MIN);
  atomic_store(&list->header->value, NULL);
  atomic_store(&list->header->level, MAX_LEVEL);
  atomic_store(&list->header->deleted, false);

  for (int i = 0; i < MAX_LEVEL; i++)
  {
    atomic_store(&list->header->forward[i], NULL);
  }

  atomic_store(&list->max_level, 1);
  atomic_store(&list->size, 0);

  return list;
}

// Insert into skip list
bool lockfree_skiplist_insert(lockfree_skiplist_t *list, long key, void *value)
{
  skip_node_t *update[MAX_LEVEL];
  skip_node_t *current = list->header;

  // Snapshot the level we actually traverse so we can initialize any update[]
  // slots above it later (a concurrent raise of list->max_level must not leave
  // update[old_max .. level-1] uninitialized).
  int list_level = atomic_load(&list->max_level);

  // Find position to insert
  for (int i = list_level - 1; i >= 0; i--)
  {
    while (true)
    {
      skip_node_t *next = atomic_load(&current->forward[i]);
      if (!next || atomic_load(&next->key) >= key)
      {
        break;
      }
      current = next;
    }
    update[i] = current;
  }

  current = atomic_load(&current->forward[0]);

  // Check if key already exists
  if (current && atomic_load(&current->key) == key &&
      !atomic_load(&current->deleted))
  {
    return false;
  }

  // Create new node
  int level = random_level();
  skip_node_t *new_node = malloc(sizeof(skip_node_t));
  if (!new_node)
    return false;

  atomic_store(&new_node->key, key);
  atomic_store(&new_node->value, value);
  atomic_store(&new_node->level, level);
  atomic_store(&new_node->deleted, false);

  // Any levels above the ones we traversed default to the header. This also
  // covers the case where another thread raised list->max_level after we read it
  // into list_level: without this, update[list_level .. level-1] would be
  // uninitialized.
  if (level > list_level)
  {
    for (int i = list_level; i < level; i++)
    {
      update[i] = list->header;
    }

    // Publish the new max level, but only ever raise it (never lower a value a
    // concurrent inserter may have set higher).
    int cur_max = atomic_load(&list->max_level);
    while (cur_max < level &&
           !atomic_compare_exchange_weak(&list->max_level, &cur_max, level))
    {
      // cur_max is refreshed by the failed CAS; loop until it is >= level.
    }
  }

  // Link new node level by level. Once new_node is linked at ANY level it is
  // reachable by other threads and must never be freed. If a CAS fails, the
  // predecessor's forward pointer changed, so re-locate the predecessor at that
  // level and retry the link (loop, not recursion, and no free()).
  for (int i = 0; i < level; i++)
  {
    while (true)
    {
      skip_node_t *next = atomic_load(&update[i]->forward[i]);
      atomic_store(&new_node->forward[i], next);

      if (atomic_compare_exchange_weak(&update[i]->forward[i], &next, new_node))
      {
        break;
      }

      // Re-find the predecessor at level i, descending from the header. new_node
      // is not yet linked at level i (only at levels < i), so the walk cannot
      // encounter it.
      skip_node_t *pred = list->header;
      for (int j = atomic_load(&list->max_level) - 1; j >= i; j--)
      {
        skip_node_t *n = atomic_load(&pred->forward[j]);
        while (n && atomic_load(&n->key) < key)
        {
          pred = n;
          n = atomic_load(&pred->forward[j]);
        }
      }
      update[i] = pred;
    }
  }

  atomic_fetch_add(&list->size, 1);
  return true;
}

// Search in skip list
bool lockfree_skiplist_search(lockfree_skiplist_t *list, long key, void **value)
{
  skip_node_t *current = list->header;

  for (int i = atomic_load(&list->max_level) - 1; i >= 0; i--)
  {
    while (true)
    {
      skip_node_t *next = atomic_load(&current->forward[i]);
      if (!next || atomic_load(&next->key) > key)
      {
        break;
      }
      if (atomic_load(&next->key) == key && !atomic_load(&next->deleted))
      {
        *value = atomic_load(&next->value);
        return true;
      }
      current = next;
    }
  }

  return false;
}
