#ifndef LB_TYPES_H
#define LB_TYPES_H

// Core data types of the load balancer. Names carry an lb_/LB_ prefix where a
// bare name would collide with the server side of the workspace (the server
// has its own connection_t in common/include/connection.h and its own
// MAX_EVENTS / BUFFER_SIZE in common/include/config.h).

#include <netinet/in.h>
#include <pthread.h>
#include <stddef.h>
#include <sys/epoll.h>
#include <time.h>

#define LB_MAX_EVENTS 1000
#define LB_BUFFER_SIZE 8192
#define MAX_BACKENDS 100

typedef enum
{
  LB_STRATEGY_ROUND_ROBIN,
  LB_STRATEGY_LEAST_CONN,
  LB_STRATEGY_IP_HASH,
  LB_STRATEGY_RANDOM,
} lb_strategy_t;

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

struct lb_connection;

typedef struct
{
  int fd;
  io_role_t role;
  struct lb_connection *conn; // NULL for the listen socket
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
  struct epoll_event events[LB_MAX_EVENTS];
  lb_strategy_t strategy;
} load_balancer_t;

typedef struct lb_connection
{
  int client_fd;
  int backend_fd;
  backend_t *backend;
  io_ctx_t client_ctx;
  io_ctx_t backend_ctx;
  // client_buffer holds bytes travelling client -> backend; backend_buffer
  // holds bytes travelling backend -> client. *_sent tracks how much of the
  // current chunk has already been flushed when a write is only partial.
  char client_buffer[LB_BUFFER_SIZE];
  char backend_buffer[LB_BUFFER_SIZE];
  size_t client_buffer_len;
  size_t client_buffer_sent;
  size_t backend_buffer_len;
  size_t backend_buffer_sent;
  struct timespec start_time;
} lb_connection_t;

#endif
