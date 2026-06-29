#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "connection.h"
#include "http_protocol.h"
#include "http_types.h"
#include "mime.h"
#include "util.h"

int send_http_response(connection_t *conn, http_response_t *response)
{
  if (!conn || !response)
  {
    return -1;
  }

  char date_buf[64];
  format_http_date(time(NULL), date_buf, sizeof(date_buf));

  // Generate response status line + standard headers in a single format string.
  char header_buffer[MAX_HEADERS_SIZE];
  int header_len = snprintf(header_buffer, sizeof(header_buffer),
                            "%s %d %s\r\n"
                            "Server: %s\r\n"
                            "Date: %s\r\n"
                            "Content-Length: %zu\r\n"
                            "Connection: %s\r\n",
                            response->version[0] ? response->version : "HTTP/1.1",
                            (int)response->status,
                            http_status_to_string(response->status),
                            http_server_name(),
                            date_buf,
                            response->body_length,
                            response->keep_alive ? "keep-alive" : "close");

  if (header_len < 0 || (size_t)header_len >= sizeof(header_buffer))
  {
    return -1;
  }

  // Add custom headers
  for (int i = 0; i < response->header_count; i++)
  {
    int written = snprintf(header_buffer + header_len,
                           sizeof(header_buffer) - (size_t)header_len,
                           "%s: %s\r\n",
                           response->headers[i].name,
                           response->headers[i].value);
    if (written < 0 || (size_t)(header_len + written) >= sizeof(header_buffer))
    {
      return -1;
    }
    header_len += written;
  }

  // End headers
  int end_written = snprintf(header_buffer + header_len,
                             sizeof(header_buffer) - (size_t)header_len, "\r\n");
  if (end_written < 0 || (size_t)(header_len + end_written) >= sizeof(header_buffer))
  {
    return -1;
  }
  header_len += end_written;

  // Copy headers and body to write buffer
  size_t total_size = (size_t)header_len + response->body_length;
  if (total_size > MAX_RESPONSE_SIZE)
  {
    return -1; // Response too large
  }

  memcpy(conn->write_buffer, header_buffer, (size_t)header_len);
  if (response->body && response->body_length > 0)
  {
    memcpy(conn->write_buffer + header_len, response->body, response->body_length);
  }

  conn->write_buffer_pos = 0;
  conn->write_buffer_size = total_size;
  conn->state = CONN_STATE_WRITNG_RESPONSE;
  return 0;
}

int send_error_response(connection_t *conn, http_status_t status, const char *message)
{
  if (!conn)
  {
    return -1;
  }

  char error_body[1024];
  snprintf(error_body, sizeof(error_body),
           "<html><head><title>%d %s</title></head>"
           "<body><h1>%d %s</h1><p>%s</p></body></html>",
           status, http_status_to_string(status),
           status, http_status_to_string(status),
           message ? message : "");

  if (conn->response.body)
  {
    free(conn->response.body);
    conn->response.body = NULL;
  }

  conn->response.status = status;
  strcpy(conn->response.version, "HTTP/1.1");
  conn->response.body = strdup(error_body);
  conn->response.body_length = conn->response.body ? strlen(conn->response.body) : 0;
  conn->response.keep_alive = false;

  // Add HTML content type header
  strcpy(conn->response.headers[0].name, "Content-Type");
  strcpy(conn->response.headers[0].value, "text/html");
  conn->response.header_count = 1;

  return send_http_response(conn, &conn->response);
}

int send_file_response(connection_t *conn, const char *file_path)
{
  if (!conn || !file_path)
  {
    return -1;
  }

  int fd = open(file_path, O_RDONLY);
  if (fd < 0)
  {
    return send_error_response(conn, HTTP_NOT_FOUND, "File not found");
  }

  struct stat st;
  if (fstat(fd, &st) < 0)
  {
    close(fd);
    return send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR, "Could not stat file");
  }

  if (!S_ISREG(st.st_mode))
  {
    close(fd);
    return send_error_response(conn, HTTP_FORBIDDEN, "Not a regular file");
  }

  // Bound the file size to what the write buffer can hold along with headers.
  if ((size_t)st.st_size > MAX_RESPONSE_SIZE - MAX_HEADERS_SIZE)
  {
    close(fd);
    return send_error_response(conn, HTTP_PAYLOAD_TOO_LARGE, "File too large");
  }

  char *body = malloc(st.st_size > 0 ? (size_t)st.st_size : 1);
  if (!body)
  {
    close(fd);
    return send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR, "Out of memory");
  }

  off_t total = 0;
  while (total < st.st_size)
  {
    ssize_t n = read(fd, body + total, (size_t)(st.st_size - total));
    if (n < 0)
    {
      if (errno == EINTR)
      {
        continue;
      }
      free(body);
      close(fd);
      return send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR, "Read error");
    }
    if (n == 0)
    {
      break;
    }
    total += n;
  }
  close(fd);

  if (total < st.st_size)
  {
    free(body);
    return send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR, "Short read");
  }

  // Replace any previously assigned response body.
  if (conn->response.body)
  {
    free(conn->response.body);
  }

  conn->response.status = HTTP_OK;
  strcpy(conn->response.version, "HTTP/1.1");
  conn->response.body = body;
  conn->response.body_length = (size_t)st.st_size;
  conn->response.keep_alive = conn->request.keep_alive;
  conn->response.last_modified = st.st_mtime;

  const char *mime = get_mime_type(file_path);
  strcpy(conn->response.headers[0].name, "Content-Type");
  strncpy(conn->response.headers[0].value, mime, sizeof(conn->response.headers[0].value) - 1);
  conn->response.headers[0].value[sizeof(conn->response.headers[0].value) - 1] = '\0';
  conn->response.header_count = 1;

  return send_http_response(conn, &conn->response);
}

