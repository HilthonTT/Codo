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
// the input). *out_len receives the decoded length. `consumed`, when given,
// receives the number of *input* bytes the message occupies -- the caller
// needs this to find where the next pipelined request begins. Returns 1 when a
// complete message (terminal 0-chunk plus trailer section) was consumed, 0 when
// more input is needed, -1 when the encoding is malformed.
int http_chunked_decode(const char *in, size_t in_len, char *out,
                        size_t *out_len, size_t *consumed);

// Parse exactly the first `request_len` bytes of conn->read_buffer as one
// request. The length comes from the worker's framing check; bounding the parse
// this way (rather than letting it run to read_buffer_pos) is what keeps a
// pipelined follow-up request from being swallowed as this request's body.
int parse_http_request(connection_t *conn, http_request_t *request,
                       size_t request_len);
int send_http_response(connection_t *conn, http_response_t *response);
int send_file_response(connection_t *conn, const char *file_path);
int send_error_response(connection_t *conn, http_status_t status, const char *message);

// Stage a handler's response: copies `body` (binary-safe) into the response,
// releasing whatever body was there before, and sets the status, version,
// keep-alive and Content-Type. Answers 500 itself if the copy cannot be
// allocated. Returns send_http_response's result.
int send_body_response(connection_t *conn, http_request_t *request,
                       http_response_t *response, http_status_t status,
                       const char *content_type, const char *body,
                       size_t body_length);

// send_body_response for a NUL-terminated JSON document.
int send_json_response(connection_t *conn, http_request_t *request,
                       http_response_t *response, http_status_t status,
                       const char *json);

// A JSON {"error": "<msg>"} body. A 401 also carries the WWW-Authenticate
// challenge naming the scheme the client should retry with.
int send_error_json(connection_t *conn, http_request_t *request,
                    http_response_t *response, http_status_t status,
                    const char *msg);

// Write one response header into `slot`, truncating rather than overflowing
// the fixed-size name/value fields. Does not touch response->header_count.
void response_set_header(http_response_t *response, int slot, const char *name,
                         const char *value);

#endif
