#ifndef OTA_HTTP_TYPES_H
#define OTA_HTTP_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "config.h"

// HTTP methods
typedef enum {
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
typedef enum {
  HTTP_OK = 200,
  HTTP_CREATED = 201,
  HTTP_ACCEPTED = 202,
  HTTP_NO_CONTENT = 204,
  HTTP_MOVED_PERMANENTLY = 301,
  HTTP_FOUND = 302,
  HTTP_NOT_MODIFIED = 304,
  HTTP_BAD_REQUEST = 400,
  HTTP_UNAUTHORIZED = 401,
  HTTP_FORBIDDEN = 403,
  HTTP_NOT_FOUND = 404,
  HTTP_METHOD_NOT_ALLOWED = 405,
  HTTP_REQUEST_TIMEOUT = 408,
  HTTP_PAYLOAD_TOO_LARGE = 413,
  HTTP_INTERNAL_SERVER_ERROR = 500,
  HTTP_NOT_IMPLEMENTED = 501,
  HTTP_BAD_GATEWAY = 502,
  HTTP_SERVICE_UNAVAILABLE = 503,
  HTTP_GATEWAY_TIMEOUT = 504,
} http_status_t;

// Single HTTP header (name/value pair)
typedef struct {
  char name[256];
  char value[2048];
} http_header_t;

// Parsed HTTP request
typedef struct {
  http_method_t method;
  char uri[2048];
  char version[16];
  char query_string[2048];
  http_header_t headers[MAX_HEADERS];
  int header_count;
  char *body;
  size_t body_length;
  size_t content_length;
  bool keep_alive;
  bool expect_continue;
  bool is_websocket_upgrade;
  char websocket_key[64];
  char websocket_protocol[256];
} http_request_t;

// Outgoing HTTP response
typedef struct {
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
} http_response_t;

#endif
