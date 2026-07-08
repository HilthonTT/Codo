#ifndef BALANCER_H
#define BALANCER_H

#include <netinet/in.h>
#include <stdint.h>

#include "types.h"
#include "selection.h"

// Lifecycle: bind/listen on `port`, create the epoll instance and register the
// listen socket. Backends are added afterwards with add_backend().
int load_balancer_init(load_balancer_t *lb, int port);
int add_backend(load_balancer_t *lb, const char *host, int port, int weight);

// High-performance load balancer main loop
int load_balancer_main_loop(load_balancer_t *lb);

// Accept a client, pick a backend and start proxying.
void lb_handle_new_connection(load_balancer_t *lb);

backend_t *select_backend(load_balancer_t *lb, const struct sockaddr_in *client_addr);

// Read-side handlers for each end of a proxied connection.
void lb_handle_client_data(load_balancer_t *lb, lb_connection_t *conn);
void lb_handle_backend_data(load_balancer_t *lb, lb_connection_t *conn);

void lb_close_connection(load_balancer_t *lb, lb_connection_t *conn);

// Socket / epoll helpers
int set_nonblocking(int fd);
int create_backend_connection(backend_t *backend);
int add_connection_to_epoll(load_balancer_t *lb, lb_connection_t *conn);
int modify_epoll_events(load_balancer_t *lb, io_ctx_t *ctx, uint32_t events);

// Write-side and error handlers
void lb_handle_client_write(load_balancer_t *lb, lb_connection_t *conn);
void lb_handle_backend_write(load_balancer_t *lb, lb_connection_t *conn);
void lb_handle_connection_error(load_balancer_t *lb, lb_connection_t *conn);

#endif
