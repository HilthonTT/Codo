#define _GNU_SOURCE

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "config.h"
#include "http_types.h"
#include "util.h"

const char *http_status_to_string(http_status_t status)
{
  switch (status)
  {
  case HTTP_SWITCHING_PROTOCOLS:
    return "Switching Protocols";
  case HTTP_OK:
    return "OK";
  case HTTP_CREATED:
    return "Created";
  case HTTP_ACCEPTED:
    return "Accepted";
  case HTTP_NO_CONTENT:
    return "No Content";
  case HTTP_PARTIAL_CONTENT:
    return "Partial Content";
  case HTTP_MOVED_PERMANENTLY:
    return "Moved Permanently";
  case HTTP_FOUND:
    return "Found";
  case HTTP_NOT_MODIFIED:
    return "Not Modified";
  case HTTP_BAD_REQUEST:
    return "Bad Request";
  case HTTP_UNAUTHORIZED:
    return "Unauthorized";
  case HTTP_FORBIDDEN:
    return "Forbidden";
  case HTTP_NOT_FOUND:
    return "Not Found";
  case HTTP_METHOD_NOT_ALLOWED:
    return "Method Not Allowed";
  case HTTP_REQUEST_TIMEOUT:
    return "Request Timeout";
  case HTTP_PAYLOAD_TOO_LARGE:
    return "Payload Too Large";
  case HTTP_RANGE_NOT_SATISFIABLE:
    return "Range Not Satisfiable";
  case HTTP_TOO_MANY_REQUESTS:
    return "Too Many Requests";
  case HTTP_INTERNAL_SERVER_ERROR:
    return "Internal Server Error";
  case HTTP_NOT_IMPLEMENTED:
    return "Not Implemented";
  case HTTP_BAD_GATEWAY:
    return "Bad Gateway";
  case HTTP_SERVICE_UNAVAILABLE:
    return "Service Unavailable";
  case HTTP_GATEWAY_TIMEOUT:
    return "Gateway Timeout";
  default:
    return "Unknown";
  }
}

http_method_t string_to_http_method(const char *method)
{
  if (strcasecmp(method, "GET") == 0)
    return HTTP_GET;
  if (strcasecmp(method, "POST") == 0)
    return HTTP_POST;
  if (strcasecmp(method, "PUT") == 0)
    return HTTP_PUT;
  if (strcasecmp(method, "DELETE") == 0)
    return HTTP_DELETE;
  if (strcasecmp(method, "HEAD") == 0)
    return HTTP_HEAD;
  if (strcasecmp(method, "OPTIONS") == 0)
    return HTTP_OPTIONS;
  if (strcasecmp(method, "PATCH") == 0)
    return HTTP_PATCH;
  if (strcasecmp(method, "CONNECT") == 0)
    return HTTP_CONNECT;
  if (strcasecmp(method, "TRACE") == 0)
    return HTTP_TRACE;
  return HTTP_UNKNOWN;
}

const char *http_method_to_string(http_method_t method)
{
  switch (method)
  {
  case HTTP_GET:
    return "GET";
  case HTTP_POST:
    return "POST";
  case HTTP_PUT:
    return "PUT";
  case HTTP_DELETE:
    return "DELETE";
  case HTTP_HEAD:
    return "HEAD";
  case HTTP_OPTIONS:
    return "OPTIONS";
  case HTTP_PATCH:
    return "PATCH";
  case HTTP_CONNECT:
    return "CONNECT";
  case HTTP_TRACE:
    return "TRACE";
  default:
    return "UNKNOWN";
  }
}

bool is_valid_uri(const char *uri)
{
  if (!uri)
  {
    return false;
  }

  // URI must be absolute path
  if (*uri != '/')
  {
    return false;
  }

  // Check for directory traversal
  if (strstr(uri, "../") != NULL || strstr(uri, "..\\") != NULL)
  {
    return false;
  }

  // Also reject a trailing ".." segment with no slash after it: the whole URI is
  // ".." or it ends with "/.." (e.g. "/", "/foo/.."), which the "../" check above
  // would miss.
  size_t len = strlen(uri);
  if (len >= 2 && uri[len - 1] == '.' && uri[len - 2] == '.' &&
      (len == 2 || uri[len - 3] == '/'))
  {
    return false;
  }

  return true;
}

static int hex_to_int(char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

char *url_decode(const char *url)
{
  if (!url)
  {
    return NULL;
  }
  size_t len = strlen(url);
  char *decoded = malloc(len + 1);
  if (!decoded)
  {
    return NULL;
  }
  size_t j = 0;
  for (size_t i = 0; i < len; i++)
  {
    if (url[i] == '%' && i + 2 < len)
    {
      int hi = hex_to_int(url[i + 1]);
      int lo = hex_to_int(url[i + 2]);
      if (hi >= 0 && lo >= 0)
      {
        int c = (hi << 4) | lo;
        // Reject an encoded NUL (%00): decoding it would embed a NUL that
        // truncates the resulting path and lets input smuggle past later
        // string-based checks.
        if (c == 0)
        {
          free(decoded);
          return NULL;
        }
        decoded[j++] = (char)c;
        i += 2;
        continue;
      }
    }
    // '+' is copied literally. The '+' -> space convention only applies to
    // query-string values, not to paths, so decoding it here would be wrong.
    decoded[j++] = url[i];
  }
  decoded[j] = '\0';
  return decoded;
}

void parse_query_string(const char *query, http_header_t *params, int *params_count)
{
  if (!query || !params || !params_count)
  {
    return;
  }
  *params_count = 0;

  char buf[2048];
  strncpy(buf, query, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  char *saveptr = NULL;
  char *pair = strtok_r(buf, "&", &saveptr);
  while (pair && *params_count < MAX_HEADERS)
  {
    char *eq = strchr(pair, '=');
    if (eq)
    {
      *eq = '\0';
      strncpy(params[*params_count].name, pair, sizeof(params[*params_count].name) - 1);
      params[*params_count].name[sizeof(params[*params_count].name) - 1] = '\0';
      strncpy(params[*params_count].value, eq + 1, sizeof(params[*params_count].value) - 1);
      params[*params_count].value[sizeof(params[*params_count].value) - 1] = '\0';
      (*params_count)++;
    }
    pair = strtok_r(NULL, "&", &saveptr);
  }
}

void format_http_date(time_t t, char *buf, size_t buf_size)
{
  struct tm gmt;
  gmtime_r(&t, &gmt);
  strftime(buf, buf_size, "%a, %d %b %Y %H:%M:%S GMT", &gmt);
}

time_t parse_http_date(const char *value)
{
  if (!value)
  {
    return (time_t)-1;
  }
  struct tm tm;
  memset(&tm, 0, sizeof(tm));
  if (!strptime(value, "%a, %d %b %Y %H:%M:%S GMT", &tm))
  {
    return (time_t)-1;
  }
  return timegm(&tm);
}
