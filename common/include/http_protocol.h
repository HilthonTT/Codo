#ifndef HTTP_PROTOCOL_H
#define HTTP_PROTOCOL_H

#include "connection.h"
#include "http_types.h"

// Name advertised in the "Server:" response header. Provided by the linking
// program (the server returns its configured name; other consumers supply
// their own), so this shared layer carries no dependency on http_server_t.
const char *http_server_name(void);

int parse_http_request(connection_t *conn, http_request_t *request);
int generate_http_response(connection_t *conn, http_response_t *response);
int send_http_response(connection_t *conn, http_response_t *response);
int send_file_response(connection_t *conn, const char *file_path);
int send_error_response(connection_t *conn, http_status_t status, const char *message);

#endif
