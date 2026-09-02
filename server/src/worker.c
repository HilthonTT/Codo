#include <errno.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/sendfile.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "connection.h"
#include "http_protocol.h"
#include "http_types.h"
#include "net_util.h"
#include "route.h"
#include "server.h"
#include "stats.h"
#include "thread_pool.h"
#include "websocket.h"
#include "worker.h"

// Outcome of one non-blocking socket transfer. EINTR is retried inside the
// helpers, so callers only ever see a terminal outcome.
typedef enum
{
  IO_OK,          // bytes moved; *transferred is non-zero
  IO_WOULD_BLOCK, // nothing more right now -- wait for the next epoll event
  IO_CLOSED,      // peer closed cleanly
  IO_ERROR,       // fatal; tear the connection down
} io_result_t;

// Read up to `len` bytes, over TLS or a plain socket depending on how the
// connection was accepted.
static io_result_t conn_recv(connection_t *conn, void *buf, size_t len,
                             size_t *transferred)
{
  *transferred = 0;
  for (;;)
  {
    ssize_t n;
    if (conn->ssl_enabled && conn->ssl)
    {
      n = SSL_read(conn->ssl, buf, (int)len);
      if (n <= 0)
      {
        int ssl_error = SSL_get_error(conn->ssl, (int)n);
        return (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE)
                   ? IO_WOULD_BLOCK
                   : IO_ERROR;
      }
    }
    else
    {
      n = read(conn->socket_fd, buf, len);
      if (n == 0)
      {
        return IO_CLOSED;
      }
      if (n < 0)
      {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
          return IO_WOULD_BLOCK;
        }
        if (errno == EINTR)
        {
          continue;
        }
        return IO_ERROR;
      }
    }
    *transferred = (size_t)n;
    return IO_OK;
  }
}

// Write up to `len` bytes, over TLS or a plain socket. A plain write returning
// 0 is reported as an error rather than looped on, which would spin forever.
static io_result_t conn_send(connection_t *conn, const void *buf, size_t len,
                             size_t *transferred)
{
  *transferred = 0;
  for (;;)
  {
    ssize_t n;
    if (conn->ssl_enabled && conn->ssl)
    {
      n = SSL_write(conn->ssl, buf, (int)len);
      if (n <= 0)
      {
        int ssl_error = SSL_get_error(conn->ssl, (int)n);
        return (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE)
                   ? IO_WOULD_BLOCK
                   : IO_ERROR;
      }
    }
    else
    {
      n = write(conn->socket_fd, buf, len);
      if (n < 0)
      {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
          return IO_WOULD_BLOCK;
        }
        if (errno == EINTR)
        {
          continue;
        }
        return IO_ERROR;
      }
      if (n == 0)
      {
        return IO_ERROR;
      }
    }
    *transferred = (size_t)n;
    return IO_OK;
  }
}

// Drop the first `len` bytes of the read buffer, sliding whatever follows to
// the front -- the next pipelined request, or the request behind a consumed
// PROXY header.
static void conn_consume_read(connection_t *conn, size_t len)
{
  size_t leftover = conn->read_buffer_pos - len;
  if (leftover > 0)
  {
    memmove(conn->read_buffer, conn->read_buffer + len, leftover);
  }
  conn->read_buffer_pos = leftover;
  conn->read_buffer[leftover] = '\0';
}

// Re-arm this connection's socket in its worker's epoll set. An `events` mask
// of 0 parks the connection, which is how it is hidden from the event loop
// while a pool thread owns it.
static int arm_socket(worker_thread_t *worker, connection_t *conn, uint32_t events)
{
  struct epoll_event ev;
  ev.events = events;
  ev.data.ptr = conn;
  return epoll_ctl(worker->epoll_fd, EPOLL_CTL_MOD, conn->socket_fd, &ev);
}

