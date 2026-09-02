// Page-level B-tree operations: binary search over the sorted pair array and
// insert/delete of variable-length kv pairs within a single page. All lengths
// coming off disk are treated as untrusted, so every traversal validates that
// a pair stays inside the page before dereferencing it. Nothing here touches
// the engine singleton -- these functions operate on one page in isolation.

#include <stdint.h>
#include <string.h>

#include "storage_internal.h"

// Separator convention used by every page-level routine here and by the
// descent in db.c: entry i's key is the *highest* key reachable through child
// i, and header.next_page_id on an internal node is the rightmost child,
// holding everything above the last separator. find_key_position() returns the
// first entry whose key is >= the search key, so a lookup follows entry i when
// one exists and the rightmost pointer otherwise -- which is exactly this
// invariant read back. (On a leaf, next_page_id is instead the sibling link
// that db_scan walks.)

size_t btree_page_capacity(void)
{
  return PAGE_SIZE - sizeof(page_header_t);
}

size_t btree_used_bytes(btree_page_t *page)
{
  const size_t capacity = btree_page_capacity();
  if (page->header.free_space > capacity)
  {
    return SIZE_MAX; // corrupt free_space
  }
  return capacity - page->header.free_space;
}

// Byte offset of entry `index` within page->data, walking the pairs linearly
// and validating each one against `used`. Returns SIZE_MAX on a corrupt page.
// index == key_count is legal and yields `used` (one past the last entry).
static size_t entry_offset(btree_page_t *page, int index, size_t used)
{
  size_t offset = 0;
  for (int i = 0; i < index; i++)
  {
    if (offset + sizeof(kv_pair_t) > used)
    {
      return SIZE_MAX;
    }
    kv_pair_t *kv = (kv_pair_t *)(page->data + offset);
    size_t pair_size = sizeof(kv_pair_t) + kv->key_length + kv->value_length;
    if (pair_size > used - offset)
    {
      return SIZE_MAX;
    }
    offset += pair_size;
  }
  return offset;
}

// Pick the entry to split at: the first one whose start crosses the halfway
// point of the used bytes. Splitting by bytes rather than by entry count keeps
// both halves comfortably under capacity even when values vary in size, which
// matters because a single pair can be a third of a page.
static int choose_split_index(btree_page_t *page, size_t used)
{
  const size_t half = used / 2;
  size_t offset = 0;
  for (int i = 0; i < page->header.key_count; i++)
  {
    if (offset >= half && i > 0)
    {
      return i;
    }
    if (offset + sizeof(kv_pair_t) > used)
    {
      return -1;
    }
    kv_pair_t *kv = (kv_pair_t *)(page->data + offset);
    size_t pair_size = sizeof(kv_pair_t) + kv->key_length + kv->value_length;
    if (pair_size > used - offset)
    {
      return -1;
    }
    offset += pair_size;
  }
  // Everything sat below the halfway mark (one very large trailing pair):
  // fall back to peeling off the last entry.
  return page->header.key_count > 1 ? page->header.key_count - 1 : -1;
}

