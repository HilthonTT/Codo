#ifndef HTTP_TYPES_H
#define HTTP_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <time.h>

#include "config.h"

// HTTP methods
typedef enum
{
  HTTP_GET,
  HTTP_POST,
  HTTP_PUT,
  HTTP_DELETE,
  HTTP_HEAD,
  HTTP_OPTIONS,
  HTTP_PATCH,
  HTTP_CONNECT,
  HTTP_TRACE,
  HTTP_UNKNOWN,
} http_method_t;

// HTTP status codes
typedef enum
{
  HTTP_SWITCHING_PROTOCOLS = 101,
  HTTP_OK = 200,
  HTTP_CREATED = 201,
  HTTP_ACCEPTED = 202,
  HTTP_NO_CONTENT = 204,
  HTTP_PARTIAL_CONTENT = 206,
  HTTP_MOVED_PERMANENTLY = 301,
  HTTP_FOUND = 302,
  HTTP_NOT_MODIFIED = 304,
  HTTP_BAD_REQUEST = 400,
  HTTP_UNAUTHORIZED = 401,
  HTTP_FORBIDDEN = 403,
  HTTP_NOT_FOUND = 404,
  HTTP_METHOD_NOT_ALLOWED = 405,
  HTTP_REQUEST_TIMEOUT = 408,
  HTTP_CONFLICT = 409,
  HTTP_PAYLOAD_TOO_LARGE = 413,
  HTTP_RANGE_NOT_SATISFIABLE = 416,
  HTTP_TOO_MANY_REQUESTS = 429,
  HTTP_INTERNAL_SERVER_ERROR = 500,
  HTTP_NOT_IMPLEMENTED = 501,
  HTTP_BAD_GATEWAY = 502,
  HTTP_SERVICE_UNAVAILABLE = 503,
  HTTP_GATEWAY_TIMEOUT = 504,
} http_status_t;

// Single HTTP header (name/value pair)
typedef struct
{
  char name[256];
  char value[2048];
} http_header_t;

// Parsed HTTP request
typedef struct
{
  http_method_t method;
  char uri[2048];
  char version[16];
  char query_string[2048];
  http_header_t headers[MAX_HEADERS];
  int header_count;
  char *body;
  size_t body_length;
  size_t content_length;
  // Body arrived with Transfer-Encoding: chunked (body/body_length hold the
  // already-decoded bytes; content_length stays 0).
  bool chunked;
  // Conditional-request validators. if_modified_since is 0 when the header is
  // absent or unparseable.
  char if_none_match[512];
  char if_range[128];
  time_t if_modified_since;
  // Single byte range from a "Range: bytes=..." header. has_range is only set
  // for a syntactically valid single range ("N-M", "N-", or "-N"); multiple or
  // malformed ranges are ignored (full response). For the suffix form ("-N"),
  // range_suffix is set and range_start holds the suffix length. range_end is
  // -1 for an open-ended range.
  bool has_range;
  bool range_suffix;
  off_t range_start;
  off_t range_end;
  bool keep_alive;
  bool expect_continue;
  bool is_websocket_upgrade;
  char websocket_key[64];
  char websocket_protocol[256];
} http_request_t;

// Outgoing HTTP response
typedef struct
{
  http_status_t status;
  char version[16];
  http_header_t headers[MAX_HEADERS];
  int header_count;
  char *body;
  size_t body_length;
  bool keep_alive;
  bool chunked_encoding;
  bool gzip_compressed;
  time_t last_modified;
  char etag[64];
  // When non-empty, send_http_response emits this as the
  // Access-Control-Allow-Origin header (plus "Vary: Origin" unless it is "*").
  // Set by the CORS middleware so a single field drives CORS on every response
  // path -- API handlers, static files, and error replies alike -- without each
  // handler having to add the header itself. See cors_middleware().
  char cors_origin[256];
} http_response_t;

#endif
