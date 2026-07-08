#define _GNU_SOURCE

// Page-level B-tree operations: binary search over the sorted pair array and
// insert/delete of variable-length kv pairs within a single page. All lengths
// coming off disk are treated as untrusted, so every traversal validates that
// a pair stays inside the page before dereferencing it. Nothing here touches
// the engine singleton -- these functions operate on one page in isolation.

#include <string.h>

#include "storage_internal.h"

int compare_keys(const char *key1, size_t len1, const char *key2, size_t len2)
{
  size_t min_len = len1 < len2 ? len1 : len2;
  int result = memcmp(key1, key2, min_len);

  if (result == 0)
  {
    if (len1 < len2)
      return -1;
    if (len1 > len2)
      return 1;
    return 0;
  }

  return result;
}

kv_pair_t *get_kv_pair(btree_page_t *page, int index)
{
  if (index < 0 || index >= page->header.key_count)
  {
    return NULL;
  }

  // Find the key-value pair at the given index. The on-disk key/value lengths
  // are untrusted, so validate that every pair (header + key + value) stays
  // within the page's data area before dereferencing it -- a corrupt page must
  // not cause an out-of-bounds read.
  const size_t data_capacity = PAGE_SIZE - sizeof(page_header_t);
  char *data_ptr = page->data;
  size_t offset = 0;

  for (int i = 0; i <= index; i++)
  {
    // Need at least the fixed-size header to read the lengths.
    if (offset + sizeof(kv_pair_t) > data_capacity)
    {
      return NULL; // Corrupt page
    }

    kv_pair_t *kv = (kv_pair_t *)data_ptr;

    if (i == index)
    {
      // Also verify this pair's payload fits before handing it back.
      size_t pair_size = sizeof(kv_pair_t) + kv->key_length + kv->value_length;
      if (offset + pair_size > data_capacity)
      {
        return NULL; // Corrupt page
      }
      return kv;
    }

    size_t pair_size = sizeof(kv_pair_t) + kv->key_length + kv->value_length;
    if (pair_size > data_capacity - offset)
    {
      return NULL; // Corrupt page
    }

    data_ptr += pair_size;
    offset += pair_size;
  }

  return NULL;
}

int find_key_position(btree_page_t *page, const char *key, size_t key_length)
{
  int left = 0;
  int right = page->header.key_count - 1;

  while (left <= right)
  {
    int mid = (left + right) / 2;
    kv_pair_t *kv = get_kv_pair(page, mid);

    if (!kv)
    {
      break;
    }

    const char *kv_key = kv->data;
    int cmp = compare_keys(key, key_length, kv_key, kv->key_length);

    if (cmp == 0)
    {
      return mid; // Exact match
    }
    else if (cmp < 0)
    {
      right = mid - 1;
    }
    else
    {
      left = mid + 1;
    }
  }

  return left;
}

int insert_kv_pair(
    btree_page_t *page,
    int position,
    const char *key,
    size_t key_length,
    const char *value,
    size_t value_length,
    uint32_t child_page_id)
{
  size_t pair_size = sizeof(kv_pair_t) + key_length + value_length;

  if (page->header.free_space < pair_size)
  {
    return -1; // Not enough size
  }

  // Guard against a corrupt free_space that would place end_ptr out of bounds.
  const size_t data_capacity = PAGE_SIZE - sizeof(page_header_t);
  if (page->header.free_space > data_capacity)
  {
    return -1; // Corrupt page
  }
  size_t used = data_capacity - page->header.free_space;

  // Find insertion point, validating each traversed pair stays within the
  // used region (untrusted on-disk lengths).
  char *insert_ptr = page->data;
  size_t insert_offset = 0;
  for (int i = 0; i < position; i++)
  {
    if (insert_offset + sizeof(kv_pair_t) > used)
    {
      return -1; // Corrupt page
    }
    kv_pair_t *kv = (kv_pair_t *)insert_ptr;
    size_t sz = sizeof(kv_pair_t) + kv->key_length + kv->value_length;
    if (sz > used - insert_offset)
    {
      return -1; // Corrupt page
    }
    insert_ptr += sz;
    insert_offset += sz;
  }

  // Calculate space needed to move existing data. end_ptr is the end of the
  // currently-used region (capacity minus free space), so we shift only the
  // pairs that sit after the insertion point.
  char *end_ptr = page->data + used;
  size_t move_size = end_ptr - insert_ptr;

  // Move existing data to make room
  if (move_size > 0)
  {
    memmove(insert_ptr + pair_size, insert_ptr, move_size);
  }

  // Write the new key-value pair into the gap
  kv_pair_t *new_kv = (kv_pair_t *)insert_ptr;
  new_kv->key_length = key_length;
  new_kv->value_length = value_length;
  new_kv->child_page_id = child_page_id;

  memcpy(new_kv->data, key, key_length);
  memcpy(new_kv->data + key_length, value, value_length);

  page->header.key_count++;
  page->header.free_space -= pair_size;

  return 0;
}

int delete_kv_pair(btree_page_t *page, int position)
{
  if (position < 0 || position >= page->header.key_count)
  {
    return -1;
  }

  kv_pair_t *kv = get_kv_pair(page, position);
  if (!kv)
  {
    return -1;
  }

  size_t pair_size = sizeof(kv_pair_t) + kv->key_length + kv->value_length;

  // Guard against a corrupt free_space that would make end_ptr precede
  // next_ptr, which would underflow move_size into a huge memmove.
  const size_t data_capacity = PAGE_SIZE - sizeof(page_header_t);
  if (page->header.free_space > data_capacity)
  {
    return -1; // Corrupt page
  }
  size_t used = data_capacity - page->header.free_space;

  // Calculate data to move
  char *delete_ptr = (char *)kv;
  char *next_ptr = delete_ptr + pair_size;
  char *end_ptr = page->data + used;
  if (next_ptr > end_ptr)
  {
    return -1; // Corrupt page
  }
  size_t move_size = end_ptr - next_ptr;

  // Move data to close the gap
  if (move_size > 0)
  {
    memmove(delete_ptr, next_ptr, move_size);
  }

  page->header.key_count--;
  page->header.free_space += pair_size;

  return 0;
}
