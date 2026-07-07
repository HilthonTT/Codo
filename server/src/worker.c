#define _GNU_SOURCE

#include <errno.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "connection.h"
#include "http_protocol.h"
#include "http_types.h"
#include "route.h"
#include "server.h"
#include "stats.h"
#include "thread_pool.h"
#include "worker.h"

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

  conn->read_buffer_pos = 0;
  conn->read_buffer[0] = '\0';
  conn->state = CONN_STATE_WRITING_RESPONSE;

  struct epoll_event ev;
  ev.events = EPOLLOUT | EPOLLET;
  ev.data.ptr = conn;
  return epoll_ctl(worker->epoll_fd, EPOLL_CTL_MOD, conn->socket_fd, &ev);
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
  conn->read_buffer_pos = 0;
  conn->read_buffer[0] = '\0';
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
                          struct sockaddr_in client_addr)
{
  // Allocate connection structure
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

  // Add to epoll
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

void *worker_thread_function(void *arg)
{
  worker_thread_t *worker = (worker_thread_t *)arg;
  struct epoll_event events[MAX_EVENTS];

  // Set thread name
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
      struct epoll_event event = events[i];
      connection_t *conn = (connection_t *)event.data.ptr;
      if (!conn)
      {
        continue;
      }

      // A storage-pool thread currently owns this connection. Defer every
      // event -- including EPOLLERR/EPOLLHUP, which the kernel delivers
      // regardless of the interest mask -- until the pool re-arms it. Touching
      // (or freeing) it here would race the pool thread.
      if (atomic_load(&conn->offloaded))
      {
        continue;
      }

      if (event.events & (EPOLLERR | EPOLLHUP))
      {
        // Connection error or hangup
        stats_record_error();
        drop_connection(worker, conn);
        continue;
      }

      if (event.events & EPOLLIN)
      {
        // Data available for reading
        if (handle_client_data(worker, conn) != 0)
        {
          drop_connection(worker, conn);
          continue;
        }
      }

      if (event.events & EPOLLOUT)
      {
        if (handle_client_write(worker, conn) != 0)
        {
          drop_connection(worker, conn);
          continue;
        }
      }
    }

    // Check for connection timeouts. Traversal + unlink under the list lock so
    // it can't race the accept loop's insert or a pool thread's re-arm.
    pthread_mutex_lock(&worker->connections_lock);
    time_t current_time = time(NULL);
    connection_t *conn = worker->connections;

    while (conn)
    {
      connection_t *next = conn->next;

      // Never reap a connection a pool thread is still working on.
      if (!atomic_load(&conn->offloaded) &&
          current_time - conn->last_activity > g_server.keepalive_timeout)
      {
        remove_connection_from_worker(worker, conn);
        free_connection(&g_server, conn);
      }

      conn = next;
    }
    pthread_mutex_unlock(&worker->connections_lock);
  }

  // Drain the worker's connection list before the thread exits so we don't
  // leak the per-connection buffers on shutdown. A connection a storage-pool
  // thread is still running (offloaded) must NOT be freed here -- the pool
  // thread may still reference it. Leave those linked; http_server_cleanup
  // frees them once the pool has been destroyed.
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

  printf("Worker thread %d stopping\n", worker->thread_id);

  return NULL;
}

// Decide whether a full request (headers plus any declared body) has arrived in
// the read buffer, without mutating it. Content-Length may only be known after
// the header terminator, and body bytes can straggle in over several reads, so
// this is checked before every dispatch. Returns:
//   1  request fully received
//   0  incomplete, keep reading
//  -1  malformed Content-Length -> 400
//  -2  Transfer-Encoding present (chunked unsupported) -> 501
//  -3  declared body exceeds max_body -> 413
static int check_request_frame(connection_t *conn, size_t max_body)
{
  const char *buf = conn->read_buffer;

  size_t term_len = 4;
  const char *hdr_end = strstr(buf, "\r\n\r\n");
  if (!hdr_end)
  {
    hdr_end = strstr(buf, "\n\n");
    term_len = 2;
  }
  if (!hdr_end)
  {
    return 0; // header terminator not seen yet
  }

  size_t header_len = (size_t)(hdr_end - buf) + term_len;

  // Walk the header lines (request line first, then fields) looking for
  // Content-Length / Transfer-Encoding. Non-mutating so a later real parse
  // still sees an intact buffer.
  size_t content_length = 0;
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
      if (name_len == 14 && strncasecmp(line, "Content-Length", 14) == 0)
      {
        const char *v = colon + 1;
        const char *v_end = line + line_len;
        char tmp[32];
        size_t vl = (size_t)(v_end - v);
        if (vl >= sizeof(tmp))
        {
          return -1; // implausibly long value
        }
        memcpy(tmp, v, vl);
        tmp[vl] = '\0';
        if (http_parse_content_length(tmp, &content_length) != 0)
        {
          return -1;
        }
      }
      else if (name_len == 17 && strncasecmp(line, "Transfer-Encoding", 17) == 0)
      {
        return -2; // chunked (or any coding) decoding is not implemented
      }
    }

    if (!nl)
    {
      break;
    }
    line = nl + 1;
  }

  if (content_length > max_body)
  {
    return -3;
  }
  if (header_len + content_length > conn->read_buffer_pos)
  {
    return 0; // full body not yet received
  }

  return 1;
}

