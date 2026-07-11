#define _GNU_SOURCE

#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stddef.h>

#include "lru.h"

static size_t get_hash_code(lru_table_t *table, const char *source)
{
    if (source == NULL)
    {
        return 0;
    }
    size_t hash = 0;
    while (*source != '\0')
    {
        char c = *source++;
        int a = c - '0';
        hash = (hash * 10) + a;
    }
    return hash % table->capacity;
}

lru_node_t *create_node(char *key, char *value)
{
    lru_node_t *new_node = (lru_node_t *)malloc(sizeof(lru_node_t));
    if (!new_node)
    {
        return NULL;
    }

    new_node->key = key;
    new_node->value = value;
    new_node->next = NULL;
    new_node->prev = NULL;
    new_node->hash_next = NULL;
    return new_node;
}

lru_table_t *create_table(int capacity)
{
    lru_table_t *new_table = (lru_table_t *)malloc(sizeof(lru_table_t));
    if (!new_table)
    {
        return NULL;
    }

    new_table->capacity = capacity;
    new_table->array = (lru_node_t **)malloc(sizeof(lru_node_t) * capacity);
    for (int i = 0; i < capacity; i++)
    {
        new_table->array[i] = NULL;
    }

    return new_table;
}

lru_list_t *create_list(int capacity)
{
    lru_list_t *new_list = (lru_list_t *)malloc(sizeof(lru_list_t));
    if (!new_list)
    {
        return NULL;
    }

    new_list->size = 0;
    new_list->capacity = capacity;
    new_list->head = new_list->tail = NULL;
    return new_list;
}

lru_cache_t *create_cache(int size)
{
    lru_cache_t *new_cache = (lru_cache_t *)malloc(sizeof(lru_cache_t));
    if (!new_cache)
    {
        return NULL;
    }

    lru_table_t *table = create_table(size * 2);
    lru_list_t *list = create_list(size);
    new_cache->table = table;
    new_cache->list = list;
    return new_cache;
}

void put(lru_cache_t *cache, char *key, char *value)
{
    lru_node_t *val_node = create_node(key, value);
    assert(val_node);

    if (add_to_hash(cache->table, val_node)) // already in list
    {
        move_to_front(cache, val_node->key);
    }
    else
    {
        add_to_list(cache, val_node);
    }
}

int add_to_hash(lru_table_t *table, lru_node_t *val_node)
{
    size_t hash_code = get_hash_code(table, val_node->key);

    // check if is in hash
    if (table->array[hash_code] != NULL)
    {
        lru_node_t *curr = table->array[hash_code];

        while (curr->hash_next != NULL)
        {
            if (strcmp(curr->key, val_node->key) == 0)
            {
                curr->value = val_node->value;
                return 1;
            }
            curr = curr->hash_next;
        }
        // last node
        if (strcmp(curr->key, val_node->key) == 0)
        {
            curr->value = val_node->value;
            return 1;
        }

        // add after last node
        curr->hash_next = val_node;
        return 0;
    }
    else
    {
        table->array[hash_code] = val_node;
        return 0;
    }
}

void move_to_front(lru_cache_t *cache, char *key)
{
    lru_table_t *table = cache->table;
    lru_list_t *list = cache->list;

    // return if only element in list
    if (list->size == 1)
    {
        return;
    }

    size_t hash_code = get_hash_code(table, key);
    lru_node_t *curr = table->array[hash_code];

    // find in hash map
    while (curr)
    {
        if (strcmp(curr->key, key) == 0)
        {
            break;
        }
        curr = curr->hash_next;
    }

    // Return if doesn't exist
    if (!curr)
    {
        return;
    }

    // move curr to latest/tail of list
    if (curr->prev == NULL)
    { // if head in list
        curr->prev = list->tail;
        list->head = curr->next;
        list->head->prev = NULL;
        list->tail->next = curr;
        list->tail = curr;
        list->tail->next = NULL;
        return;
    }

    if (curr->next == NULL) // already latest at tail
    {
        return;
    }

    // if curr in middle
    curr->next->prev = curr->prev;
    curr->prev->next = curr->next;
    curr->next = NULL;
    list->tail->next = curr;
    curr->prev = list->tail;
    list->tail = curr;
}

void add_to_list(lru_cache_t *cache, lru_node_t *val_node)
{
    lru_list_t *list = cache->list;
    if (list->size == list->capacity)
    {
        evict_cache(cache);
    }

    if (!list->head)
    {
        list->head = list->tail = val_node;
        list->size = 1;
        return;
    }

    val_node->prev = list->tail;
    list->tail->next = val_node;
    list->tail = val_node;
    list->size = list->size + 1;
}

void evict_cache(lru_cache_t *cache)
{
    lru_table_t *table = cache->table;
    lru_list_t *list = cache->list;

    lru_node_t *entry = list->head;
    // return if empty
    if (list->head == NULL)
    {
        return;
    }
    // remove head and tail if only one node
    if (list->head == list->tail)
    {
        list->head = NULL;
        list->tail = NULL;
    }
    else
    {
        // remove entry from list with multiple nodes
        list->head = entry->next;
        list->size = list->size - 1;
        list->head->prev = NULL;
    }

    // remove from map
    size_t hash_code = get_hash_code(table, entry->key);
    lru_node_t **indirect = &table->array[hash_code];
    while ((*indirect) != entry)
    {
        indirect = &(*indirect)->next;
    }
    *indirect = entry->next;

    free(entry);
}