// Consume a PROXY protocol v1 header from the head of the read buffer and
// adopt the client address it declares. Only called when the server is
// configured to trust the peer (TRUST_PROXY_PROTOCOL) -- otherwise any client
// could name its own source address and walk past per-IP rate limiting.
//
// Returns 1 when a header was consumed, 0 when more bytes are needed, -1 when
// the connection is unusable (no header, or a malformed one). Trust is
// all-or-nothing by design: with it on, the header is mandatory, so a direct
// connection that bypasses the balancer is refused rather than silently
// treated as anonymous.
static int consume_proxy_header(connection_t *conn)
{
  // v1 headers are at most 107 bytes plus CRLF.
  const size_t max_len = 108;
  const char *buf = conn->read_buffer;
  size_t avail = conn->read_buffer_pos;

  if (avail < 6)
  {
    return memcmp(buf, "PROXY ", avail) == 0 ? 0 : -1;
  }
  if (memcmp(buf, "PROXY ", 6) != 0)
  {
    return -1;
  }

  const char *nl = memchr(buf, '\n', avail < max_len ? avail : max_len);
  if (!nl)
  {
    return avail >= max_len ? -1 : 0; // still arriving, or over-long
  }
  size_t line_len = (size_t)(nl - buf) + 1;

  char line[128];
  size_t copy = line_len - 1;
  if (copy > 0 && buf[copy - 1] == '\r')
  {
    copy--;
  }
  if (copy >= sizeof(line))
  {
    return -1;
  }
  memcpy(line, buf, copy);
  line[copy] = '\0';

  // "PROXY UNKNOWN..." is legal and means the sender could not determine the
  // peer; keep the transport-level address in that case.
  char proto[8], src[NET_ADDR_STRLEN], dst[NET_ADDR_STRLEN];
  unsigned sport = 0, dport = 0;
  if (sscanf(line, "PROXY %7s %45s %45s %u %u", proto, src, dst, &sport, &dport) == 5 &&
      (strcmp(proto, "TCP4") == 0 || strcmp(proto, "TCP6") == 0) &&
      sport <= 65535)
  {
    struct sockaddr_storage declared;
    if (net_addr_from_str(src, (uint16_t)sport, &declared) == 0)
    {
      conn->client_addr = declared;
    }
  }
  else if (strncmp(line, "PROXY UNKNOWN", 13) != 0)
  {
    return -1;
  }

  // Drop the header so the HTTP parser sees a clean request.
  conn_consume_read(conn, line_len);
  return 1;
}

// Payload handed to a storage-pool thread describing one offloaded request.
typedef struct
{
  worker_thread_t *worker;
  connection_t *conn;
  route_handler_t handler;
} offload_task_t;

// Storage-pool priority for a request: reads jump ahead of writes so a GET
// (page read) isn't stuck behind a burst of WAL-fsyncing writes.
static int request_priority(const http_request_t *request)
{
  return (request->method == HTTP_GET || request->method == HTTP_HEAD)
             ? STORAGE_PRIORITY_READ
             : STORAGE_PRIORITY_WRITE;
}

// Shared tail for a completed request (inline or offloaded): account the
// request, reset the read buffer for the next one, and arm the socket for the
// response write. Returns the epoll_ctl result (0 ok, -1 -> caller tears down).
static int finalize_response(worker_thread_t *worker, connection_t *conn)
{
  atomic_fetch_add(&g_server.total_requests, 1);
  atomic_fetch_add(&worker->requests_processed, 1);

  // The read buffer is deliberately left alone: handle_client_data has already
  // trimmed it to the bytes of the next pipelined request, if any.
  conn->state = CONN_STATE_WRITING_RESPONSE;

  return arm_socket(worker, conn, EPOLLOUT | EPOLLET);
}

// Answer a request we refuse to process any further. The read buffer is
// dropped wholesale rather than trimmed: once framing is in doubt we cannot
// tell where a following pipelined request would begin.
static int reject_request(worker_thread_t *worker, connection_t *conn,
                          http_status_t status, const char *message)
{
  send_error_response(conn, status, message);
  conn->read_buffer_pos = 0;
  conn->read_buffer[0] = '\0';
  return finalize_response(worker, conn);
}

// Runs on a storage-pool thread: execute the blocking handler that fills the
// write buffer, then hand the connection back to its worker by arming EPOLLOUT.
static void run_offloaded_handler(void *arg)
{
  offload_task_t *task = (offload_task_t *)arg;
  worker_thread_t *worker = task->worker;
  connection_t *conn = task->conn;
  route_handler_t handler = task->handler;
  free(task);

  if (run_with_middleware(conn, &conn->request, &conn->response, handler) != 0 &&
      conn->write_buffer_size == 0)
  {
    send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR, "Handler error");
  }

  // Account and reset while we still exclusively own the connection.
  atomic_fetch_add(&g_server.total_requests, 1);
  atomic_fetch_add(&worker->requests_processed, 1);
  // Leave the read buffer as handle_client_data left it: it may hold the next
  // pipelined request, which the worker picks up once this response flushes.
  conn->last_activity = time(NULL);
  conn->state = CONN_STATE_WRITING_RESPONSE;

  // Snapshot everything the epoll call needs BEFORE releasing ownership. Once
  // we clear `offloaded` and arm EPOLLOUT, the worker may write, fail, and
  // free(conn) at any instant -- so conn must not be dereferenced past here.
  int epoll_fd = worker->epoll_fd;
  int socket_fd = conn->socket_fd;
  struct epoll_event ev;
  ev.events = EPOLLOUT | EPOLLET;
  ev.data.ptr = conn;

  // Release ownership and re-arm under the worker's list lock. The worker's
  // free paths take the same lock and re-check `offloaded`, so it cannot see
  // offloaded=false, free the connection, and close socket_fd in between --
  // which would leave this MOD acting on a stale/reused fd with a dangling
  // pointer stored in the epoll data.
  pthread_mutex_lock(&worker->connections_lock);
  atomic_store(&conn->offloaded, false);
  epoll_ctl(epoll_fd, EPOLL_CTL_MOD, socket_fd, &ev);
  pthread_mutex_unlock(&worker->connections_lock);
}