int handle_client_data(worker_thread_t *worker, connection_t *conn)
{
  if (!worker || !conn)
  {
    return -1;
  }

  // Drain everything currently readable on the socket into the read buffer.
  // We're using edge-triggered epoll, so we must keep reading until EAGAIN.
  for (;;)
  {
    if (conn->read_buffer_pos >= sizeof(conn->read_buffer) - 1)
    {
      // Buffer full -- request larger than we can handle.
      send_error_response(conn, HTTP_PAYLOAD_TOO_LARGE, "Request too large");
      struct epoll_event ev;
      ev.events = EPOLLOUT | EPOLLET;
      ev.data.ptr = conn;
      epoll_ctl(worker->epoll_fd, EPOLL_CTL_MOD, conn->socket_fd, &ev);
      return 0;
    }

    ssize_t bytes_read;
    if (conn->ssl_enabled && conn->ssl)
    {
      bytes_read = SSL_read(conn->ssl,
                            conn->read_buffer + conn->read_buffer_pos,
                            (int)(sizeof(conn->read_buffer) - conn->read_buffer_pos - 1));
      if (bytes_read <= 0)
      {
        int ssl_error = SSL_get_error(conn->ssl, (int)bytes_read);
        if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE)
        {
          break;
        }
        return -1;
      }
    }
    else
    {
      bytes_read = read(conn->socket_fd,
                        conn->read_buffer + conn->read_buffer_pos,
                        sizeof(conn->read_buffer) - conn->read_buffer_pos - 1);
      if (bytes_read == 0)
      {
        // Peer closed.
        return -1;
      }
      if (bytes_read < 0)
      {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
          break;
        }
        if (errno == EINTR)
        {
          continue;
        }
        return -1;
      }
    }

    conn->read_buffer_pos += (size_t)bytes_read;
    conn->read_buffer[conn->read_buffer_pos] = '\0';
    worker->bytes_received += (uint64_t)bytes_read;
    stats_record_bytes_received((uint64_t)bytes_read);
    conn->last_activity = time(NULL);
  }

  // Wait until the full request -- headers plus any Content-Length body -- has
  // arrived before dispatching. A body can straggle in over several reads.
  int frame = check_request_frame(conn, g_server.max_request_size);
  if (frame == 0)
  {
    return 0; // need more data
  }
  if (frame == -2)
  {
    send_error_response(conn, HTTP_NOT_IMPLEMENTED, "Transfer-Encoding not supported");
    return finalize_response(worker, conn);
  }
  if (frame == -3)
  {
    send_error_response(conn, HTTP_PAYLOAD_TOO_LARGE, "Request too large");
    return finalize_response(worker, conn);
  }
  if (frame < 0)
  {
    send_error_response(conn, HTTP_BAD_REQUEST, "Bad Request");
    return finalize_response(worker, conn);
  }

  conn->state = CONN_STATE_PROCESSING;

  int parse_result = parse_http_request(conn, &conn->request);
  if (parse_result != 0)
  {
    // parse_http_request returns -2 for an internal failure (e.g. body malloc)
    // and -1 for a malformed request.
    http_status_t status =
        (parse_result == -2) ? HTTP_INTERNAL_SERVER_ERROR : HTTP_BAD_REQUEST;
    send_error_response(conn, status,
                        (parse_result == -2) ? "Internal Server Error" : "Bad Request");
    return finalize_response(worker, conn);
  }

  route_t *route = find_route(&g_server, conn->request.uri, conn->request.method);

  // Pick the handler and decide whether it blocks. A matched route offloads per
  // its own flag; an unmatched request falls to the default file handler, which
  // hits the disk (stat + read), so it's treated as blocking and offloaded too.
  route_handler_t handler = NULL;
  bool offload = false;
  if (route && route->handler)
  {
    handler = route->handler;
    offload = route->offload;
  }
  else if (g_server.default_handler)
  {
    handler = g_server.default_handler;
    offload = true;
  }

  // Blocking handlers are dispatched to the thread pool so a WAL fsync or page/
  // file read can't freeze this worker's whole event loop. Gate on
  // g_server.running: once shutdown starts, http_server_cleanup tears the pool
  // down, so we must stop submitting and run inline instead.
  if (handler && offload && g_server.pool && g_server.running)
  {
    offload_task_t *task = malloc(sizeof(*task));
    if (task)
    {
      task->worker = worker;
      task->conn = conn;
      task->handler = handler;

      // Take the connection off the interest set (no EPOLLIN/OUT while the pool
      // owns it) and flag it so the event loop and timeout sweep leave it
      // alone. Set before submit so the pool's re-arm is the only clear.
      atomic_store(&conn->offloaded, true);
      struct epoll_event off_ev;
      off_ev.events = 0;
      off_ev.data.ptr = conn;
      epoll_ctl(worker->epoll_fd, EPOLL_CTL_MOD, conn->socket_fd, &off_ev);

      if (thread_pool_submit(g_server.pool, run_offloaded_handler, task,
                             request_priority(&conn->request)) == 0)
      {
        return 0; // handled asynchronously; pool arms EPOLLOUT when done
      }

      // Submit failed -- reclaim the connection and fall through to inline.
      atomic_store(&conn->offloaded, false);
      free(task);
    }
    // A malloc failure likewise falls through to inline handling.
  }

  if (handler)
  {
    if (run_with_middleware(conn, &conn->request, &conn->response, handler) != 0)
    {
      // Handler reported an error; if it didn't already prepare a response,
      // send a generic one.
      if (conn->write_buffer_size == 0)
      {
        send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR, "Handler error");
      }
    }
  }
  else
  {
    send_error_response(conn, HTTP_NOT_FOUND, "Not Found");
  }

  return finalize_response(worker, conn);
}

