#define _GNU_SOURCE

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "lru.h"

// FNV-1a. The bucket index has to be derived from the whole key: an earlier
// version folded each byte as if it were a decimal digit ((hash * 10) + c-'0'),
// which piled every non-numeric key onto a handful of buckets.
static size_t get_hash_code(const lru_table_t *table, const char *source)
{
    uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)source; *p != '\0'; p++)
    {
        hash ^= (uint64_t)*p;
        hash *= 1099511628211ULL;
    }
    return (size_t)(hash % (uint64_t)table->capacity);
}

static lru_node_t *create_node(const char *key, const char *value, size_t value_length)
{
    lru_node_t *node = (lru_node_t *)calloc(1, sizeof(lru_node_t));
    if (!node)
    {
        return NULL;
    }

    node->key = strdup(key);
    node->value = (char *)malloc(value_length + 1);
    if (!node->key || !node->value)
    {
        free(node->key);
        free(node->value);
        free(node);
        return NULL;
    }

    memcpy(node->value, value, value_length);
    node->value[value_length] = '\0';
    node->value_length = value_length;
    return node;
}

static void free_node(lru_node_t *node)
{
    if (!node)
    {
        return;
    }
    free(node->key);
    free(node->value);
    free(node);
}

static lru_node_t *hash_find(lru_table_t *table, const char *key)
{
    for (lru_node_t *curr = table->array[get_hash_code(table, key)]; curr; curr = curr->hash_next)
    {
        if (strcmp(curr->key, key) == 0)
        {
            return curr;
        }
    }
    return NULL;
}

static void hash_insert(lru_table_t *table, lru_node_t *node)
{
    size_t bucket = get_hash_code(table, node->key);
    node->hash_next = table->array[bucket];
    table->array[bucket] = node;
}

static void hash_remove(lru_table_t *table, lru_node_t *node)
{
    // Walk the bucket chain via hash_next. Following ->next here would wander
    // off into the recency list and splice a foreign node into the bucket.
    lru_node_t **indirect = &table->array[get_hash_code(table, node->key)];
    while (*indirect && *indirect != node)
    {
        indirect = &(*indirect)->hash_next;
    }
    if (*indirect)
    {
        *indirect = node->hash_next;
    }
    node->hash_next = NULL;
}

static void list_unlink(lru_list_t *list, lru_node_t *node)
{
    if (node->prev)
    {
        node->prev->next = node->next;
    }
    else
    {
        list->head = node->next;
    }

    if (node->next)
    {
        node->next->prev = node->prev;
    }
    else
    {
        list->tail = node->prev;
    }

    node->prev = node->next = NULL;
    list->size--;
}

// Append at the tail: the entry becomes the most recently used.
static void list_push_back(lru_list_t *list, lru_node_t *node)
{
    node->prev = list->tail;
    node->next = NULL;

    if (list->tail)
    {
        list->tail->next = node;
    }
    else
    {
        list->head = node;
    }

    list->tail = node;
    list->size++;
}

static void touch(lru_cache_t *cache, lru_node_t *node)
{
    list_unlink(cache->list, node);
    list_push_back(cache->list, node);
}

static void evict_lru(lru_cache_t *cache)
{
    lru_node_t *victim = cache->list->head;
    if (!victim)
    {
        return;
    }

    list_unlink(cache->list, victim);
    hash_remove(cache->table, victim);
    free_node(victim);
    atomic_fetch_add(&cache->evictions, 1);
}

static lru_table_t *create_table(int capacity)
{
    lru_table_t *table = (lru_table_t *)malloc(sizeof(lru_table_t));
    if (!table)
    {
        return NULL;
    }

    table->capacity = capacity;
    table->array = (lru_node_t **)calloc((size_t)capacity, sizeof(lru_node_t *));
    if (!table->array)
    {
        free(table);
        return NULL;
    }
    return table;
}

static lru_list_t *create_list(int capacity)
{
    lru_list_t *list = (lru_list_t *)malloc(sizeof(lru_list_t));
    if (!list)
    {
        return NULL;
    }

    list->size = 0;
    list->capacity = capacity;
    list->head = list->tail = NULL;
    return list;
}

lru_cache_t *lru_cache_create(int capacity)
{
    if (capacity <= 0)
    {
        return NULL;
    }

    lru_cache_t *cache = (lru_cache_t *)calloc(1, sizeof(lru_cache_t));
    if (!cache)
    {
        return NULL;
    }

    // Twice the entry count keeps the bucket chains short.
    cache->table = create_table(capacity * 2);
    cache->list = create_list(capacity);
    if (!cache->table || !cache->list || pthread_mutex_init(&cache->lock, NULL) != 0)
    {
        if (cache->table)
        {
            free(cache->table->array);
            free(cache->table);
        }
        free(cache->list);
        free(cache);
        return NULL;
    }

    atomic_init(&cache->generation, 0);
    atomic_init(&cache->hits, 0);
    atomic_init(&cache->misses, 0);
    atomic_init(&cache->evictions, 0);
    return cache;
}

