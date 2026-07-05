#ifndef WORKER_H
#define WORKER_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "connection.h"

typedef struct
{
  int thread_id;
  pthread_t thread;
  int epoll_fd;
  connection_t *connections;
  int connection_count;
  bool running;

  // Statistics. requests_processed is bumped from thread-pool threads when a
  // blocking handler is offloaded, so it must be atomic. bytes_{sent,received}
  // are only ever touched by the owning worker thread and stay plain.
  _Atomic(uint64_t) requests_processed;
  uint64_t bytes_sent;
  uint64_t bytes_received;
} worker_thread_t;

void *worker_thread_function(void *arg);
int handle_new_connection(worker_thread_t *worker, int client_fd);
int handle_client_data(worker_thread_t *worker, connection_t *conn);
int handle_client_write(worker_thread_t *worker, connection_t *conn);

#endif
