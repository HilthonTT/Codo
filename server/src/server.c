#define _GNU_SOURCE

#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "config.h"
#include "connection.h"
#include "http_protocol.h"
#include "route.h"
#include "server.h"
#include "ssl_util.h"
#include "worker.h"

// Hook consumed by the shared HTTP layer (common/http_protocol.c) to fill the
// "Server:" header without depending on http_server_t directly.
const char *http_server_name(void)
{
  return g_server.server_name ? g_server.server_name : "OTA";
}

int http_server_init(http_server_t *server, int port, const char *document_root)
{
  if (!server)
  {
    return -1;
  }

  memset(server, 0, sizeof(http_server_t));

  server->listen_port = port;
  server->document_root = strdup(document_root);
  server->server_name = strdup("OTA-HTTP-Server/1.0");
  server->max_connections = MAX_CONNECTIONS;
  server->enable_keepalive = true;
  server->keepalive_timeout = KEEPALIVE_TIMEOUT;
  server->enable_compression = true;
  server->max_request_size = MAX_REQUEST_SIZE;
  server->worker_count = WORKER_THREADS;
  server->default_handler = default_file_handler;
  server->running = true;

  // Initialize statistics mutex
  pthread_mutex_init(&server->stats_mutex, NULL);

  // Create listening socket
  server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server->listen_fd < 0)
  {
    perror("socket");
    return -1;
  }

  // Set socket options
  set_socket_options(server->listen_fd);
  set_socket_nonblocking(server->listen_fd);

  // Bind to port
  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(port);

  if (bind(server->listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
  {
    perror("bind");
    close(server->listen_fd);
    return -1;
  }

  // Listen for connections
  if (listen(server->listen_fd, BACKLOG) < 0)
  {
    perror("listen");
    close(server->listen_fd);
    return -1;
  }

  return 0;
}

int http_server_start(http_server_t *server)
{
  if (!server)
  {
    return -1;
  }

  for (int i = 0; i < server->worker_count; i++)
  {
    worker_thread_t *worker = &server->workers[i];
    worker->thread_id = i;
    worker->running = true;

    // Create epoll instance for this worker
    worker->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (worker->epoll_fd < 0)
    {
      perror("epoll_create1");
      return -1;
    }

    // Create worker thread
    if (pthread_create(&worker->thread, NULL, worker_thread_function, worker) != 0)
    {
      perror("pthread_create");
      return -1;
    }
  }

  // Accept connections and distribute to workers
  int worker_index = 0;
  while (server->running)
  {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    int client_fd = accept(server->listen_fd, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd < 0)
    {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
      {
        usleep(1000); // 1ms
        continue;
      }
      if (errno == EINTR)
      {
        continue;
      }
      perror("accept");
      continue;
    }

    // Set client socket options
    set_socket_nonblocking(client_fd);
    set_socket_options(client_fd);

    // Distribute connection to worker
    worker_thread_t *worker = &server->workers[worker_index];
    if (handle_new_connection(worker, client_fd) != 0)
    {
      close(client_fd);
    }
    else
    {
      // Record client address on the just-allocated connection (head of list).
      if (worker->connections)
      {
        worker->connections->client_addr = client_addr;
      }
    }

    worker_index = (worker_index + 1) % server->worker_count;

    // Update statistics
    pthread_mutex_lock(&server->stats_mutex);
    server->total_connections++;
    server->active_connections_count++;
    pthread_mutex_unlock(&server->stats_mutex);
  }

  return 0;
}

int http_server_stop(http_server_t *server)
{
  if (!server)
  {
    return -1;
  }
  server->running = false;
  for (int i = 0; i < server->worker_count; i++)
  {
    server->workers[i].running = false;
  }
  return 0;
}

int http_server_cleanup(http_server_t *server)
{
  if (!server)
  {
    return -1;
  }

  for (int i = 0; i < server->worker_count; i++)
  {
    server->workers[i].running = false;
  }
  for (int i = 0; i < server->worker_count; i++)
  {
    if (server->workers[i].thread)
    {
      pthread_join(server->workers[i].thread, NULL);
    }
    if (server->workers[i].epoll_fd > 0)
    {
      close(server->workers[i].epoll_fd);
    }
  }

  // Close listening socket
  if (server->listen_fd > 0)
  {
    close(server->listen_fd);
    server->listen_fd = -1;
  }

  // Cleanup SSL
  if (server->ssl_enabled)
  {
    cleanup_ssl(server);
  }

  // Free resources
  free(server->document_root);
  free(server->server_name);
  server->document_root = NULL;
  server->server_name = NULL;

  // Cleanup routes
  route_t *route = server->routes;
  while (route)
  {
    route_t *next = route->next;
    free(route);
    route = next;
  }
  server->routes = NULL;

  pthread_mutex_destroy(&server->stats_mutex);

  printf("HTTP server cleanup completed\n");
  return 0;
}
