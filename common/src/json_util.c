#include <stdio.h>
#include <string.h>

#include "json_util.h"

int json_escape(char *dst, size_t dst_size, const char *src, size_t src_len)
{
  size_t out = 0;
  for (size_t i = 0; i < src_len; i++)
  {
    unsigned char c = (unsigned char)src[i];
    char unicode[8];
    const char *rep = NULL;

    switch (c)
    {
    case '"':
      rep = "\\\"";
      break;
    case '\\':
      rep = "\\\\";
      break;
    case '\n':
      rep = "\\n";
      break;
    case '\r':
      rep = "\\r";
      break;
    case '\t':
      rep = "\\t";
      break;
    default:
      if (c < 0x20)
      {
        snprintf(unicode, sizeof(unicode), "\\u%04x", c);
        rep = unicode;
      }
      break;
    }

    if (rep)
    {
      size_t len = strlen(rep);
      if (out + len >= dst_size)
      {
        return -1;
      }
      memcpy(dst + out, rep, len);
      out += len;
    }
    else
    {
      if (out + 1 >= dst_size)
      {
        return -1;
      }
      dst[out++] = (char)c;
    }
  }

  if (out >= dst_size)
  {
    return -1;
  }
  dst[out] = '\0';
  return (int)out;
}

static const char *skip_ws(const char *p, const char *end)
{
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
  {
    p++;
  }
  return p;
}

const char *json_find_field(const char *body, size_t len, const char *field)
{
  size_t field_len = strlen(field);
  const char *end = body + len;
  const char *p = body;

  // Walk the object one JSON string at a time so that "field" is only matched
  // when it appears as an object key (its closing quote is followed by ':'),
  // never when it appears inside a string value.
  while (p < end)
  {
    if (*p != '"')
    {
      p++;
      continue;
    }

    // p is at a string's opening quote; scan to its unescaped closing quote.
    const char *str = p + 1;
    const char *q = str;
    while (q < end && *q != '"')
    {
      if (*q == '\\' && q + 1 < end)
      {
        q++; // skip the escaped character
      }
      q++;
    }
    if (q >= end)
    {
      return NULL; // unterminated string
    }

    // A key is a string immediately followed (after whitespace) by ':'.
    const char *after = skip_ws(q + 1, end);
    if (after < end && *after == ':')
    {
      if ((size_t)(q - str) == field_len && memcmp(str, field, field_len) == 0)
      {
        return skip_ws(after + 1, end);
      }
    }

    // Not our key: advance past this string; a following value string is thus
    // skipped rather than being mistaken for a key.
    p = q + 1;
  }
  return NULL;
}

bool json_get_string(const char *body, size_t len, const char *field,
                     char *out, size_t out_size)
{
  const char *p = json_find_field(body, len, field);
  const char *end = body + len;
  if (!p || p >= end || *p != '"')
  {
    return false;
  }
  p++;

  size_t out_len = 0;
  while (p < end && *p != '"')
  {
    char c = *p;
    if (c == '\\' && p + 1 < end)
    {
      p++;
      switch (*p)
      {
      case 'n':
        c = '\n';
        break;
      case 'r':
        c = '\r';
        break;
      case 't':
        c = '\t';
        break;
      case 'b':
        c = '\b';
        break;
      case 'f':
        c = '\f';
        break;
      case '/':
        c = '/';
        break;
      case '"':
        c = '"';
        break;
      case '\\':
        c = '\\';
        break;
      case 'u':
        // Non-ASCII escapes are collapsed to '?'; skip the 4 hex digits.
        if (p + 4 < end)
        {
          p += 4;
        }
        c = '?';
        break;
      default:
        c = *p;
        break;
      }
    }

    if (out_len + 1 >= out_size)
    {
      return false; // value too long
    }
    out[out_len++] = c;
    p++;
  }

  if (p >= end || *p != '"')
  {
    return false; // unterminated string
  }
  out[out_len] = '\0';
  return true;
}

bool json_get_bool(const char *body, size_t len, const char *field, bool *out)
{
  const char *p = json_find_field(body, len, field);
  const char *end = body + len;
  if (!p)
  {
    return false;
  }
  if (p + 4 <= end && memcmp(p, "true", 4) == 0)
  {
    *out = true;
    return true;
  }
  if (p + 5 <= end && memcmp(p, "false", 5) == 0)
  {
    *out = false;
    return true;
  }
  return false;
}

bool json_get_uint64(const char *body, size_t len, const char *field,
                     uint64_t *out)
{
  const char *p = json_find_field(body, len, field);
  const char *end = body + len;
  if (!p || p >= end || *p < '0' || *p > '9')
  {
    return false;
  }

  uint64_t value = 0;
  while (p < end && *p >= '0' && *p <= '9')
  {
    value = value * 10 + (uint64_t)(*p - '0');
    p++;
  }
  *out = value;
  return true;
}