int btree_split_page(btree_page_t *src, btree_page_t *dst, uint32_t dst_page_id,
                     char *sep_out, size_t sep_out_size, size_t *sep_len_out)
{
  const size_t capacity = btree_page_capacity();
  const bool is_leaf = (src->header.page_type == PAGE_TYPE_LEAF);
  size_t used = btree_used_bytes(src);
  if (used == SIZE_MAX)
  {
    return -1; // corrupt page
  }

  // A leaf must keep at least one entry on each side. An internal node hands
  // its middle separator to the parent, so it needs one entry just to have a
  // separator to give up -- the halves may legitimately end up empty of keys
  // while still carrying a child pointer each.
  if (src->header.key_count < (is_leaf ? 2 : 1))
  {
    return -1;
  }

  int mid = choose_split_index(src, used);
  if (mid < 0 || mid >= src->header.key_count)
  {
    return -1;
  }

  // The separator is the highest key that stays on the left. For a leaf that
  // is entry mid-1 (which src keeps); for an internal node it is entry mid,
  // which moves up into the parent and is dropped from both halves.
  int sep_index = is_leaf ? mid - 1 : mid;
  size_t sep_offset = entry_offset(src, sep_index, used);
  if (sep_offset == SIZE_MAX || sep_offset + sizeof(kv_pair_t) > used)
  {
    return -1;
  }
  kv_pair_t *sep_kv = (kv_pair_t *)(src->data + sep_offset);
  if (sizeof(kv_pair_t) + sep_kv->key_length > used - sep_offset ||
      sep_kv->key_length > sep_out_size)
  {
    return -1;
  }
  memcpy(sep_out, sep_kv->data, sep_kv->key_length);
  *sep_len_out = sep_kv->key_length;

  // Byte range that moves to dst, and the count of entries in it.
  size_t split_offset = entry_offset(src, mid, used);
  if (split_offset == SIZE_MAX || split_offset > used)
  {
    return -1;
  }
  size_t right_offset = split_offset;
  int right_count = src->header.key_count - mid;
  uint32_t promoted_child = 0;

  if (!is_leaf)
  {
    // Entry mid is consumed as the separator: src inherits its child pointer
    // as its new rightmost child, and dst starts after it.
    kv_pair_t *mid_kv = (kv_pair_t *)(src->data + split_offset);
    if (sizeof(kv_pair_t) + mid_kv->key_length + mid_kv->value_length >
        used - split_offset)
    {
      return -1;
    }
    promoted_child = mid_kv->child_page_id;
    right_offset = split_offset + sizeof(kv_pair_t) + mid_kv->key_length +
                   mid_kv->value_length;
    right_count = src->header.key_count - mid - 1;
  }

  if (right_offset > used || right_count < 0)
  {
    return -1;
  }
  size_t right_bytes = used - right_offset;
  if (right_bytes > capacity)
  {
    return -1;
  }

  // Carve. dst is a freshly allocated page, so its data area is zeroed and we
  // only ever write the range we are about to account for.
  memcpy(dst->data, src->data + right_offset, right_bytes);
  dst->header.page_id = dst_page_id;
  dst->header.page_type = src->header.page_type;
  dst->header.key_count = (uint16_t)right_count;
  dst->header.free_space = (uint16_t)(capacity - right_bytes);

  if (is_leaf)
  {
    // dst takes src's place in the sibling chain that db_scan walks.
    dst->header.next_page_id = src->header.next_page_id;
    src->header.next_page_id = dst_page_id;
  }
  else
  {
    dst->header.next_page_id = src->header.next_page_id;
    src->header.next_page_id = promoted_child;
  }

  src->header.key_count = (uint16_t)mid;
  src->header.free_space = (uint16_t)(capacity - split_offset);
  return 0;
}

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

  if (move_size > 0)
  {
    memmove(insert_ptr + pair_size, insert_ptr, move_size);
  }

  kv_pair_t *new_kv = (kv_pair_t *)insert_ptr;
  new_kv->key_length = key_length;
  new_kv->value_length = value_length;
  new_kv->child_page_id = child_page_id;

  memcpy(new_kv->data, key, key_length);
  // Internal separators carry a child pointer and no value, so value may be
  // NULL here; memcpy with a NULL source is undefined even for a zero length.
  if (value_length > 0)
  {
    memcpy(new_kv->data + key_length, value, value_length);
  }

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

  char *delete_ptr = (char *)kv;
  char *next_ptr = delete_ptr + pair_size;
  char *end_ptr = page->data + used;
  if (next_ptr > end_ptr)
  {
    return -1; // Corrupt page
  }
  size_t move_size = end_ptr - next_ptr;

  if (move_size > 0)
  {
    memmove(delete_ptr, next_ptr, move_size);
  }

  page->header.key_count--;
  page->header.free_space += pair_size;

  return 0;
}