int handle_client_write(worker_thread_t *worker, connection_t *conn)
{
  if (!conn || conn->state != CONN_STATE_WRITING_RESPONSE)
  {
    return -1;
  }

  // Edge-triggered: write until we'd block.
  while (conn->write_buffer_pos < conn->write_buffer_size)
  {
    ssize_t bytes_written;

    if (conn->ssl_enabled && conn->ssl)
    {
      // SSL write
      bytes_written = SSL_write(conn->ssl,
                                conn->write_buffer + conn->write_buffer_pos,
                                (int)(conn->write_buffer_size - conn->write_buffer_pos));

      if (bytes_written <= 0)
      {
        int ssl_error = SSL_get_error(conn->ssl, (int)bytes_written);
        if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE)
        {
          return 0; // Would block
        }
        return -1; // Error
      }
    }
    else
    {
      // Regular write
      bytes_written = write(
          conn->socket_fd,
          conn->write_buffer + conn->write_buffer_pos,
          conn->write_buffer_size - conn->write_buffer_pos);

      if (bytes_written < 0)
      {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
          return 0; // Would block
        }
        if (errno == EINTR)
        {
          continue;
        }
        return -1; // Error
      }
      if (bytes_written == 0)
      {
        return -1;
      }
    }

    conn->write_buffer_pos += (size_t)bytes_written;
    worker->bytes_sent += (uint64_t)bytes_written;
    stats_record_bytes_sent((uint64_t)bytes_written);
  }

  // Response completely sent.
  if (conn->request.keep_alive && g_server.enable_keepalive)
  {
    // Reset for next request on this connection.
    conn->state = CONN_STATE_READING_REQUEST;
    conn->read_buffer_pos = 0;
    conn->write_buffer_pos = 0;
    conn->write_buffer_size = 0;

    if (conn->request.body)
    {
      free(conn->request.body);
    }
    if (conn->response.body)
    {
      free(conn->response.body);
    }
    memset(&conn->request, 0, sizeof(http_request_t));
    memset(&conn->response, 0, sizeof(http_response_t));

    // Re-arm for reads.
    struct epoll_event event;
    event.events = EPOLLIN | EPOLLET;
    event.data.ptr = conn;
    if (epoll_ctl(worker->epoll_fd, EPOLL_CTL_MOD, conn->socket_fd, &event) < 0)
    {
      return -1;
    }
  }
  else
  {
    // Close connection (caller will tear it down).
    return -1;
  }

  return 0;
}
