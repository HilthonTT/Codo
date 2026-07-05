#define _GNU_SOURCE

#include <errno.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
  route_t *route;
} offload_task_t;

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
  route_t *route = task->route;
  free(task);

  if (route->handler(conn, &conn->request, &conn->response) != 0 &&
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

  atomic_store(&conn->offloaded, false);
  epoll_ctl(epoll_fd, EPOLL_CTL_MOD, socket_fd, &ev);
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

int handle_new_connection(worker_thread_t *worker, int client_fd)
{
  // Allocate connection structure
  connection_t *conn = allocate_connection(&g_server);
  if (!conn)
  {
    return -1;
  }

  conn->socket_fd = client_fd;
  conn->state = CONN_STATE_READING_REQUEST;
  conn->last_activity = time(NULL);
  conn->connection_time = conn->last_activity;

  // Add to worker's connection list
  conn->next = worker->connections;
  if (worker->connections)
  {
    worker->connections->prev = conn;
  }

  worker->connections = conn;
  worker->connection_count++;

  // Add to epoll
  struct epoll_event event;
  event.events = EPOLLIN | EPOLLET;
  event.data.ptr = conn;

  if (epoll_ctl(worker->epoll_fd, EPOLL_CTL_ADD, client_fd, &event) < 0)
  {
    perror("epoll_ctl");
    remove_connection_from_worker(worker, conn);
    cleanup_connection(conn);
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
        remove_connection_from_worker(worker, conn);
        free_connection(&g_server, conn);
        continue;
      }

      if (event.events & EPOLLIN)
      {
        // Data available for reading
        if (handle_client_data(worker, conn) != 0)
        {
          remove_connection_from_worker(worker, conn);
          free_connection(&g_server, conn);
          continue;
        }
      }

      if (event.events & EPOLLOUT)
      {
        if (handle_client_write(worker, conn) != 0)
        {
          remove_connection_from_worker(worker, conn);
          free_connection(&g_server, conn);
          continue;
        }
      }
    }

    // Check for connection timeouts
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
  }

  // Drain the worker's connection list before the thread exits so we don't
  // leak the per-connection buffers on shutdown.
  connection_t *conn = worker->connections;
  while (conn)
  {
    connection_t *next = conn->next;
    free_connection(&g_server, conn);
    conn = next;
  }
  worker->connections = NULL;
  worker->connection_count = 0;

  printf("Worker thread %d stopping\n", worker->thread_id);

  return NULL;
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

  // Need at least the end-of-headers marker before we attempt a parse.
  if (!strstr(conn->read_buffer, "\r\n\r\n") && !strstr(conn->read_buffer, "\n\n"))
  {
    return 0; // wait for more data
  }

  conn->state = CONN_STATE_PROCESSING;

  int parse_result = parse_http_request(conn, &conn->request);
  if (parse_result != 0)
  {
    send_error_response(conn, HTTP_BAD_REQUEST, "Bad Request");
    return finalize_response(worker, conn);
  }

  route_t *route = find_route(&g_server, conn->request.uri, conn->request.method);

  // Blocking, storage-backed handlers are dispatched to the thread pool so a
  // WAL fsync or page read/write can't freeze this worker's whole event loop.
  // Gate on g_server.running: once shutdown starts, http_server_cleanup tears
  // the pool down, so we must stop submitting and run inline instead.
  if (route && route->handler && route->offload && g_server.pool && g_server.running)
  {
    offload_task_t *task = malloc(sizeof(*task));
    if (task)
    {
      task->worker = worker;
      task->conn = conn;
      task->route = route;

      // Take the connection off the interest set (no EPOLLIN/OUT while the pool
      // owns it) and flag it so the event loop and timeout sweep leave it
      // alone. Set before submit so the pool's re-arm is the only clear.
      atomic_store(&conn->offloaded, true);
      struct epoll_event off_ev;
      off_ev.events = 0;
      off_ev.data.ptr = conn;
      epoll_ctl(worker->epoll_fd, EPOLL_CTL_MOD, conn->socket_fd, &off_ev);

      if (thread_pool_submit(g_server.pool, run_offloaded_handler, task, 0) == 0)
      {
        return 0; // handled asynchronously; pool arms EPOLLOUT when done
      }

      // Submit failed -- reclaim the connection and fall through to inline.
      atomic_store(&conn->offloaded, false);
      free(task);
    }
    // A malloc failure likewise falls through to inline handling.
  }

  if (route && route->handler)
  {
    if (route->handler(conn, &conn->request, &conn->response) != 0)
    {
      // Handler reported an error; if it didn't already prepare a response,
      // send a generic one.
      if (conn->write_buffer_size == 0)
      {
        send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR, "Handler error");
      }
    }
  }
  else if (g_server.default_handler)
  {
    g_server.default_handler(conn, &conn->request, &conn->response);
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
