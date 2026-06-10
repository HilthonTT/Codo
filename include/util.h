#ifndef OTA_UTIL_H
#define OTA_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "http_types.h"

const char *http_method_to_string(http_method_t method);
const char *http_status_to_string(http_status_t status);
http_method_t string_to_http_method(const char *method);

char *url_decode(const char *url);
void parse_query_string(const char *query, http_header_t *params, int *params_count);
bool is_valid_uri(const char *uri);

// RFC 1123 / HTTP-date formatter.
void format_http_date(time_t t, char *buf, size_t buf_size);

#endif
