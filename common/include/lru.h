#ifndef LRU_CACHE_H
#define LRU_CACHE_H

typedef struct lru_node
{
    char *key;
    char *value;
    struct lru_node *prev;
    struct lru_node *next;
    struct lru_node *hash_next;
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
    lru_node_t *head;
    lru_node_t *tail;
} lru_list_t;

typedef struct
{
    lru_table_t *table;
    lru_list_t *list;
} lru_cache_t;

lru_node_t *create_node(char *key, char *value);
lru_table_t *create_table(int capacity);
lru_list_t *create_list(int capacity);
lru_cache_t *create_cache(int size);

void put(lru_cache_t *cache, char *key, char *value);
int add_to_hash(lru_table_t *table, lru_node_t *val_node);
void move_to_front(lru_cache_t *cache, char *key);
void add_to_list(lru_cache_t *cache, lru_node_t *val_node);
void evict_cache(lru_cache_t *cache);

#endif