// Unlink a connection from its owning worker's list.
static void remove_connection_from_worker(worker_thread_t *worker, connection_t *conn)
{
  if (!worker || !conn)
  {
    return;
  }
  if (conn->prev)
  {
    conn->prev->next = conn->next;
  }
  else
  {
    worker->connections = conn->next;
  }
  if (conn->next)
  {
    conn->next->prev = conn->prev;
  }
  conn->next = NULL;
  conn->prev = NULL;
  if (worker->connection_count > 0)
  {
    worker->connection_count--;
  }
}

// Tear a connection down from the worker thread. Guarded by the list lock and a
// final `offloaded` re-check: a storage-pool thread re-arms offloaded
// connections under the same lock, so this never frees (and closes the fd of) a
// connection the pool is still handing back.
static void drop_connection(worker_thread_t *worker, connection_t *conn)
{
  pthread_mutex_lock(&worker->connections_lock);
  if (!atomic_load(&conn->offloaded))
  {
    remove_connection_from_worker(worker, conn);
    free_connection(&g_server, conn);
  }
  pthread_mutex_unlock(&worker->connections_lock);
}

int handle_new_connection(worker_thread_t *worker, int client_fd,
                          struct sockaddr_storage client_addr)
{
  connection_t *conn = allocate_connection(&g_server);
  if (!conn)
  {
    // No connection took ownership of the fd; close it here so the accept loop
    // never double-closes it.
    close(client_fd);
    return -1;
  }

  conn->socket_fd = client_fd;
  // Set client_addr BEFORE publishing the node into the list: once it is
  // linked, the worker thread may inspect (or reap) it concurrently.
  conn->client_addr = client_addr;
  conn->state = CONN_STATE_READING_REQUEST;
  conn->last_activity = time(NULL);
  conn->connection_time = conn->last_activity;

  // Publish into the worker's connection list. The worker thread traverses and
  // unlinks this same list, so the insert must hold the list lock.
  pthread_mutex_lock(&worker->connections_lock);
  conn->next = worker->connections;
  if (worker->connections)
  {
    worker->connections->prev = conn;
  }
  worker->connections = conn;
  worker->connection_count++;
  pthread_mutex_unlock(&worker->connections_lock);

  struct epoll_event event;
  event.events = EPOLLIN | EPOLLET;
  event.data.ptr = conn;

  if (epoll_ctl(worker->epoll_fd, EPOLL_CTL_ADD, client_fd, &event) < 0)
  {
    perror("epoll_ctl");
    pthread_mutex_lock(&worker->connections_lock);
    remove_connection_from_worker(worker, conn);
    pthread_mutex_unlock(&worker->connections_lock);
    // free_connection runs cleanup_connection, which closes client_fd exactly
    // once. The accept loop must not close it again on this -1 return.
    free_connection(&g_server, conn);
    return -1;
  }

  return 0;
}

// Handle one epoll event for a connection, tearing it down on any failure.
static void dispatch_connection_event(worker_thread_t *worker, connection_t *conn,
                                      uint32_t events)
{
  // A storage-pool thread currently owns this connection. Defer every event --
  // including EPOLLERR/EPOLLHUP, which the kernel delivers regardless of the
  // interest mask -- until the pool re-arms it. Touching (or freeing) it here
  // would race the pool thread.
  if (atomic_load(&conn->offloaded))
  {
    return;
  }

  if (events & (EPOLLERR | EPOLLHUP))
  {
    stats_record_error();
    drop_connection(worker, conn);
    return;
  }

  if ((events & EPOLLIN) && handle_client_data(worker, conn) != 0)
  {
    drop_connection(worker, conn);
    return;
  }

  // handle_client_data may have handed this connection to the storage pool (a
  // pipelined request picked up in handle_client_write can do the same), so
  // re-check before touching it again for the write half of a combined
  // EPOLLIN|EPOLLOUT event -- the pool owns it until it re-arms.
  if (atomic_load(&conn->offloaded))
  {
    return;
  }

  if ((events & EPOLLOUT) && handle_client_write(worker, conn) != 0)
  {
    drop_connection(worker, conn);
  }
}

