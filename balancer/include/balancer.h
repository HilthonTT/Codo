#ifndef BALANCER_H
#define BALANCER_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <errno.h>
#include <stdbool.h>

#include <time.h>
#include <stdint.h>

#define MAX_EVENTS 1000
#define BUFFER_SIZE 8192
#define MAX_BACKENDS 100

// Passive health check tuning. A backend is marked unhealthy after this many
// consecutive failures (connect refused, read/write errors, EPOLLERR/HUP).
// It is re-admitted after HEALTH_RECOVERY_SECS have passed since the last
// failure, giving it a chance to prove itself again.
#define HEALTH_FAILURE_THRESHOLD 3
#define HEALTH_RECOVERY_SECS 30

typedef struct
{
  int fd;
  struct sockaddr_in addr;
  int current_weight;
  int weight;
  int current_connections;
  int total_requests;
  int failed_requests;
  int consecutive_failures;
  long long avg_response_time;
  int health_status;
  time_t last_health_check;
} backend_t;

// Which side of a proxied connection a file descriptor belongs to. The listen
// socket and both ends of every connection register an io_ctx_t as their epoll
// data.ptr, so epoll_wait tells us exactly which fd fired and on which conn.
typedef enum
{
  IO_LISTEN,
  IO_CLIENT,
  IO_BACKEND
} io_role_t;

struct connection;

typedef struct
{
  int fd;
  io_role_t role;
  struct connection *conn; // NULL for the listen socket
} io_ctx_t;

typedef struct
{
  int epoll_fd;
  int listen_fd;
  io_ctx_t listen_ctx;
  backend_t backends[MAX_BACKENDS];
  int backend_count;
  int current_backend;
  pthread_mutex_t backend_mutex;
  struct epoll_event events[MAX_EVENTS];
} load_balancer_t;

typedef struct connection
{
  int client_fd;
  int backend_fd;
  backend_t *backend;
  io_ctx_t client_ctx;
  io_ctx_t backend_ctx;
  // client_buffer holds bytes travelling client -> backend; backend_buffer
  // holds bytes travelling backend -> client. *_sent tracks how much of the
  // current chunk has already been flushed when a write is only partial.
  char client_buffer[BUFFER_SIZE];
  char backend_buffer[BUFFER_SIZE];
  size_t client_buffer_len;
  size_t client_buffer_sent;
  size_t backend_buffer_len;
  size_t backend_buffer_sent;
  struct timespec start_time;
} connection_t;

// Lifecycle: bind/listen on `port`, create the epoll instance and register the
// listen socket. Backends are added afterwards with add_backend().
int load_balancer_init(load_balancer_t *lb, int port);
int add_backend(load_balancer_t *lb, const char *host, int port, int weight);

// High-performance load balancer main loop
int load_balancer_main_loop(load_balancer_t *lb);

// Handle new client connection
void handle_new_connection(load_balancer_t *lb);

backend_t *select_backend(load_balancer_t *lb);

void handle_client_data(load_balancer_t *lb, connection_t *conn);

void handle_backend_data(load_balancer_t *lb, connection_t *conn);

void close_connection(load_balancer_t *lb, connection_t *conn);

// Socket / epoll helpers
int set_nonblocking(int fd);
int create_backend_connection(backend_t *backend);
int add_connection_to_epoll(load_balancer_t *lb, connection_t *conn);
int modify_epoll_events(load_balancer_t *lb, io_ctx_t *ctx, uint32_t events);

// Write-side and error handlers
void handle_client_write(load_balancer_t *lb, connection_t *conn);
void handle_backend_write(load_balancer_t *lb, connection_t *conn);
void handle_connection_error(load_balancer_t *lb, connection_t *conn);

#endif
