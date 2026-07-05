#define _GNU_SOURCE

#include <stdlib.h>
#include <limits.h>

#include "lockfree.h"

static hazard_pointer_record_t hazard_pointer_table[MAX_THREADS];

// Thread-local hazard pointer record
static __thread hazard_pointer_record_t *local_hazard_record = NULL;

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
    return NULL;
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

          // Free old head node (with hazard pointer protection)
          if (!is_hazard_pointer(head))
          {
            free(head);
          }

          clear_hazard_pointer(0);
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

  while (current)
  {
    // Verify node is still valid
    if (current != atomic_load(&table->buckets[bucket]))
    {
      current = atomic_load(&table->buckets[bucket]);
      set_hazard_pointer(0, current);
      continue;
    }

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
  int level = 1;
  while ((rand() & 0x1) && level < MAX_LEVEL)
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

  // Find position to insert
  for (int i = atomic_load(&list->max_level) - 1; i >= 0; i--)
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

  // Update max level if necessary
  if (level > atomic_load(&list->max_level))
  {
    for (int i = atomic_load(&list->max_level); i < level; i++)
    {
      update[i] = list->header;
    }
    atomic_store(&list->max_level, level);
  }

  // Link new node
  for (int i = 0; i < level; i++)
  {
    skip_node_t *next = atomic_load(&update[i]->forward[i]);
    atomic_store(&new_node->forward[i], next);

    if (!atomic_compare_exchange_weak(&update[i]->forward[i], &next, new_node))
    {
      // Retry on failure
      free(new_node);
      return lockfree_skiplist_insert(list, key, value);
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