// Close connections idle past the keep-alive timeout. Traversal and unlink run
// under the list lock so this cannot race the accept loop's insert or a pool
// thread's re-arm.
static void reap_idle_connections(worker_thread_t *worker)
{
  pthread_mutex_lock(&worker->connections_lock);

  time_t now = time(NULL);
  connection_t *conn = worker->connections;
  while (conn)
  {
    connection_t *next = conn->next;

    // Never reap a connection a pool thread is still working on.
    if (!atomic_load(&conn->offloaded) &&
        now - conn->last_activity > g_server.keepalive_timeout)
    {
      remove_connection_from_worker(worker, conn);
      free_connection(&g_server, conn);
    }

    conn = next;
  }

  pthread_mutex_unlock(&worker->connections_lock);
}

// Free the worker's connections before the thread exits so the per-connection
// buffers are not leaked on shutdown. A connection a storage-pool thread is
// still running (offloaded) must NOT be freed here -- the pool thread may still
// reference it. Those stay on the list; http_server_cleanup frees them once the
// pool has been destroyed.
static void drain_worker_connections(worker_thread_t *worker)
{
  connection_t *conn = worker->connections;
  connection_t *offloaded_head = NULL;
  int offloaded_count = 0;

  while (conn)
  {
    connection_t *next = conn->next;
    if (atomic_load(&conn->offloaded))
    {
      conn->prev = NULL;
      conn->next = offloaded_head;
      if (offloaded_head)
      {
        offloaded_head->prev = conn;
      }
      offloaded_head = conn;
      offloaded_count++;
    }
    else
    {
      free_connection(&g_server, conn);
    }
    conn = next;
  }

  worker->connections = offloaded_head;
  worker->connection_count = offloaded_count;
}

void *worker_thread_function(void *arg)
{
  worker_thread_t *worker = (worker_thread_t *)arg;
  struct epoll_event events[MAX_EVENTS];

  char thread_name[16];
  snprintf(thread_name, sizeof(thread_name), "http_worker_%d", worker->thread_id);
  pthread_setname_np(pthread_self(), thread_name);

  printf("Worker thread %d started\n", worker->thread_id);

  while (worker->running)
  {
    int event_count = epoll_wait(worker->epoll_fd, events, MAX_EVENTS, 1000);

    if (event_count < 0)
    {
      if (errno == EINTR)
      {
        continue;
      }

      perror("epoll_wait");
      break;
    }

    for (int i = 0; i < event_count; i++)
    {
      connection_t *conn = (connection_t *)events[i].data.ptr;
      if (conn)
      {
        dispatch_connection_event(worker, conn, events[i].events);
      }
    }

    reap_idle_connections(worker);
  }

  drain_worker_connections(worker);

  printf("Worker thread %d stopping\n", worker->thread_id);

  return NULL;
}

// Copy a header value (sans surrounding whitespace) into tmp. Returns -1 when
// it doesn't fit.
static int copy_header_value(const char *v, const char *v_end, char *tmp, size_t tmp_size)
{
  while (v < v_end && (*v == ' ' || *v == '\t'))
  {
    v++;
  }
  while (v_end > v && (v_end[-1] == ' ' || v_end[-1] == '\t'))
  {
    v_end--;
  }
  size_t vl = (size_t)(v_end - v);
  if (vl >= tmp_size)
  {
    return -1;
  }
  memcpy(tmp, v, vl);
  tmp[vl] = '\0';
  return 0;
}

