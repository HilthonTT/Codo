#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "compression.h"
#include "config.h"
#include "connection.h"
#include "http_protocol.h"
#include "http_types.h"
#include "mime.h"
#include "util.h"

// Bodies below this many bytes aren't worth gzipping: the ~20-byte gzip framing
// plus the CPU cost outweigh the savings on tiny payloads.
#define GZIP_MIN_SIZE 256

int http_parse_content_length(const char *value, size_t *out)
{
  if (!value || !out)
  {
    return -1;
  }

  // Skip leading whitespace.
  while (*value == ' ' || *value == '\t')
  {
    value++;
  }
  // Content-Length must be a bare digit string. Reject empty and any explicit
  // sign -- a leading '-' would otherwise be silently wrapped by strtoul.
  if (*value == '\0' || *value == '-' || *value == '+')
  {
    return -1;
  }

  errno = 0;
  char *endptr = NULL;
  unsigned long parsed = strtoul(value, &endptr, 10);
  if (endptr == value || errno == ERANGE)
  {
    return -1;
  }

  // Only trailing whitespace may follow the digits.
  while (*endptr == ' ' || *endptr == '\t')
  {
    endptr++;
  }
  if (*endptr != '\0')
  {
    return -1;
  }
  if (parsed > (unsigned long)SIZE_MAX)
  {
    return -1;
  }

  *out = (size_t)parsed;
  return 0;
}