int parse_http_request(connection_t *conn, http_request_t *request)
{
  if (!conn || !request)
  {
    return -1;
  }

  // Hold onto the old body pointer in case we are re-parsing on the same
  // connection; free it after zeroing the struct.
  char *old_body = request->body;
  memset(request, 0, sizeof(http_request_t));
  if (old_body)
  {
    free(old_body);
  }

  char *buf = conn->read_buffer;
  char *end = buf + conn->read_buffer_pos;

  // Parse request line
  char *line_end = memchr(buf, '\n', (size_t)(end - buf));
  if (!line_end)
  {
    return -1;
  }
  *line_end = '\0';
  if (line_end > buf && *(line_end - 1) == '\r')
  {
    *(line_end - 1) = '\0';
  }

  char method_str[16];
  if (sscanf(buf, "%15s %2047s %15s", method_str, request->uri, request->version) != 3)
  {
    return -1;
  }

  request->method = string_to_http_method(method_str);

  // Parse query string
  char *query_start = strchr(request->uri, '?');
  if (query_start)
  {
    *query_start = '\0';
    strncpy(request->query_string, query_start + 1, sizeof(request->query_string) - 1);
    request->query_string[sizeof(request->query_string) - 1] = '\0';
  }

  // Default keep-alive on HTTP/1.1, off on HTTP/1.0. Either side may override
  // via an explicit Connection header below.
  request->keep_alive = (strcmp(request->version, "HTTP/1.1") == 0);

  // Parse headers
  char *p = line_end + 1;
  while (p < end)
  {
    char *next_end = memchr(p, '\n', (size_t)(end - p));
    if (!next_end)
    {
      break;
    }
    *next_end = '\0';
    if (next_end > p && *(next_end - 1) == '\r')
    {
      *(next_end - 1) = '\0';
    }

    if (*p == '\0')
    {
      // End of headers -- body (if any) starts here.
      p = next_end + 1;
      if (p < end)
      {
        size_t blen = (size_t)(end - p);
        request->body = malloc(blen + 1);
        if (request->body)
        {
          memcpy(request->body, p, blen);
          request->body[blen] = '\0';
          request->body_length = blen;
        }
      }
      break;
    }

    char *colon = strchr(p, ':');
    if (!colon)
    {
      p = next_end + 1;
      continue;
    }

    *colon = '\0';
    char *name = p;
    char *value = colon + 1;

    // Skip whitespace
    while (*value == ' ' || *value == '\t')
    {
      value++;
    }

    if (request->header_count < MAX_HEADERS)
    {
      strncpy(request->headers[request->header_count].name, name, 255);
      request->headers[request->header_count].name[255] = '\0';
      strncpy(request->headers[request->header_count].value, value, 2047);
      request->headers[request->header_count].value[2047] = '\0';
      request->header_count++;
    }

    // Check for special headers
    if (strcasecmp(name, "Connection") == 0)
    {
      request->keep_alive = (strcasecmp(value, "keep-alive") == 0);
    }
    else if (strcasecmp(name, "Content-Length") == 0)
    {
      request->content_length = (size_t)atol(value);
    }
    else if (strcasecmp(name, "Expect") == 0)
    {
      request->expect_continue = (strcasecmp(value, "100-continue") == 0);
    }
    else if (strcasecmp(name, "Upgrade") == 0)
    {
      request->is_websocket_upgrade = (strcasecmp(value, "websocket") == 0);
    }
    else if (strcasecmp(name, "Sec-WebSocket-Key") == 0)
    {
      strncpy(request->websocket_key, value, sizeof(request->websocket_key) - 1);
      request->websocket_key[sizeof(request->websocket_key) - 1] = '\0';
    }
    else if (strcasecmp(name, "Sec-WebSocket-Protocol") == 0)
    {
      strncpy(request->websocket_protocol, value, sizeof(request->websocket_protocol) - 1);
      request->websocket_protocol[sizeof(request->websocket_protocol) - 1] = '\0';
    }

    p = next_end + 1;
  }

  return 0;
}

int generate_http_response(connection_t *conn, http_response_t *response)
{
  // Simple passthrough: the heavy lifting lives in send_http_response, which
  // already formats and stages the response in the connection's write buffer.
  if (!conn || !response)
  {
    return -1;
  }
  return send_http_response(conn, response);
}