// Decide whether a full request (headers plus any declared body) has arrived in
// the read buffer, without mutating it. The body's framing (Content-Length or
// chunked) is only known after the header terminator, and body bytes can
// straggle in over several reads, so this is checked before every dispatch.
// *expect_100 is set when the headers are complete and carry
// "Expect: 100-continue" -- on a 0 return the caller should send the interim
// response so the client releases the body. On a 1 return *request_len holds
// the byte length of this request, so the caller knows where the next
// pipelined one starts. Returns:
//   1  request fully received
//   0  incomplete, keep reading
//  -1  malformed framing (bad Content-Length, bad chunking, both
//      Content-Length and Transfer-Encoding present, or a NUL in the
//      headers) -> 400
//  -2  unsupported (non-chunked) Transfer-Encoding -> 501
//  -3  body exceeds max_body -> 413
static int check_request_frame(connection_t *conn, size_t max_body,
                               bool *expect_100, size_t *request_len)
{
  const char *buf = conn->read_buffer;
  const size_t avail = conn->read_buffer_pos;

  // Scan by length, not with strstr: a NUL byte anywhere in the request would
  // otherwise hide the header terminator from every later read, leaving the
  // connection to sit until the keep-alive reaper collects it.
  size_t term_len = 4;
  const char *hdr_end = memmem(buf, avail, "\r\n\r\n", 4);
  if (!hdr_end)
  {
    hdr_end = memmem(buf, avail, "\n\n", 2);
    term_len = 2;
  }
  if (!hdr_end)
  {
    return 0; // header terminator not seen yet
  }

  size_t header_len = (size_t)(hdr_end - buf) + term_len;

  // A NUL cannot appear in a request line or header field. Rejecting it
  // outright stops a header from being truncated at the NUL by the
  // string-based parsing below while a proxy in front read the whole value --
  // the classic setup for request smuggling.
  if (memchr(buf, '\0', header_len) != NULL)
  {
    return -1;
  }

  // Walk the header lines (request line first, then fields) looking for the
  // framing headers. Non-mutating so a later real parse still sees an intact
  // buffer.
  size_t content_length = 0;
  bool saw_content_length = false;
  bool chunked = false;
  const char *line = buf;
  while (line < hdr_end)
  {
    const char *nl = memchr(line, '\n', (size_t)(hdr_end - line));
    const char *line_end = nl ? nl : hdr_end;
    size_t line_len = (size_t)(line_end - line);
    if (line_len > 0 && line[line_len - 1] == '\r')
    {
      line_len--;
    }

    const char *colon = memchr(line, ':', line_len);
    if (colon)
    {
      size_t name_len = (size_t)(colon - line);
      const char *v = colon + 1;
      const char *v_end = line + line_len;
      if (name_len == 14 && strncasecmp(line, "Content-Length", 14) == 0)
      {
        char tmp[32];
        if (copy_header_value(v, v_end, tmp, sizeof(tmp)) != 0)
        {
          return -1; // implausibly long value
        }
        if (http_parse_content_length(tmp, &content_length) != 0)
        {
          return -1;
        }
        saw_content_length = true;
      }
      else if (name_len == 17 && strncasecmp(line, "Transfer-Encoding", 17) == 0)
      {
        char tmp[32];
        if (copy_header_value(v, v_end, tmp, sizeof(tmp)) != 0 ||
            strcasecmp(tmp, "chunked") != 0)
        {
          return -2; // only the chunked coding is implemented
        }
        chunked = true;
      }
      else if (name_len == 6 && strncasecmp(line, "Expect", 6) == 0)
      {
        char tmp[32];
        if (copy_header_value(v, v_end, tmp, sizeof(tmp)) == 0 &&
            strcasecmp(tmp, "100-continue") == 0 && expect_100)
        {
          *expect_100 = true;
        }
      }
    }

    if (!nl)
    {
      break;
    }
    line = nl + 1;
  }

  if (chunked)
  {
    // Both framings on one message is a request-smuggling vector; refuse.
    if (saw_content_length)
    {
      return -1;
    }
    size_t decoded = 0;
    size_t chunk_bytes = 0;
    int r = http_chunked_decode(buf + header_len, avail - header_len, NULL,
                                &decoded, &chunk_bytes);
    if (r < 0)
    {
      return -1;
    }
    if (decoded > max_body)
    {
      return -3;
    }
    if (r == 1 && request_len)
    {
      *request_len = header_len + chunk_bytes;
    }
    return r; // 1 = terminal chunk seen, 0 = still arriving
  }

  if (content_length > max_body)
  {
    return -3;
  }
  if (header_len + content_length > avail)
  {
    return 0; // full body not yet received
  }

  if (request_len)
  {
    *request_len = header_len + content_length;
  }
  return 1;
}

// Push the interim "HTTP/1.1 100 Continue" line straight to the socket -- it
// precedes (and is separate from) the buffered final response. Partial-write
// progress is kept in conn->continue_pos so a retry resumes mid-line;
// conn->continue_sent marks completion. Returns 0 on progress (sent or
// would-block), -1 on a fatal socket error.
static int send_100_continue(connection_t *conn)
{
  static const char interim[] = "HTTP/1.1 100 Continue\r\n\r\n";
  const size_t len = sizeof(interim) - 1;

  while (conn->continue_pos < len)
  {
    size_t sent;
    io_result_t r = conn_send(conn, interim + conn->continue_pos,
                              len - conn->continue_pos, &sent);
    if (r == IO_WOULD_BLOCK)
    {
      return 0;
    }
    if (r != IO_OK)
    {
      return -1;
    }
    conn->continue_pos += sent;
    stats_record_bytes_sent((uint64_t)sent);
  }

  conn->continue_sent = true;
  return 0;
}

