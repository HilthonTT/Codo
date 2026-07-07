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

#include "types.h"
#include "selection.h"

// Lifecycle: bind/listen on `port`, create the epoll instance and register the
// listen socket. Backends are added afterwards with add_backend().
int load_balancer_init(load_balancer_t *lb, int port);
int add_backend(load_balancer_t *lb, const char *host, int port, int weight);

// High-performance load balancer main loop
int load_balancer_main_loop(load_balancer_t *lb);

// Handle new client connection
void handle_new_connection(load_balancer_t *lb);

backend_t *select_backend(load_balancer_t *lb, const struct sockaddr_in *client_addr);

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
