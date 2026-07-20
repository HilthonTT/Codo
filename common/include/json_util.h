#ifndef JSON_UTIL_H
#define JSON_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Minimal helpers for the flat, single-level JSON objects this project stores
// and accepts ({"id":1,"title":"...","completed":false}). Not a general JSON
// parser: nested objects/arrays are not traversed, and unicode escapes are
// collapsed to '?'.

// Escape src into dst as a JSON string body (without surrounding quotes).
// Returns the written length, or -1 if dst is too small.
int json_escape(char *dst, size_t dst_size, const char *src, size_t src_len);

// Locate the value that follows "field": in a JSON object body. Returns a
// pointer to the first non-whitespace value character, or NULL if not found.
const char *json_find_field(const char *body, size_t len, const char *field);

// Extract a string field, unescaping the common JSON escapes. Returns false if
// the field is missing, not a string, or does not fit in out.
bool json_get_string(const char *body, size_t len, const char *field,
                     char *out, size_t out_size);

// Extract a boolean field. Returns false if absent; *out is set on success.
bool json_get_bool(const char *body, size_t len, const char *field, bool *out);

// Extract a non-negative integer field. Returns false if the field is missing
// or does not start with a digit; *out is set on success.
bool json_get_uint64(const char *body, size_t len, const char *field,
                     uint64_t *out);

#endif