// Drive an established WebSocket connection: drain readable bytes, decode the
// buffered frames, and stage the server's replies. Returns 0 to keep the
// connection (armed for read or write as appropriate) or -1 to tear it down.
static int handle_websocket_data(worker_thread_t *worker, connection_t *conn)
{
  bool should_close = false;

  // Read/decode in passes so an edge-triggered socket is fully drained even if
  // the read buffer fills mid-way (decoding frees space by consuming frames).
  for (;;)
  {
    bool drained = false;

    for (;;)
    {
      if (conn->read_buffer_pos >= conn->read_buffer_cap - 1)
      {
        break; // buffer full; decode below frees room (or flags a close)
      }

      size_t bytes_read;
      io_result_t r = conn_recv(conn, conn->read_buffer + conn->read_buffer_pos,
                                conn->read_buffer_cap - conn->read_buffer_pos - 1,
                                &bytes_read);
      if (r == IO_WOULD_BLOCK)
      {
        drained = true;
        break;
      }
      if (r != IO_OK)
      {
        return -1;
      }

      conn->read_buffer_pos += bytes_read;
      worker->bytes_received += (uint64_t)bytes_read;
      stats_record_bytes_received((uint64_t)bytes_read);
      conn->last_activity = time(NULL);
    }

    ws_process_frames(conn, &should_close);
    if (should_close)
    {
      break;
    }
    if (drained)
    {
      break;
    }
    // Buffer filled: if decoding could not free any room the pending frame is
    // wedged (ws_process_frames emits a Close in that case), so stop. Otherwise
    // loop back and keep reading the still-readable socket.
    if (conn->read_buffer_pos >= conn->read_buffer_cap - 1)
    {
      should_close = true;
      break;
    }
  }

  // Flush any staged reply frames (echoes/pongs/close) by arming EPOLLOUT.
  if (conn->write_buffer_size > 0)
  {
    conn->write_buffer_pos = 0;
    conn->websocket_closing = should_close;
    conn->state = CONN_STATE_WRITING_RESPONSE;

    return arm_socket(worker, conn, EPOLLOUT | EPOLLET) < 0 ? -1 : 0;
  }

  // Nothing to send: either wait for more data, or tear down on a bare close.
  return should_close ? -1 : 0;
}

// Drain everything currently readable on the socket into the read buffer.
// Edge-triggered epoll, so we must keep reading until it would block. Sets
// *buffer_full when the configured request ceiling was reached with bytes
// still pending. Returns 0 to carry on, -1 to tear the connection down.
static int drain_socket_reads(worker_thread_t *worker, connection_t *conn,
                              bool *buffer_full)
{
  *buffer_full = false;

  for (;;)
  {
    if (conn->read_buffer_pos >= conn->read_buffer_cap - 1)
    {
      // Try to grow first: the read buffer starts small and is allowed to
      // reach max_request_size, so the configured limit is the real limit
      // rather than whatever the initial buffer happened to be.
      if (!conn_reserve_read(conn, conn->read_buffer_cap * 2,
                             g_server.max_request_size))
      {
        // At the configured ceiling. That is only an error if no complete
        // request is framed in what we already have -- a buffer packed with
        // pipelined requests is perfectly legal, and the leftovers make room
        // as each one is consumed.
        *buffer_full = true;
        return 0;
      }
    }

    size_t bytes_read;
    io_result_t r = conn_recv(conn, conn->read_buffer + conn->read_buffer_pos,
                              conn->read_buffer_cap - conn->read_buffer_pos - 1,
                              &bytes_read);
    if (r == IO_WOULD_BLOCK)
    {
      return 0;
    }
    if (r != IO_OK)
    {
      return -1;
    }

    conn->read_buffer_pos += bytes_read;
    conn->read_buffer[conn->read_buffer_pos] = '\0';
    worker->bytes_received += (uint64_t)bytes_read;
    stats_record_bytes_received((uint64_t)bytes_read);
    conn->last_activity = time(NULL);
  }
}

// Pick the handler for a request and report whether it blocks. A matched route
// offloads per its own flag; an unmatched request falls to the default file
// handler, which hits the disk (stat + read), so it is treated as blocking too.
static route_handler_t resolve_handler(const http_request_t *request, bool *offload)
{
  route_t *route = find_route(&g_server, request->uri, request->method);
  if (route && route->handler)
  {
    *offload = route->offload;
    return route->handler;
  }
  *offload = true;
  return g_server.default_handler;
}

// Hand a blocking handler to the storage pool so a WAL fsync or page/file read
// cannot freeze this worker's event loop. Returns true once the pool owns the
// connection -- the caller must not touch it again -- and false to run inline.
static bool dispatch_offloaded(worker_thread_t *worker, connection_t *conn,
                               route_handler_t handler)
{
  offload_task_t *task = malloc(sizeof(*task));
  if (!task)
  {
    return false; // a malloc failure just falls through to inline handling
  }
  task->worker = worker;
  task->conn = conn;
  task->handler = handler;

  // Take the connection off the interest set (no EPOLLIN/OUT while the pool
  // owns it) and flag it so the event loop and timeout sweep leave it alone.
  // Set before submit so the pool's re-arm is the only clear.
  atomic_store(&conn->offloaded, true);
  arm_socket(worker, conn, 0);

  if (thread_pool_submit(g_server.pool, run_offloaded_handler, task,
                         request_priority(&conn->request)) == 0)
  {
    return true;
  }

  // Submit failed -- reclaim the connection and fall back to inline.
  atomic_store(&conn->offloaded, false);
  free(task);
  return false;
}