static int hex_digit(char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

int http_chunked_decode(const char *in, size_t in_len, char *out, size_t *out_len)
{
  if (!in || !out_len)
  {
    return -1;
  }

  size_t pos = 0;
  size_t decoded = 0;
  *out_len = 0;

  for (;;)
  {
    // Chunk-size line: hex digits, optionally followed by a ";ext" extension,
    // terminated by CRLF (a bare LF is tolerated, matching the header parser).
    const char *nl = memchr(in + pos, '\n', in_len - pos);
    if (!nl)
    {
      *out_len = decoded;
      return 0;
    }
    size_t line_len = (size_t)(nl - (in + pos));
    size_t sz_len = line_len;
    if (sz_len > 0 && in[pos + sz_len - 1] == '\r')
    {
      sz_len--;
    }

    size_t chunk = 0;
    size_t i = 0;
    for (; i < sz_len; i++)
    {
      int h = hex_digit(in[pos + i]);
      if (h < 0)
      {
        break;
      }
      if (chunk > (SIZE_MAX >> 4))
      {
        return -1; // chunk size overflows size_t
      }
      chunk = (chunk << 4) | (size_t)h;
    }
    if (i == 0)
    {
      return -1; // no hex digits where a chunk size belongs
    }
    // Anything after the digits must be a chunk extension or padding.
    if (i < sz_len && in[pos + i] != ';' && in[pos + i] != ' ' && in[pos + i] != '\t')
    {
      return -1;
    }
    pos += line_len + 1;

    if (chunk == 0)
    {
      // Terminal chunk: consume (and discard) trailer lines up to the blank
      // line that ends the message.
      for (;;)
      {
        const char *tnl = memchr(in + pos, '\n', in_len - pos);
        if (!tnl)
        {
          *out_len = decoded;
          return 0;
        }
        size_t tline = (size_t)(tnl - (in + pos));
        size_t tlen = tline;
        if (tlen > 0 && in[pos + tlen - 1] == '\r')
        {
          tlen--;
        }
        pos += tline + 1;
        if (tlen == 0)
        {
          *out_len = decoded;
          return 1;
        }
      }
    }

    if (in_len - pos < chunk)
    {
      *out_len = decoded;
      return 0; // chunk data still in flight
    }
    if (out)
    {
      memcpy(out + decoded, in + pos, chunk);
    }
    decoded += chunk;
    pos += chunk;

    // CRLF (or LF) closing the chunk data.
    if (pos >= in_len)
    {
      *out_len = decoded;
      return 0;
    }
    if (in[pos] == '\r')
    {
      pos++;
      if (pos >= in_len)
      {
        *out_len = decoded;
        return 0;
      }
    }
    if (in[pos] != '\n')
    {
      return -1;
    }
    pos++;
  }
}

// Weak entity-tag comparison (RFC 7232 §2.3.2): a W/ prefix on the candidate
// is stripped before comparing. The etags we generate are always strong.
static bool etag_match_weak(const char *candidate, size_t len, const char *etag)
{
  if (len >= 2 && (candidate[0] == 'W' || candidate[0] == 'w') && candidate[1] == '/')
  {
    candidate += 2;
    len -= 2;
  }
  return strlen(etag) == len && memcmp(candidate, etag, len) == 0;
}

// True if an If-None-Match header value (a comma-separated entity-tag list,
// or "*") matches the current etag.
static bool etag_list_contains(const char *list, const char *etag)
{
  const char *p = list;
  while (*p)
  {
    while (*p == ' ' || *p == '\t' || *p == ',')
    {
      p++;
    }
    if (!*p)
    {
      break;
    }
    const char *start = p;
    while (*p && *p != ',')
    {
      p++;
    }
    const char *end = p;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
    {
      end--;
    }
    size_t len = (size_t)(end - start);
    if (len == 1 && *start == '*')
    {
      return true;
    }
    if (etag_match_weak(start, len, etag))
    {
      return true;
    }
  }
  return false;
}

// Conditional-GET check (RFC 7232 §6 precedence): If-None-Match, when present,
// decides alone; If-Modified-Since is only consulted without it.
static bool request_not_modified(const http_request_t *request, const char *etag,
                                 time_t mtime)
{
  if (request->if_none_match[0])
  {
    return etag_list_contains(request->if_none_match, etag);
  }
  if (request->if_modified_since != 0 && mtime != 0)
  {
    return mtime <= request->if_modified_since;
  }
  return false;
}

// If-Range gate: the Range header only applies when the client's validator
// still matches the current entity. An entity-tag requires a strong match (a
// weak tag never qualifies); a date must equal the file's mtime exactly.
// Absent If-Range, the Range always applies.
static bool if_range_matches(const http_request_t *request, const char *etag,
                             time_t mtime)
{
  const char *v = request->if_range;
  if (!v[0])
  {
    return true;
  }
  if (v[0] == '"')
  {
    return strcmp(v, etag) == 0;
  }
  if ((v[0] == 'W' || v[0] == 'w') && v[1] == '/')
  {
    return false;
  }
  time_t t = parse_http_date(v);
  return t != (time_t)-1 && t == mtime;
}

// Resolve the request's parsed byte range against the actual entity size.
// Returns 0 with [*start, *end] filled (inclusive, clamped to the entity) when
// satisfiable, -1 when unsatisfiable (-> 416).
static int resolve_range(const http_request_t *request, off_t size, off_t *start,
                         off_t *end)
{
  if (size <= 0)
  {
    return -1; // no byte of an empty entity is addressable
  }
  if (request->range_suffix)
  {
    if (request->range_start == 0)
    {
      return -1; // "-0" selects nothing
    }
    off_t n = request->range_start > size ? size : request->range_start;
    *start = size - n;
    *end = size - 1;
    return 0;
  }
  if (request->range_start >= size)
  {
    return -1;
  }
  *start = request->range_start;
  *end = (request->range_end < 0 || request->range_end >= size) ? size - 1
                                                                : request->range_end;
  return 0;
}

// Parse a "Range: bytes=..." value into the request. Only a single range is
// supported; a multi-range or malformed value leaves has_range unset so the
// request is served in full (RFC 7233 allows ignoring the header).
static void parse_range_header(const char *value, http_request_t *request)
{
  if (strncasecmp(value, "bytes=", 6) != 0)
  {
    return;
  }
  const char *spec = value + 6;
  if (strchr(spec, ','))
  {
    return; // multi-range: ignored, full response
  }
  while (*spec == ' ' || *spec == '\t')
  {
    spec++;
  }

  if (*spec == '-')
  {
    // Suffix form "-N": the last N bytes.
    errno = 0;
    char *endp = NULL;
    long long n = strtoll(spec + 1, &endp, 10);
    if (endp == spec + 1 || errno == ERANGE || n < 0)
    {
      return;
    }
    while (*endp == ' ' || *endp == '\t')
    {
      endp++;
    }
    if (*endp)
    {
      return;
    }
    request->range_suffix = true;
    request->range_start = (off_t)n;
    request->range_end = -1;
    request->has_range = true;
    return;
  }

  errno = 0;
  char *endp = NULL;
  long long first = strtoll(spec, &endp, 10);
  if (endp == spec || errno == ERANGE || first < 0 || *endp != '-')
  {
    return;
  }
  const char *after = endp + 1;
  while (*after == ' ' || *after == '\t')
  {
    after++;
  }
  long long last = -1;
  if (*after)
  {
    errno = 0;
    char *endp2 = NULL;
    last = strtoll(after, &endp2, 10);
    if (endp2 == after || errno == ERANGE || last < first)
    {
      return;
    }
    while (*endp2 == ' ' || *endp2 == '\t')
    {
      endp2++;
    }
    if (*endp2)
    {
      return;
    }
  }
  request->range_start = (off_t)first;
  request->range_end = (off_t)last;
  request->has_range = true;
}

// True if `value` contains a CR or LF. Header names/values carrying these
// could inject extra response headers (response splitting), so we refuse to
// emit them.
static bool header_field_has_crlf(const char *field)
{
  for (const char *c = field; *c; c++)
  {
    if (*c == '\r' || *c == '\n')
    {
      return true;
    }
  }
  return false;
}

// Remove any CR/LF bytes from a stored header field in place. Header parsing
// only strips the single terminating '\r'; a lone embedded '\r' would survive
// and could leak into logs or an echoed response, so drop them all here.
static void strip_crlf_inplace(char *s)
{
  char *dst = s;
  for (char *src = s; *src; src++)
  {
    if (*src != '\r' && *src != '\n')
    {
      *dst++ = *src;
    }
  }
  *dst = '\0';
}

// True if the client advertised gzip in Accept-Encoding. We don't parse q-values
// -- a bare presence of the "gzip" token is enough for our purposes.
static bool client_accepts_gzip(const http_request_t *request)
{
  for (int i = 0; i < request->header_count; i++)
  {
    if (strcasecmp(request->headers[i].name, "Accept-Encoding") == 0)
    {
      return strcasestr(request->headers[i].value, "gzip") != NULL;
    }
  }
  return false;
}

// Only compress content types that actually shrink: text, JSON, JS, XML, SVG.
// Already-compressed formats (images, video, archives) would just waste CPU.
static bool mime_type_compressible(const char *v)
{
  return strncasecmp(v, "text/", 5) == 0 || strcasestr(v, "json") != NULL ||
         strcasestr(v, "javascript") != NULL || strcasestr(v, "xml") != NULL ||
         strcasestr(v, "svg") != NULL;
}

static bool content_type_compressible(const http_response_t *response)
{
  for (int i = 0; i < response->header_count; i++)
  {
    if (strcasecmp(response->headers[i].name, "Content-Type") == 0)
    {
      return mime_type_compressible(response->headers[i].value);
    }
  }
  return false;
}

// Decide whether this response body should be gzip-compressed: the client must
// accept it, the body must be present and worth compressing, the content type
// must be compressible, and we must not double-encode.
static bool response_should_gzip(connection_t *conn, http_response_t *response)
{
  if (response->gzip_compressed)
  {
    return false;
  }
  if (!response->body || response->body_length < GZIP_MIN_SIZE)
  {
    return false;
  }
  return content_type_compressible(response) && client_accepts_gzip(&conn->request);
}

int send_http_response(connection_t *conn, http_response_t *response)
{
  if (!conn || !response)
  {
    return -1;
  }

  // Body actually placed in the write buffer. gzip may swap in a compressed copy
  // (freed before we return). A queued static file (conn->file_fd >= 0) is
  // streamed separately by the write path via sendfile, so here it contributes
  // only its length to Content-Length -- no bytes are copied into the buffer.
  const char *out_body = response->body;
  size_t out_body_len = response->body_length;
  unsigned char *gzip_buf = NULL;
  bool gzipped = false;
  bool streaming_file = (conn->file_fd >= 0);

  if (!streaming_file && response_should_gzip(conn, response))
  {
    unsigned char *compressed = NULL;
    size_t compressed_len = 0;
    if (gzip_compress_buffer(response->body, response->body_length, &compressed,
                             &compressed_len) == 0)
    {
      // Only adopt the compressed copy if it actually shrank the payload.
      if (compressed_len < response->body_length)
      {
        gzip_buf = compressed;
        out_body = (const char *)compressed;
        out_body_len = compressed_len;
        gzipped = true;
      }
      else
      {
        free(compressed);
      }
    }
  }

  // A 304 carries no body by definition; omit Content-Length entirely rather
  // than advertising a zero-length representation to caches.
  bool no_body = (response->status == HTTP_NOT_MODIFIED);

  // file_offset is non-zero when a byte range is being served, so the length
  // is what remains of the file window, not the whole file.
  size_t content_length =
      streaming_file ? conn->file_size - (size_t)conn->file_offset : out_body_len;

  char date_buf[64];
  format_http_date(time(NULL), date_buf, sizeof(date_buf));

  char content_length_hdr[64] = "";
  if (!no_body)
  {
    snprintf(content_length_hdr, sizeof(content_length_hdr),
             "Content-Length: %zu\r\n", content_length);
  }

  // Generate response status line + standard headers in a single format string.
  char header_buffer[MAX_HEADERS_SIZE];
  int header_len = snprintf(header_buffer, sizeof(header_buffer),
                            "%s %d %s\r\n"
                            "Server: %s\r\n"
                            "Date: %s\r\n"
                            "%s"
                            "Connection: %s\r\n",
                            response->version[0] ? response->version : "HTTP/1.1",
                            (int)response->status,
                            http_status_to_string(response->status),
                            http_server_name(),
                            date_buf,
                            content_length_hdr,
                            response->keep_alive ? "keep-alive" : "close");

  if (header_len < 0 || (size_t)header_len >= sizeof(header_buffer))
  {
    free(gzip_buf);
    return -1;
  }

  // Announce the gzip encoding (and vary on Accept-Encoding so caches key the
  // compressed and plain representations separately).
  if (gzipped)
  {
    int written = snprintf(header_buffer + header_len,
                           sizeof(header_buffer) - (size_t)header_len,
                           "Content-Encoding: gzip\r\n"
                           "Vary: Accept-Encoding\r\n");
    if (written < 0 || (size_t)(header_len + written) >= sizeof(header_buffer))
    {
      free(gzip_buf);
      return -1;
    }
    header_len += written;
  }

  // Entity validators, set on static-file responses (200/206 and 304 alike).
  // A gzipped body is a different representation of the entity, so its etag is
  // weakened -- a strong tag must be byte-exact across encodings.
  if (response->last_modified != 0 || response->etag[0])
  {
    char lm_buf[96] = "";
    if (response->last_modified != 0)
    {
      char lm_date[64];
      format_http_date(response->last_modified, lm_date, sizeof(lm_date));
      snprintf(lm_buf, sizeof(lm_buf), "Last-Modified: %s\r\n", lm_date);
    }
    char etag_buf[96] = "";
    if (response->etag[0] && !header_field_has_crlf(response->etag))
    {
      snprintf(etag_buf, sizeof(etag_buf), "ETag: %s%s\r\n",
               gzipped ? "W/" : "", response->etag);
    }
    int written = snprintf(header_buffer + header_len,
                           sizeof(header_buffer) - (size_t)header_len,
                           "%s%s", lm_buf, etag_buf);
    if (written < 0 || (size_t)(header_len + written) >= sizeof(header_buffer))
    {
      free(gzip_buf);
      return -1;
    }
    header_len += written;
  }

  // Add custom headers
  for (int i = 0; i < response->header_count; i++)
  {
    // Refuse to emit a header whose name or value smuggles CR/LF: a handler
    // that echoes client data must not be able to split the response.
    if (header_field_has_crlf(response->headers[i].name) ||
        header_field_has_crlf(response->headers[i].value))
    {
      free(gzip_buf);
      return -1;
    }
    int written = snprintf(header_buffer + header_len,
                           sizeof(header_buffer) - (size_t)header_len,
                           "%s: %s\r\n",
                           response->headers[i].name,
                           response->headers[i].value);
    if (written < 0 || (size_t)(header_len + written) >= sizeof(header_buffer))
    {
      free(gzip_buf);
      return -1;
    }
    header_len += written;
  }

  // Emit the CORS origin header the middleware selected for this response. Kept
  // here (rather than in each handler) so every response path carries it. The
  // value is server-controlled config, but guard against CR/LF injection all
  // the same before writing it into the header block.
  if (response->cors_origin[0])
  {
    if (header_field_has_crlf(response->cors_origin))
    {
      free(gzip_buf);
      return -1;
    }
    // "Vary: Origin" is only meaningful when the allowed origin is specific --
    // for a wildcard the response does not depend on the request's Origin.
    const char *vary = (strcmp(response->cors_origin, "*") == 0) ? "" : "Vary: Origin\r\n";
    int written = snprintf(header_buffer + header_len,
                           sizeof(header_buffer) - (size_t)header_len,
                           "Access-Control-Allow-Origin: %s\r\n%s",
                           response->cors_origin, vary);
    if (written < 0 || (size_t)(header_len + written) >= sizeof(header_buffer))
    {
      free(gzip_buf);
      return -1;
    }
    header_len += written;
  }

  // End headers
  int end_written = snprintf(header_buffer + header_len,
                             sizeof(header_buffer) - (size_t)header_len, "\r\n");
  if (end_written < 0 || (size_t)(header_len + end_written) >= sizeof(header_buffer))
  {
    free(gzip_buf);
    return -1;
  }
  header_len += end_written;

  // Copy headers (and, unless a file is being streamed, the body) to the write
  // buffer. For a streamed file only the headers are staged here; the body bytes
  // are pumped from the fd by the write path, so they don't count toward the
  // in-memory response cap.
  size_t body_in_buffer = (streaming_file || no_body) ? 0 : out_body_len;
  size_t total_size = (size_t)header_len + body_in_buffer;
  if (total_size > MAX_RESPONSE_SIZE)
  {
    free(gzip_buf);
    return -1; // Response too large
  }

  memcpy(conn->write_buffer, header_buffer, (size_t)header_len);
  if (body_in_buffer > 0 && out_body)
  {
    memcpy(conn->write_buffer + header_len, out_body, body_in_buffer);
  }
  free(gzip_buf);

  conn->write_buffer_pos = 0;
  conn->write_buffer_size = total_size;
  conn->state = CONN_STATE_WRITING_RESPONSE;
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
  // Drop any validators a partially-built file response left behind; an error
  // body must not carry the entity's ETag / Last-Modified.
  conn->response.etag[0] = '\0';
  conn->response.last_modified = 0;

  // Add HTML content type header
  strcpy(conn->response.headers[0].name, "Content-Type");
  strcpy(conn->response.headers[0].value, "text/html");
  conn->response.header_count = 1;

  return send_http_response(conn, &conn->response);
}

// 416 with the "Content-Range: bytes */<size>" the client needs to learn the
// entity's actual extent. Built here rather than via send_error_response, which
// cannot carry the extra header.
static int send_range_not_satisfiable(connection_t *conn, off_t size)
{
  char body[256];
  snprintf(body, sizeof(body),
           "<html><head><title>416 Range Not Satisfiable</title></head>"
           "<body><h1>416 Range Not Satisfiable</h1></body></html>");

  if (conn->response.body)
  {
    free(conn->response.body);
  }

  conn->response.status = HTTP_RANGE_NOT_SATISFIABLE;
  strcpy(conn->response.version, "HTTP/1.1");
  conn->response.body = strdup(body);
  conn->response.body_length = conn->response.body ? strlen(conn->response.body) : 0;
  conn->response.keep_alive = conn->request.keep_alive;
  conn->response.etag[0] = '\0';
  conn->response.last_modified = 0;

  strcpy(conn->response.headers[0].name, "Content-Type");
  strcpy(conn->response.headers[0].value, "text/html");
  strcpy(conn->response.headers[1].name, "Content-Range");
  snprintf(conn->response.headers[1].value, sizeof(conn->response.headers[1].value),
           "bytes */%lld", (long long)size);
  conn->response.header_count = 2;

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

  const char *mime = get_mime_type(file_path);

  // Strong validator: mtime + size identify the entity. Sent as ETag /
  // Last-Modified on every file response and compared against the request's
  // conditional headers below.
  char etag[64];
  snprintf(etag, sizeof(etag), "\"%llx-%llx\"",
           (unsigned long long)st.st_mtime, (unsigned long long)st.st_size);

  bool is_get = (conn->request.method == HTTP_GET);

  // Conditional GET: a matching validator means the client's cached copy is
  // current -- answer 304 with the validators and no body.
  if (is_get && request_not_modified(&conn->request, etag, st.st_mtime))
  {
    close(fd);
    if (conn->response.body)
    {
      free(conn->response.body);
      conn->response.body = NULL;
    }
    conn->response.status = HTTP_NOT_MODIFIED;
    strcpy(conn->response.version, "HTTP/1.1");
    conn->response.body_length = 0;
    conn->response.keep_alive = conn->request.keep_alive;
    conn->response.last_modified = st.st_mtime;
    snprintf(conn->response.etag, sizeof(conn->response.etag), "%s", etag);
    conn->response.header_count = 0;
    return send_http_response(conn, &conn->response);
  }

  // Byte range: applies only when the If-Range validator (if any) still
  // matches, so a client resuming against a changed file gets the full entity
  // instead of a corrupt splice.
  off_t body_start = 0;
  off_t body_end = (off_t)st.st_size - 1;
  bool partial = false;
  if (is_get && conn->request.has_range &&
      if_range_matches(&conn->request, etag, st.st_mtime))
  {
    if (resolve_range(&conn->request, st.st_size, &body_start, &body_end) != 0)
    {
      close(fd);
      return send_range_not_satisfiable(conn, st.st_size);
    }
    partial = true;
  }

  size_t body_len = (size_t)(body_end - body_start + 1);
  bool fits_buffer = body_len <= (size_t)(MAX_RESPONSE_SIZE - MAX_HEADERS_SIZE);

  // A compressible asset the client accepts gzip for is worth reading into
  // memory and compressing (browsers expect gzipped HTML/CSS/JS). Everything
  // else -- binary/media, oversized files, or clients that didn't ask -- takes
  // the zero-copy sendfile path instead. Files too large to buffer always
  // sendfile, compressible or not, rather than being rejected. A range
  // response is never gzipped: the range addresses the identity encoding.
  bool want_gzip = !partial && !conn->ssl_enabled && fits_buffer &&
                   body_len >= GZIP_MIN_SIZE &&
                   mime_type_compressible(mime) &&
                   client_accepts_gzip(&conn->request);

  // Plaintext connections stream the file straight from the fd to the socket
  // with sendfile(2): zero-copy, and no in-memory size cap. TLS connections
  // can't hand a raw fd to the OpenSSL layer, and a response we intend to gzip
  // must be buffered first, so both fall through to the read path below.
  if (!conn->ssl_enabled && !want_gzip)
  {
    if (conn->response.body)
    {
      free(conn->response.body);
      conn->response.body = NULL;
    }

    // Hand the open fd to the connection; the write path streams it and closes
    // it when done (cleanup_connection also closes it if the connection dies).
    // The [offset, size) window is the requested range (the whole file when no
    // range applies), so the sendfile loop and Content-Length need no
    // range-awareness of their own.
    conn->file_fd = fd;
    conn->file_offset = body_start;
    conn->file_size = (size_t)(body_end + 1);

    conn->response.status = partial ? HTTP_PARTIAL_CONTENT : HTTP_OK;
    strcpy(conn->response.version, "HTTP/1.1");
    conn->response.body = NULL;
    conn->response.body_length = 0;
    conn->response.keep_alive = conn->request.keep_alive;
    conn->response.last_modified = st.st_mtime;
    snprintf(conn->response.etag, sizeof(conn->response.etag), "%s", etag);

    strcpy(conn->response.headers[0].name, "Content-Type");
    strncpy(conn->response.headers[0].value, mime,
            sizeof(conn->response.headers[0].value) - 1);
    conn->response.headers[0].value[sizeof(conn->response.headers[0].value) - 1] = '\0';
    strcpy(conn->response.headers[1].name, "Accept-Ranges");
    strcpy(conn->response.headers[1].value, "bytes");
    conn->response.header_count = 2;
    if (partial)
    {
      strcpy(conn->response.headers[2].name, "Content-Range");
      snprintf(conn->response.headers[2].value,
               sizeof(conn->response.headers[2].value), "bytes %lld-%lld/%lld",
               (long long)body_start, (long long)body_end, (long long)st.st_size);
      conn->response.header_count = 3;
    }

    // send_http_response sees conn->file_fd >= 0 and stages only the headers
    // (Content-Length = file_size); handle_client_write sendfile()s the body.
    if (send_http_response(conn, &conn->response) != 0)
    {
      close(conn->file_fd);
      conn->file_fd = -1;
      conn->file_size = 0;
      return send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR, "Response too large");
    }
    return 0;
  }

  // Bound the buffered window (whole file, or the requested range of it) to
  // what the write buffer can hold along with headers.
  if (!fits_buffer)
  {
    close(fd);
    return send_error_response(conn, HTTP_PAYLOAD_TOO_LARGE, "File too large");
  }

  char *body = malloc(body_len > 0 ? body_len : 1);
  if (!body)
  {
    close(fd);
    return send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR, "Out of memory");
  }

  size_t total = 0;
  while (total < body_len)
  {
    ssize_t n = pread(fd, body + total, body_len - total, body_start + (off_t)total);
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
    total += (size_t)n;
  }
  close(fd);

  if (total < body_len)
  {
    free(body);
    return send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR, "Short read");
  }

  // Replace any previously assigned response body.
  if (conn->response.body)
  {
    free(conn->response.body);
  }

  conn->response.status = partial ? HTTP_PARTIAL_CONTENT : HTTP_OK;
  strcpy(conn->response.version, "HTTP/1.1");
  conn->response.body = body;
  conn->response.body_length = body_len;
  conn->response.keep_alive = conn->request.keep_alive;
  conn->response.last_modified = st.st_mtime;
  snprintf(conn->response.etag, sizeof(conn->response.etag), "%s", etag);

  strcpy(conn->response.headers[0].name, "Content-Type");
  strncpy(conn->response.headers[0].value, mime, sizeof(conn->response.headers[0].value) - 1);
  conn->response.headers[0].value[sizeof(conn->response.headers[0].value) - 1] = '\0';
  strcpy(conn->response.headers[1].name, "Accept-Ranges");
  strcpy(conn->response.headers[1].value, "bytes");
  conn->response.header_count = 2;
  if (partial)
  {
    strcpy(conn->response.headers[2].name, "Content-Range");
    snprintf(conn->response.headers[2].value,
             sizeof(conn->response.headers[2].value), "bytes %lld-%lld/%lld",
             (long long)body_start, (long long)body_end, (long long)st.st_size);
    conn->response.header_count = 3;
  }

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
  bool saw_content_length = false;
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
      // End of headers -- body (if any) starts here. The worker only dispatches
      // once the full message has arrived, so bound the body to the declared
      // Content-Length rather than grabbing every trailing byte (which could
      // include a pipelined next request); a chunked body is decoded in place.
      p = next_end + 1;
      if (request->chunked && saw_content_length)
      {
        // Both framings present is a request-smuggling vector; refuse.
        return -1;
      }
      if (p < end)
      {
        size_t blen = (size_t)(end - p);
        if (request->chunked)
        {
          // Decoded data is always a subset of the encoded input, so blen
          // bounds the output. The worker already validated completeness.
          char *decoded = malloc(blen + 1);
          if (!decoded)
          {
            return -2;
          }
          size_t decoded_len = 0;
          if (http_chunked_decode(p, blen, decoded, &decoded_len) != 1)
          {
            free(decoded);
            return -1;
          }
          decoded[decoded_len] = '\0';
          request->body = decoded;
          request->body_length = decoded_len;
          break;
        }
        if (request->content_length > 0 && request->content_length < blen)
        {
          blen = request->content_length;
        }
        request->body = malloc(blen + 1);
        if (!request->body)
        {
          // Signal an internal error so the caller replies 500 rather than
          // proceeding as if the request carried no body.
          return -2;
        }
        memcpy(request->body, p, blen);
        request->body[blen] = '\0';
        request->body_length = blen;
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
      // Drop any embedded CR/LF so stored fields can't smuggle control bytes.
      strip_crlf_inplace(request->headers[request->header_count].name);
      strip_crlf_inplace(request->headers[request->header_count].value);
      request->header_count++;
    }

    // Check for special headers
    if (strcasecmp(name, "Connection") == 0)
    {
      // Connection is a comma-separated token list; match keep-alive/upgrade/
      // close as tokens (case-insensitive) so "keep-alive, Upgrade" is not
      // mistaken for a close. An explicit "close" token wins.
      if (strcasestr(value, "close"))
      {
        request->keep_alive = false;
      }
      else if (strcasestr(value, "keep-alive") || strcasestr(value, "upgrade"))
      {
        request->keep_alive = true;
      }
    }
    else if (strcasecmp(name, "Content-Length") == 0)
    {
      if (http_parse_content_length(value, &request->content_length) != 0)
      {
        return -1;
      }
      saw_content_length = true;
    }
    else if (strcasecmp(name, "Transfer-Encoding") == 0)
    {
      // Only chunked is understood. Any other coding is rejected rather than
      // risking a mis-framed message body (the worker answers those with 501
      // before parsing ever runs).
      size_t vlen = strlen(value);
      while (vlen > 0 && (value[vlen - 1] == ' ' || value[vlen - 1] == '\t'))
      {
        vlen--;
      }
      if (vlen != 7 || strncasecmp(value, "chunked", 7) != 0)
      {
        return -1;
      }
      request->chunked = true;
    }
    else if (strcasecmp(name, "If-None-Match") == 0)
    {
      strncpy(request->if_none_match, value, sizeof(request->if_none_match) - 1);
      request->if_none_match[sizeof(request->if_none_match) - 1] = '\0';
    }
    else if (strcasecmp(name, "If-Modified-Since") == 0)
    {
      time_t t = parse_http_date(value);
      request->if_modified_since = (t == (time_t)-1) ? 0 : t;
    }
    else if (strcasecmp(name, "If-Range") == 0)
    {
      strncpy(request->if_range, value, sizeof(request->if_range) - 1);
      request->if_range[sizeof(request->if_range) - 1] = '\0';
    }
    else if (strcasecmp(name, "Range") == 0)
    {
      parse_range_header(value, request);
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