void lru_cache_destroy(lru_cache_t *cache)
{
    if (!cache)
    {
        return;
    }

    lru_node_t *curr = cache->list->head;
    while (curr)
    {
        lru_node_t *next = curr->next;
        free_node(curr);
        curr = next;
    }

    pthread_mutex_destroy(&cache->lock);
    free(cache->table->array);
    free(cache->table);
    free(cache->list);
    free(cache);
}

bool lru_get(lru_cache_t *cache, const char *key, char *value, size_t *value_length)
{
    if (!cache || !key || !value || !value_length)
    {
        return false;
    }

    pthread_mutex_lock(&cache->lock);

    lru_node_t *node = hash_find(cache->table, key);
    if (!node)
    {
        pthread_mutex_unlock(&cache->lock);
        atomic_fetch_add(&cache->misses, 1);
        return false;
    }

    touch(cache, node);

    size_t copied = node->value_length < *value_length ? node->value_length : *value_length;
    memcpy(value, node->value, copied);
    *value_length = node->value_length; // full length, even when truncated

    pthread_mutex_unlock(&cache->lock);
    atomic_fetch_add(&cache->hits, 1);
    return true;
}

// Insert or replace. Caller holds cache->lock.
static int store(lru_cache_t *cache, const char *key, const char *value, size_t value_length)
{
    lru_node_t *existing = hash_find(cache->table, key);
    if (existing)
    {
        char *copy = (char *)malloc(value_length + 1);
        if (!copy)
        {
            return -1;
        }
        memcpy(copy, value, value_length);
        copy[value_length] = '\0';

        free(existing->value);
        existing->value = copy;
        existing->value_length = value_length;
        touch(cache, existing);
        return 0;
    }

    lru_node_t *node = create_node(key, value, value_length);
    if (!node)
    {
        return -1;
    }

    if (cache->list->size >= cache->list->capacity)
    {
        evict_lru(cache);
    }

    hash_insert(cache->table, node);
    list_push_back(cache->list, node);
    return 0;
}

int lru_put(lru_cache_t *cache, const char *key, const char *value, size_t value_length)
{
    if (!cache || !key || !value)
    {
        return -1;
    }

    pthread_mutex_lock(&cache->lock);
    // Bump first: a reader that has already gone to storage must lose to this
    // write, whichever of the two reaches the cache first.
    atomic_fetch_add(&cache->generation, 1);
    int rc = store(cache, key, value, value_length);
    pthread_mutex_unlock(&cache->lock);
    return rc;
}

int lru_fill(lru_cache_t *cache, const char *key, const char *value,
             size_t value_length, uint64_t generation)
{
    if (!cache || !key || !value)
    {
        return -1;
    }

    pthread_mutex_lock(&cache->lock);
    if (atomic_load(&cache->generation) != generation)
    {
        pthread_mutex_unlock(&cache->lock);
        return 1; // a write landed while we were reading storage; drop the fill
    }
    int rc = store(cache, key, value, value_length);
    pthread_mutex_unlock(&cache->lock);
    return rc;
}

bool lru_invalidate(lru_cache_t *cache, const char *key)
{
    if (!cache || !key)
    {
        return false;
    }

    pthread_mutex_lock(&cache->lock);
    atomic_fetch_add(&cache->generation, 1);

    lru_node_t *node = hash_find(cache->table, key);
    if (node)
    {
        list_unlink(cache->list, node);
        hash_remove(cache->table, node);
    }
    pthread_mutex_unlock(&cache->lock);

    // Unlinked from both structures, so no other thread can still reach it.
    free_node(node);
    return node != NULL;
}

uint64_t lru_generation(lru_cache_t *cache)
{
    return cache ? atomic_load(&cache->generation) : 0;
}

void lru_stats(lru_cache_t *cache, uint64_t *hits, uint64_t *misses,
               uint64_t *evictions, uint64_t *size)
{
    if (!cache)
    {
        return;
    }

    if (hits)
    {
        *hits = atomic_load(&cache->hits);
    }
    if (misses)
    {
        *misses = atomic_load(&cache->misses);
    }
    if (evictions)
    {
        *evictions = atomic_load(&cache->evictions);
    }
    if (size)
    {
        pthread_mutex_lock(&cache->lock);
        *size = (uint64_t)cache->list->size;
        pthread_mutex_unlock(&cache->lock);
    }
}