int handle_client_data(worker_thread_t *worker, connection_t *conn)
{
  if (!worker || !conn)
  {
    return -1;
  }

  // A connection that has just been accepted, or that was stripped back while
  // idle, gets its read buffer and header tables here.
  if (!conn_ensure_buffers(conn))
  {
    return -1;
  }

  // Once the handshake has switched this connection to WebSocket framing, all
  // further inbound bytes are frames, not HTTP requests.
  if (conn->state == CONN_STATE_WEBSOCKET)
  {
    return handle_websocket_data(worker, conn);
  }

  bool buffer_full = false;
  if (drain_socket_reads(worker, conn, &buffer_full) != 0)
  {
    return -1;
  }

  // A trusted PROXY header precedes the first request on the connection and
  // replaces the transport-level peer, so it must be consumed before any
  // framing or routing happens.
  if (g_server.trust_proxy_protocol && !conn->proxy_header_done)
  {
    int pr = consume_proxy_header(conn);
    if (pr < 0)
    {
      return -1; // no usable header from a peer we require one from
    }
    if (pr == 0)
    {
      return 0; // header still arriving
    }
    conn->proxy_header_done = true;
  }

  // Wait until the full request -- headers plus any declared body -- has
  // arrived before dispatching. A body can straggle in over several reads.
  bool expect_100 = false;
  size_t request_len = 0;
  int frame =
      check_request_frame(conn, g_server.max_request_size, &expect_100, &request_len);
  if (frame == 0)
  {
    if (buffer_full)
    {
      // Nothing complete and nowhere left to put more: the request genuinely
      // does not fit.
      return reject_request(worker, conn, HTTP_PAYLOAD_TOO_LARGE, "Request too large");
    }
    // Headers complete but the body still in flight: honor Expect:
    // 100-continue (once) so a waiting client releases the body.
    if (expect_100 && !conn->continue_sent && send_100_continue(conn) != 0)
    {
      return -1;
    }
    return 0; // need more data
  }
  if (frame == -2)
  {
    return reject_request(worker, conn, HTTP_NOT_IMPLEMENTED,
                          "Unsupported Transfer-Encoding");
  }
  if (frame == -3)
  {
    return reject_request(worker, conn, HTTP_PAYLOAD_TOO_LARGE, "Request too large");
  }
  if (frame < 0)
  {
    return reject_request(worker, conn, HTTP_BAD_REQUEST, "Bad Request");
  }

  // A half-written interim "100 Continue" must be flushed before any final
  // response bytes, or the two would interleave mid-line. If the socket still
  // can't take 25 bytes, the connection is wedged -- drop it.
  if (conn->continue_pos > 0 && !conn->continue_sent)
  {
    if (send_100_continue(conn) != 0 || !conn->continue_sent)
    {
      return -1;
    }
  }

  conn->state = CONN_STATE_PROCESSING;

  int parse_result = parse_http_request(conn, &conn->request, request_len);
  if (parse_result != 0)
  {
    // parse_http_request returns -2 for an internal failure (e.g. body malloc)
    // and -1 for a malformed request.
    return parse_result == -2
               ? reject_request(worker, conn, HTTP_INTERNAL_SERVER_ERROR,
                                "Internal Server Error")
               : reject_request(worker, conn, HTTP_BAD_REQUEST, "Bad Request");
  }

  // The parse copied everything it needs out of the read buffer, so the bytes
  // of this request can go now. Whatever follows is the next pipelined request:
  // slide it to the front and keep it. Discarding it here (as this code used to)
  // silently dropped the request -- and because epoll is edge-triggered and no
  // new bytes were coming, the connection then sat idle until it timed out.
  conn_consume_read(conn, request_len);

  bool offload = false;
  route_handler_t handler = resolve_handler(&conn->request, &offload);

  // Gate on g_server.running: once shutdown starts, http_server_cleanup tears
  // the pool down, so we must stop submitting and run inline instead.
  if (handler && offload && g_server.pool && g_server.running &&
      dispatch_offloaded(worker, conn, handler))
  {
    return 0; // handled asynchronously; the pool arms EPOLLOUT when done
  }

  if (handler)
  {
    if (run_with_middleware(conn, &conn->request, &conn->response, handler) != 0 &&
        conn->write_buffer_size == 0)
    {
      // The handler reported an error without preparing a response of its own.
      send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR, "Handler error");
    }
  }
  else
  {
    send_error_response(conn, HTTP_NOT_FOUND, "Not Found");
  }

  return finalize_response(worker, conn);
}

