#ifndef HTTP_PROTOCOL_H
#define HTTP_PROTOCOL_H

#include "connection.h"
#include "http_types.h"

// Name advertised in the "Server:" response header. Provided by the linking
// program (the server returns its configured name; other consumers supply
// their own), so this shared layer carries no dependency on http_server_t.
const char *http_server_name(void);

// Parse a Content-Length field value safely. On success returns 0 and writes
// the value to *out. Returns -1 for empty, non-numeric, negative, or
// overflowing input (leading/trailing whitespace is tolerated). Used both by
// request parsing and by the worker's message-framing check.
int http_parse_content_length(const char *value, size_t *out);

// Decode (or, with out == NULL, just validate and measure) a chunked-encoded
// message body. `in` points at the first chunk-size line; `out`, when given,
// must have room for at least in_len bytes (decoded data is always a subset of
// the input). *out_len receives the decoded length. Returns 1 when a complete
// message (terminal 0-chunk plus trailer section) was consumed, 0 when more
// input is needed, -1 when the encoding is malformed.
int http_chunked_decode(const char *in, size_t in_len, char *out, size_t *out_len);

int parse_http_request(connection_t *conn, http_request_t *request);
int generate_http_response(connection_t *conn, http_response_t *response);
int send_http_response(connection_t *conn, http_response_t *response);
int send_file_response(connection_t *conn, const char *file_path);
int send_error_response(connection_t *conn, http_status_t status, const char *message);

#endif