// Push the staged response bytes to the socket. Edge-triggered, so write until
// the buffer is empty or the socket would block. Returns 0 when fully flushed,
// 1 when it would block (a later EPOLLOUT resumes), -1 on a fatal error.
static int flush_write_buffer(worker_thread_t *worker, connection_t *conn)
{
  while (conn->write_buffer_pos < conn->write_buffer_size)
  {
    size_t written;
    io_result_t r = conn_send(conn, conn->write_buffer + conn->write_buffer_pos,
                              conn->write_buffer_size - conn->write_buffer_pos,
                              &written);
    if (r == IO_WOULD_BLOCK)
    {
      return 1;
    }
    if (r != IO_OK)
    {
      return -1;
    }

    conn->write_buffer_pos += written;
    worker->bytes_sent += (uint64_t)written;
    stats_record_bytes_sent((uint64_t)written);
  }
  return 0;
}

// Stream a queued static file body straight from its fd to the socket with
// sendfile(2) -- plaintext connections only, set up in send_file_response.
// Edge-triggered, so pump until the file is drained or the socket would block;
// a later EPOLLOUT resumes from the saved offset. Returns 0 once the file is
// done and its fd released, 1 when it would block, -1 on a fatal error.
static int stream_queued_file(worker_thread_t *worker, connection_t *conn)
{
  while (conn->file_offset < (off_t)conn->file_size)
  {
    ssize_t sent = sendfile(conn->socket_fd, conn->file_fd, &conn->file_offset,
                            conn->file_size - (size_t)conn->file_offset);
    if (sent < 0)
    {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
      {
        return 1; // socket buffer full; wait for the next EPOLLOUT
      }
      if (errno == EINTR)
      {
        continue;
      }
      return -1;
    }
    if (sent == 0)
    {
      break; // file shrank under us; stop rather than spin
    }
    worker->bytes_sent += (uint64_t)sent;
    stats_record_bytes_sent((uint64_t)sent);
  }

  close(conn->file_fd);
  conn->file_fd = -1;
  conn->file_offset = 0;
  conn->file_size = 0;
  return 0;
}

// Clear the per-request state so this connection can carry another request.
// read_buffer_pos is preserved: it counts the bytes of an already-received
// pipelined request.
static void reset_for_next_request(connection_t *conn)
{
  conn->state = CONN_STATE_READING_REQUEST;
  conn->write_buffer_pos = 0;
  conn->write_buffer_size = 0;
  conn->continue_pos = 0;
  conn->continue_sent = false;

  free(conn->request.body);
  free(conn->response.body);

  // The header tables are heap-allocated; carry the pointers across the reset
  // so they are not leaked by the memset.
  http_header_t *req_headers = conn->request.headers;
  http_header_t *resp_headers = conn->response.headers;
  memset(&conn->request, 0, sizeof(http_request_t));
  memset(&conn->response, 0, sizeof(http_response_t));
  conn->request.headers = req_headers;
  conn->response.headers = resp_headers;

  // Nothing is pending on an idle keep-alive connection, so hand back the
  // write buffer and header tables until the next request needs them.
  if (conn->read_buffer_pos == 0)
  {
    conn_release_idle(conn);
  }
}

int handle_client_write(worker_thread_t *worker, connection_t *conn)
{
  if (!conn || conn->state != CONN_STATE_WRITING_RESPONSE)
  {
    return -1;
  }

  int flushed = flush_write_buffer(worker, conn);
  if (flushed != 0)
  {
    return flushed < 0 ? -1 : 0;
  }

  // Headers are out; a queued static file body still has to follow before the
  // response counts as sent.
  if (conn->file_fd >= 0)
  {
    int streamed = stream_queued_file(worker, conn);
    if (streamed != 0)
    {
      return streamed < 0 ? -1 : 0;
    }
  }

  // A WebSocket connection (the 101 handshake or a subsequent reply frame just
  // finished flushing): either close after a staged Close, or return to frame-
  // read mode. Do NOT touch read_buffer_pos here -- it may already hold the
  // start of the next frame.
  if (conn->websocket_handshake_complete)
  {
    if (conn->websocket_closing)
    {
      return -1; // Close frame flushed; tear the connection down.
    }

    conn->state = CONN_STATE_WEBSOCKET;
    conn->write_buffer_pos = 0;
    conn->write_buffer_size = 0;
    return arm_socket(worker, conn, EPOLLIN | EPOLLET) < 0 ? -1 : 0;
  }

  if (!conn->request.keep_alive || !g_server.enable_keepalive)
  {
    return -1;
  }

  reset_for_next_request(conn);

  if (arm_socket(worker, conn, EPOLLIN | EPOLLET) < 0)
  {
    return -1;
  }

  // A pipelined request is already sitting in the buffer. The socket is
  // edge-triggered and those bytes arrived with the request we just answered,
  // so no further EPOLLIN will announce them -- dispatch it here instead.
  // handle_client_data re-arms EPOLLOUT for its response, which drives the
  // next round; the chain is one call deep, not recursive.
  if (conn->read_buffer_pos > 0)
  {
    return handle_client_data(worker, conn);
  }

  return 0;
}
