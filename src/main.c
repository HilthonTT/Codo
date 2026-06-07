#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>
#include <openssl/bio.h>
#include <zlib.h>
#include <pthread.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/epoll.h>
#include <unistd.h>

#define MAX_EVENTS 10000
#define MAX_CONNECTIONS 100000
#define BUFFER_SIZE 8192
#define MAX_REQUEST_SIZE 65536
#define MAX_RESPONSE_SIZE 1048576
#define WORKER_THREADS 8
#define BACKLOG 1024
#define KEEPALIVE_TIMEOUT 30
#define MAX_HEADERS 64
#define MAX_HEADERS_SIZE 8192

// HTTP methods types
typedef enum {
  HTTP_GET,
  HTTP_POST,
  HTTP_PUT,
  HTTP_DELETE,
  HTTP_HEAD,
  HTTP_OPTIONS,
  HTTP_PATCH,
  HTTP_CONNECT,
  HTTP_TRACE,
  HTTP_UNKNOWN,
} http_method_t;

// HTTP status codes
typedef enum {
  HTTP_OK = 200,
  HTTP_CREATED = 201,
  HTTP_ACCEPTED = 202,
  HTTP_NO_CONTENT = 204,
  HTTP_MOVED_PERMANENTLY = 301,
  HTTP_FOUND = 302,
  HTTP_NOT_MODIFIED = 304,
  HTTP_BAD_REQUEST = 400,
  HTTP_UNAUTHORIZED = 401,
  HTTP_FORBIDDEN = 403,
  HTTP_NOT_FOUND = 404,
  HTTP_METHOD_NOT_ALLOWED = 405,
  HTTP_REQUEST_TIMEOUT = 408,
  HTTP_PAYLOAD_TOO_LARGE = 413,
  HTTP_INTERNAL_SERVER_ERROR = 500,
  HTTP_NOT_IMPLEMENTED = 501,
  HTTP_BAD_GATEWAY = 502,
  HTTP_SERVICE_UNAVAILABLE = 503,
  HTTP_GATEWAY_TIMEOUT = 504,
} http_status_t;

// Connection states
typedef enum {
  CONN_STATE_READING_REQUEST,
  CONN_STATE_PROCESSING,
  CONN_STATE_WRITNG_RESPONSE,
  CONN_STATE_KEEPALIVE,
  CONN_STATE_WEBSOCKET,
  CONN_STATE_CLOSING,
} connection_state_t;

// Http header structure
typedef struct {
  char name[256];
  char value[2048];
} http_header_t;

// Http request structure
typedef struct {
  http_method_t method;
  char uri[2048];
  char version[16];
  char query_string[2048];
  http_header_t headers[MAX_HEADERS];
  int header_count;
  char *body;
  size_t body_length;
  size_t content_length;
  bool keep_alive;
  bool expect_continue;
  bool is_websocket_upgrade;
  char websocket_key[64];
  char websocket_protocol[256];
} http_request_t;

// HTTP response structure
typedef struct {
  http_status_t status;
  char version[16];
  http_header_t headers[MAX_HEADERS];
  int header_count;
  char *body;
  size_t body_length;
  bool keep_alive;
  bool chunked_encoding;
  bool gzip_compressed;
  time_t last_modified;
  char etag[64];
} http_response_t;

typedef struct connection {
  int socket_fd;
  struct sockaddr_in client_addr;
  connection_state_t state;

  // SSL support
  SSL *ssl;
  bool ssl_enabled;

  // Request/response data
  char read_buffer[BUFFER_SIZE];
  size_t read_buffer_pos;
  size_t read_buffer_size;

  char write_buffer[MAX_RESPONSE_SIZE];
  size_t write_buffer_pos;
  size_t write_buffer_size;

  http_request_t request;
  http_response_t response;

  // Timing
  time_t last_activity;
  time_t connection_time;

  // Websocket support
  bool websocket_handshake_complete;
  char websocket_frame_buffer[BUFFER_SIZE];
  size_t websocket_frame_pos;

  // File serving
  int file_fd;
  off_t file_offset;
  size_t file_size;

  // Compression
  z_stream gzip_stream;
  bool gzip_initialized;

  // Linked list for connection pool
  struct connection *next;
  struct connection *prev;
} connection_t;

// Route handler function type
typedef int (*route_handler_t)(connection_t *conn, http_request_t *request, http_response_t *response);

typedef struct route {
  char pattern[512];
  http_method_t method;
  route_handler_t handler;
  struct route *next;
} route_t;

// Worker thread structure
typedef struct {
  int thread_id;
  pthread_t thread;
  int epoll_fd;
  connection_t *connections;
  int connection_count;
  bool running;

  // Statistics
  uint64_t requests_processed;
  uint64_t bytes_sent;
  uint64_t bytes_received;
} worker_thread_t;

// HTTP server structure
typedef struct {
  int listen_fd;
  int listen_port;
  char *document_root;
  char *server_name;

  // SSL configuration
  SSL_CTX *ssl_ctx;
  bool ssl_enabled;
  char *ssl_cert_file;
  char *ssl_key_file;

  // Worker threads
  worker_thread_t workers[WORKER_THREADS];
  int worker_count;

  // Route handling
  route_t *routes;
  route_handler_t default_handler;

  // Connection pool
  connection_t *connection_pool;
  int max_connections;
  int active_connections;

  // Configuration
  bool enable_keepalive;
  int keepalive_timeout;
  bool enable_compression;
  size_t max_request_size;

  // Statistics
  uint64_t total_requests;
  uint64_t total_connections;
  uint64_t active_connections_count;

  // Control flags
  volatile bool running;
  pthread_mutex_t stats_mutex;
} http_server_t;

// Function prototypes
int http_server_init(http_server_t *server, int port, const char *document_root);
int http_server_start(http_server_t *server);
int http_server_stop(http_server_t *server);
int http_server_cleanup(http_server_t *server);

// SSL functions
int init_ssl(http_server_t *server, const char *cert_file, const char *key_file);
void cleanup_ssl(http_server_t *server);

// Worker thread functions
void *worker_thread_function(void *arg);
int handle_new_connection(worker_thread_t *worker, int client_fd);
int handle_client_data(worker_thread_t *worker, connection_t *conn);
int handle_client_write(worker_thread_t *worker, connection_t *conn);

// HTTP protocol functions
int parse_http_request(connection_t *conn, http_request_t *request);
int generate_http_response(connection_t *conn, http_response_t *response);
int send_http_response(connection_t *conn, http_response_t *response);
int send_file_response(connection_t *conn, const char *file_path);
int send_error_response(connection_t *conn, http_status_t status, const char *message);

// WebSocket functions
int handle_websocket_upgrade(connection_t *conn, http_request_t *request);
int handle_websocket_frame(connection_t *conn, const char *data, size_t length);
void generate_websocket_accept_key(const char *key, char *accept_key);

// Route handling functions
int add_route(http_server_t *server, const char *pattern, http_method_t method, route_handler_t handler);
route_t *find_route(http_server_t *server, const char *uri, http_method_t method);
int default_file_handler(connection_t *conn, http_request_t *request, http_response_t *response);

// Connection management
connection_t *allocate_connection(http_server_t *server);
void free_connection(http_server_t *server, connection_t *conn);
void cleanup_connection(connection_t *conn);
int set_socket_nonblocking(int fd);
int set_socket_options(int fd);

// Compression functions
int init_gzip_compression(connection_t *conn);
void compress_response_body(connection_t *conn, const char *input, size_t input_size);
void cleanup_gzip_compression(connection_t *conn);

// Utility functions
const char *http_method_to_string(http_method_t method);
const char *http_status_to_string(http_status_t status);
http_method_t string_to_http_method(const char *method);
const char *get_mime_type(const char *file_path);
char *url_decode(const char *url);
void parse_query_string(const char *query, http_header_t *params, int *params_count);
bool is_valid_uri(const char *uri);

// Example route handlers
int api_hello_handler(connection_t *conn, http_request_t *request, http_response_t *response);
int api_echo_handler(connection_t *conn, http_request_t *request, http_response_t *response);
int api_status_handler(connection_t *conn, http_request_t *request, http_response_t *response);
int websocket_chat_handler(connection_t *conn, http_request_t *request, http_response_t *response);

// Global server instance
static http_server_t g_server;
static volatile bool g_running = true;

// Internal helper: unlink a connection from its owning worker's list.
static void remove_connection_from_worker(worker_thread_t *worker, connection_t *conn) {
  if (!worker || !conn) {
    return;
  }
  if (conn->prev) {
    conn->prev->next = conn->next;
  } else {
    worker->connections = conn->next;
  }
  if (conn->next) {
    conn->next->prev = conn->prev;
  }
  conn->next = NULL;
  conn->prev = NULL;
  if (worker->connection_count > 0) {
    worker->connection_count--;
  }
}

// Internal helper: format a date in RFC 1123 / HTTP-date format.
static void format_http_date(time_t t, char *buf, size_t buf_size) {
  struct tm gmt;
  gmtime_r(&t, &gmt);
  strftime(buf, buf_size, "%a, %d %b %Y %H:%M:%S GMT", &gmt);
}

void signal_handler(int signum) {
  (void)signum;
  g_running = false;
  g_server.running = false;
}

int main(int argc, char *argv[]) {
  int port = 8080;
  char *document_root = "/var/www/html";

  // Parse command line arguments
  if (argc > 1) {
    port = atoi(argv[1]);
  }
  if (argc > 2) {
    document_root = argv[2];
  }

  // Setup signal handlers
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);
  signal(SIGPIPE, SIG_IGN);

  // Initialize HTTP server
  if (http_server_init(&g_server, port, document_root) != 0) {
    fprintf(stderr, "Failed to initialize HTTP server\n");
    return 1;
  }

  // Add example routes
  add_route(&g_server, "/api/hello", HTTP_GET, api_hello_handler);
  add_route(&g_server, "/api/echo", HTTP_POST, api_echo_handler);
  add_route(&g_server, "/api/status", HTTP_GET, api_status_handler);
  add_route(&g_server, "/ws/chat", HTTP_GET, websocket_chat_handler);

  // Enable SSL if certificates are available
  if (access("server.crt", F_OK) == 0 && access("server.key", F_OK) == 0) {
    if (init_ssl(&g_server, "server.crt", "server.key") == 0) {
      printf("SSL enabled\n");
    }
  }

  printf("HTTP server starting on port %d\n", port);
  printf("Document root: %s\n", document_root);

  if (http_server_start(&g_server) != 0) {
    fprintf(stderr, "Failed to start HTTP server\n");
    http_server_cleanup(&g_server);
    return 1;
  }

  // http_server_start blocks in the accept loop until server->running is
  // cleared by the signal handler, so once we get here we are shutting down.
  http_server_stop(&g_server);
  http_server_cleanup(&g_server);

  printf("HTTP server stopped\n");

  return 0;
}

int http_server_init(http_server_t *server, int port, const char *document_root) {
  if (!server) {
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
  if (server->listen_fd < 0) {
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

  if (bind(server->listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    perror("bind");
    close(server->listen_fd);
    return -1;
  }

  // Listen for connections
  if (listen(server->listen_fd, BACKLOG) < 0) {
    perror("listen");
    close(server->listen_fd);
    return -1;
  }

  return 0;
}

int http_server_start(http_server_t *server) {
  if (!server) {
    return -1;
  }

  for (int i = 0; i < server->worker_count; i++) {
    worker_thread_t *worker = &server->workers[i];
    worker->thread_id = i;
    worker->running = true;

    // Create epoll instance for this worker
    worker->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (worker->epoll_fd < 0) {
      perror("epoll_create1");
      return -1;
    }

    // Create worker thread
    if (pthread_create(&worker->thread, NULL, worker_thread_function, worker) != 0) {
      perror("pthread_create");
      return -1;
    }
  }

  // Accept connections and distribute to workers
  int worker_index = 0;
  while (server->running) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    int client_fd = accept(server->listen_fd, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        usleep(1000); // 1ms
        continue;
      }
      if (errno == EINTR) {
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
    if (handle_new_connection(worker, client_fd) != 0) {
      close(client_fd);
    } else {
      // Record client address on the just-allocated connection (head of list).
      if (worker->connections) {
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

int http_server_stop(http_server_t *server) {
  if (!server) {
    return -1;
  }
  server->running = false;
  for (int i = 0; i < server->worker_count; i++) {
    server->workers[i].running = false;
  }
  return 0;
}

int handle_new_connection(worker_thread_t *worker, int client_fd) {
  // Allocate connection structure
  connection_t *conn = allocate_connection(&g_server);
  if (!conn) {
    return -1;
  }

  conn->socket_fd = client_fd;
  conn->state = CONN_STATE_READING_REQUEST;
  conn->last_activity = time(NULL);
  conn->connection_time = conn->last_activity;

  // Add to worker's connection list
  conn->next = worker->connections;
  if (worker->connections) {
    worker->connections->prev = conn;
  }

  worker->connections = conn;
  worker->connection_count++;

  // Add to epoll
  struct epoll_event event;
  event.events = EPOLLIN | EPOLLET;
  event.data.ptr = conn;

  if (epoll_ctl(worker->epoll_fd, EPOLL_CTL_ADD, client_fd, &event) < 0) {
    perror("epoll_ctl");
    remove_connection_from_worker(worker, conn);
    cleanup_connection(conn);
    free_connection(&g_server, conn);
    return -1;
  }

  return 0;
}

connection_t *allocate_connection(http_server_t *server) {
  if (!server) {
    return NULL;
  }
  connection_t *conn = calloc(1, sizeof(connection_t));
  if (!conn) {
    return NULL;
  }
  // calloc zeroes everything, but 0 is a valid (stdin) fd. Default the fd
  // fields to -1 so cleanup_connection knows there is nothing to close yet.
  conn->socket_fd = -1;
  conn->file_fd = -1;
  pthread_mutex_lock(&server->stats_mutex);
  server->active_connections++;
  pthread_mutex_unlock(&server->stats_mutex);
  return conn;
}

void free_connection(http_server_t *server, connection_t *conn) {
  if (!conn) {
    return;
  }
  cleanup_connection(conn);
  if (server) {
    pthread_mutex_lock(&server->stats_mutex);
    if (server->active_connections > 0) {
      server->active_connections--;
    }
    if (server->active_connections_count > 0) {
      server->active_connections_count--;
    }
    pthread_mutex_unlock(&server->stats_mutex);
  }
  free(conn);
}

void cleanup_connection(connection_t *conn) {
  if (!conn) {
    return;
  }
  if (conn->ssl) {
    SSL_shutdown(conn->ssl);
    SSL_free(conn->ssl);
    conn->ssl = NULL;
  }
  if (conn->socket_fd >= 0) {
    close(conn->socket_fd);
    conn->socket_fd = -1;
  }
  if (conn->file_fd >= 0) {
    close(conn->file_fd);
    conn->file_fd = -1;
  }
  if (conn->request.body) {
    free(conn->request.body);
    conn->request.body = NULL;
  }
  if (conn->response.body) {
    free(conn->response.body);
    conn->response.body = NULL;
  }
  if (conn->gzip_initialized) {
    cleanup_gzip_compression(conn);
  }
}

int set_socket_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return -1;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int set_socket_options(int fd) {
  int optval = 1;

  // Reuse address
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
    return -1;
  }

  // Disable Nagle's algorithm
  if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval)) < 0) {
    return -1;
  }

  return 0;
}

int init_ssl(http_server_t *server, const char *cert_file, const char *key_file) {
  if (!server || !cert_file || !key_file) {
    return -1;
  }

  SSL_library_init();
  SSL_load_error_strings();
  OpenSSL_add_all_algorithms();

  server->ssl_ctx = SSL_CTX_new(TLS_server_method());
  if (!server->ssl_ctx) {
    ERR_print_errors_fp(stderr);
    return -1;
  }

  SSL_CTX_set_min_proto_version(server->ssl_ctx, TLS1_2_VERSION);

  if (SSL_CTX_use_certificate_file(server->ssl_ctx, cert_file, SSL_FILETYPE_PEM) <= 0) {
    ERR_print_errors_fp(stderr);
    SSL_CTX_free(server->ssl_ctx);
    server->ssl_ctx = NULL;
    return -1;
  }

  if (SSL_CTX_use_PrivateKey_file(server->ssl_ctx, key_file, SSL_FILETYPE_PEM) <= 0) {
    ERR_print_errors_fp(stderr);
    SSL_CTX_free(server->ssl_ctx);
    server->ssl_ctx = NULL;
    return -1;
  }

  if (!SSL_CTX_check_private_key(server->ssl_ctx)) {
    fprintf(stderr, "SSL: private key does not match certificate\n");
    SSL_CTX_free(server->ssl_ctx);
    server->ssl_ctx = NULL;
    return -1;
  }

  server->ssl_enabled = true;
  server->ssl_cert_file = strdup(cert_file);
  server->ssl_key_file = strdup(key_file);
  return 0;
}

void cleanup_ssl(http_server_t *server) {
  if (!server) {
    return;
  }
  if (server->ssl_ctx) {
    SSL_CTX_free(server->ssl_ctx);
    server->ssl_ctx = NULL;
  }
  free(server->ssl_cert_file);
  free(server->ssl_key_file);
  server->ssl_cert_file = NULL;
  server->ssl_key_file = NULL;
  server->ssl_enabled = false;
  EVP_cleanup();
  ERR_free_strings();
}

int default_file_handler(connection_t *conn, http_request_t *request, http_response_t *response) {
  (void)response;

  if (request->method != HTTP_GET && request->method != HTTP_HEAD) {
    return send_error_response(conn, HTTP_METHOD_NOT_ALLOWED, "Method not allowed");
  }

  // Check if path is safe
  if (!is_valid_uri(request->uri)) {
    return send_error_response(conn, HTTP_FORBIDDEN, "Access denied");
  }

  // Construct file path
  char file_path[2048];
  snprintf(file_path, sizeof(file_path), "%s%s", g_server.document_root, request->uri);

  // Check if file exists
  struct stat file_stat;
  if (stat(file_path, &file_stat) < 0) {
    return send_error_response(conn, HTTP_NOT_FOUND, "File not found");
  }

  // Send file
  return send_file_response(conn, file_path);
}

int send_file_response(connection_t *conn, const char *file_path) {
  if (!conn || !file_path) {
    return -1;
  }

  int fd = open(file_path, O_RDONLY);
  if (fd < 0) {
    return send_error_response(conn, HTTP_NOT_FOUND, "File not found");
  }

  struct stat st;
  if (fstat(fd, &st) < 0) {
    close(fd);
    return send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR, "Could not stat file");
  }

  if (!S_ISREG(st.st_mode)) {
    close(fd);
    return send_error_response(conn, HTTP_FORBIDDEN, "Not a regular file");
  }

  // Bound the file size to what the write buffer can hold along with headers.
  if ((size_t)st.st_size > MAX_RESPONSE_SIZE - MAX_HEADERS_SIZE) {
    close(fd);
    return send_error_response(conn, HTTP_PAYLOAD_TOO_LARGE, "File too large");
  }

  char *body = malloc(st.st_size > 0 ? (size_t)st.st_size : 1);
  if (!body) {
    close(fd);
    return send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR, "Out of memory");
  }

  off_t total = 0;
  while (total < st.st_size) {
    ssize_t n = read(fd, body + total, (size_t)(st.st_size - total));
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      free(body);
      close(fd);
      return send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR, "Read error");
    }
    if (n == 0) {
      break;
    }
    total += n;
  }
  close(fd);

  if (total < st.st_size) {
    free(body);
    return send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR, "Short read");
  }

  // Replace any previously assigned response body.
  if (conn->response.body) {
    free(conn->response.body);
  }

  conn->response.status = HTTP_OK;
  strcpy(conn->response.version, "HTTP/1.1");
  conn->response.body = body;
  conn->response.body_length = (size_t)st.st_size;
  conn->response.keep_alive = conn->request.keep_alive;
  conn->response.last_modified = st.st_mtime;

  const char *mime = get_mime_type(file_path);
  strcpy(conn->response.headers[0].name, "Content-Type");
  strncpy(conn->response.headers[0].value, mime, sizeof(conn->response.headers[0].value) - 1);
  conn->response.headers[0].value[sizeof(conn->response.headers[0].value) - 1] = '\0';
  conn->response.header_count = 1;

  return send_http_response(conn, &conn->response);
}

int send_error_response(connection_t *conn, http_status_t status, const char *message) {
  if (!conn) {
    return -1;
  }

  char error_body[1024];
  snprintf(error_body, sizeof(error_body),
           "<html><head><title>%d %s</title></head>"
           "<body><h1>%d %s</h1><p>%s</p></body></html>",
           status, http_status_to_string(status),
           status, http_status_to_string(status),
           message ? message : "");

  if (conn->response.body) {
    free(conn->response.body);
    conn->response.body = NULL;
  }

  conn->response.status = status;
  strcpy(conn->response.version, "HTTP/1.1");
  conn->response.body = strdup(error_body);
  conn->response.body_length = conn->response.body ? strlen(conn->response.body) : 0;
  conn->response.keep_alive = false;

  // Add HTML content type header
  strcpy(conn->response.headers[0].name, "Content-Type");
  strcpy(conn->response.headers[0].value, "text/html");
  conn->response.header_count = 1;

  return send_http_response(conn, &conn->response);
}

bool is_valid_uri(const char *uri) {
  if (!uri) {
    return false;
  }

  // URI must be absolute path
  if (*uri != '/') {
    return false;
  }

  // Check for directory traversal
  if (strstr(uri, "../") != NULL || strstr(uri, "..\\") != NULL) {
    return false;
  }

  return true;
}

int send_http_response(connection_t *conn, http_response_t *response) {
  if (!conn || !response) {
    return -1;
  }

  char date_buf[64];
  format_http_date(time(NULL), date_buf, sizeof(date_buf));

  // Generate response status line + standard headers in a single format string.
  char header_buffer[MAX_HEADERS_SIZE];
  int header_len = snprintf(header_buffer, sizeof(header_buffer),
                            "%s %d %s\r\n"
                            "Server: %s\r\n"
                            "Date: %s\r\n"
                            "Content-Length: %zu\r\n"
                            "Connection: %s\r\n",
                            response->version[0] ? response->version : "HTTP/1.1",
                            (int)response->status,
                            http_status_to_string(response->status),
                            g_server.server_name ? g_server.server_name : "OTA",
                            date_buf,
                            response->body_length,
                            response->keep_alive ? "keep-alive" : "close");

  if (header_len < 0 || (size_t)header_len >= sizeof(header_buffer)) {
    return -1;
  }

  // Add custom headers
  for (int i = 0; i < response->header_count; i++) {
    int written = snprintf(header_buffer + header_len,
                           sizeof(header_buffer) - (size_t)header_len,
                           "%s: %s\r\n",
                           response->headers[i].name,
                           response->headers[i].value);
    if (written < 0 || (size_t)(header_len + written) >= sizeof(header_buffer)) {
      return -1;
    }
    header_len += written;
  }

  // End headers
  int end_written = snprintf(header_buffer + header_len,
                             sizeof(header_buffer) - (size_t)header_len, "\r\n");
  if (end_written < 0 || (size_t)(header_len + end_written) >= sizeof(header_buffer)) {
    return -1;
  }
  header_len += end_written;

  // Copy headers and body to write buffer
  size_t total_size = (size_t)header_len + response->body_length;
  if (total_size > MAX_RESPONSE_SIZE) {
    return -1; // Response too large
  }

  memcpy(conn->write_buffer, header_buffer, (size_t)header_len);
  if (response->body && response->body_length > 0) {
    memcpy(conn->write_buffer + header_len, response->body, response->body_length);
  }

  conn->write_buffer_pos = 0;
  conn->write_buffer_size = total_size;
  conn->state = CONN_STATE_WRITNG_RESPONSE;
  return 0;
}

int parse_http_request(connection_t *conn, http_request_t *request) {
  if (!conn || !request) {
    return -1;
  }

  // Hold onto the old body pointer in case we are re-parsing on the same
  // connection; free it after zeroing the struct.
  char *old_body = request->body;
  memset(request, 0, sizeof(http_request_t));
  if (old_body) {
    free(old_body);
  }

  char *buf = conn->read_buffer;
  char *end = buf + conn->read_buffer_pos;

  // Parse request line
  char *line_end = memchr(buf, '\n', (size_t)(end - buf));
  if (!line_end) {
    return -1;
  }
  *line_end = '\0';
  if (line_end > buf && *(line_end - 1) == '\r') {
    *(line_end - 1) = '\0';
  }

  char method_str[16];
  if (sscanf(buf, "%15s %2047s %15s", method_str, request->uri, request->version) != 3) {
    return -1;
  }

  request->method = string_to_http_method(method_str);

  // Parse query string
  char *query_start = strchr(request->uri, '?');
  if (query_start) {
    *query_start = '\0';
    strncpy(request->query_string, query_start + 1, sizeof(request->query_string) - 1);
    request->query_string[sizeof(request->query_string) - 1] = '\0';
  }

  // Default keep-alive on HTTP/1.1, off on HTTP/1.0. Either side may override
  // via an explicit Connection header below.
  request->keep_alive = (strcmp(request->version, "HTTP/1.1") == 0);

  // Parse headers
  char *p = line_end + 1;
  while (p < end) {
    char *next_end = memchr(p, '\n', (size_t)(end - p));
    if (!next_end) {
      break;
    }
    *next_end = '\0';
    if (next_end > p && *(next_end - 1) == '\r') {
      *(next_end - 1) = '\0';
    }

    if (*p == '\0') {
      // End of headers -- body (if any) starts here.
      p = next_end + 1;
      if (p < end) {
        size_t blen = (size_t)(end - p);
        request->body = malloc(blen + 1);
        if (request->body) {
          memcpy(request->body, p, blen);
          request->body[blen] = '\0';
          request->body_length = blen;
        }
      }
      break;
    }

    char *colon = strchr(p, ':');
    if (!colon) {
      p = next_end + 1;
      continue;
    }

    *colon = '\0';
    char *name = p;
    char *value = colon + 1;

    // Skip whitespace
    while (*value == ' ' || *value == '\t') {
      value++;
    }

    if (request->header_count < MAX_HEADERS) {
      strncpy(request->headers[request->header_count].name, name, 255);
      request->headers[request->header_count].name[255] = '\0';
      strncpy(request->headers[request->header_count].value, value, 2047);
      request->headers[request->header_count].value[2047] = '\0';
      request->header_count++;
    }

    // Check for special headers
    if (strcasecmp(name, "Connection") == 0) {
      request->keep_alive = (strcasecmp(value, "keep-alive") == 0);
    } else if (strcasecmp(name, "Content-Length") == 0) {
      request->content_length = (size_t)atol(value);
    } else if (strcasecmp(name, "Expect") == 0) {
      request->expect_continue = (strcasecmp(value, "100-continue") == 0);
    } else if (strcasecmp(name, "Upgrade") == 0) {
      request->is_websocket_upgrade = (strcasecmp(value, "websocket") == 0);
    } else if (strcasecmp(name, "Sec-WebSocket-Key") == 0) {
      strncpy(request->websocket_key, value, sizeof(request->websocket_key) - 1);
      request->websocket_key[sizeof(request->websocket_key) - 1] = '\0';
    } else if (strcasecmp(name, "Sec-WebSocket-Protocol") == 0) {
      strncpy(request->websocket_protocol, value, sizeof(request->websocket_protocol) - 1);
      request->websocket_protocol[sizeof(request->websocket_protocol) - 1] = '\0';
    }

    p = next_end + 1;
  }

  return 0;
}

const char *http_status_to_string(http_status_t status) {
  switch (status)
  {
    case HTTP_OK: return "OK";
    case HTTP_CREATED: return "Created";
    case HTTP_ACCEPTED: return "Accepted";
    case HTTP_NO_CONTENT: return "No Content";
    case HTTP_MOVED_PERMANENTLY: return "Moved Permanently";
    case HTTP_FOUND: return "Found";
    case HTTP_NOT_MODIFIED: return "Not Modified";
    case HTTP_BAD_REQUEST: return "Bad Request";
    case HTTP_UNAUTHORIZED: return "Unauthorized";
    case HTTP_FORBIDDEN: return "Forbidden";
    case HTTP_NOT_FOUND: return "Not Found";
    case HTTP_METHOD_NOT_ALLOWED: return "Method Not Allowed";
    case HTTP_REQUEST_TIMEOUT: return "Request Timeout";
    case HTTP_PAYLOAD_TOO_LARGE: return "Payload Too Large";
    case HTTP_INTERNAL_SERVER_ERROR: return "Internal Server Error";
    case HTTP_NOT_IMPLEMENTED: return "Not Implemented";
    case HTTP_BAD_GATEWAY: return "Bad Gateway";
    case HTTP_SERVICE_UNAVAILABLE: return "Service Unavailable";
    case HTTP_GATEWAY_TIMEOUT: return "Gateway Timeout";
    default: return "Unknown";
  }
}

http_method_t string_to_http_method(const char *method) {
  if (strcasecmp(method, "GET") == 0) return HTTP_GET;
  if (strcasecmp(method, "POST") == 0) return HTTP_POST;
  if (strcasecmp(method, "PUT") == 0) return HTTP_PUT;
  if (strcasecmp(method, "DELETE") == 0) return HTTP_DELETE;
  if (strcasecmp(method, "HEAD") == 0) return HTTP_HEAD;
  if (strcasecmp(method, "OPTIONS") == 0) return HTTP_OPTIONS;
  if (strcasecmp(method, "PATCH") == 0) return HTTP_PATCH;
  if (strcasecmp(method, "CONNECT") == 0) return HTTP_CONNECT;
  if (strcasecmp(method, "TRACE") == 0) return HTTP_TRACE;
  return HTTP_UNKNOWN;
}

const char *http_method_to_string(http_method_t method) {
  switch (method) {
    case HTTP_GET: return "GET";
    case HTTP_POST: return "POST";
    case HTTP_PUT: return "PUT";
    case HTTP_DELETE: return "DELETE";
    case HTTP_HEAD: return "HEAD";
    case HTTP_OPTIONS: return "OPTIONS";
    case HTTP_PATCH: return "PATCH";
    case HTTP_CONNECT: return "CONNECT";
    case HTTP_TRACE: return "TRACE";
    default: return "UNKNOWN";
  }
}

void *worker_thread_function(void *arg) {
  worker_thread_t *worker = (worker_thread_t *)arg;
  struct epoll_event events[MAX_EVENTS];

  // Set thread name
  char thread_name[16];
  snprintf(thread_name, sizeof(thread_name), "http_worker_%d", worker->thread_id);
  pthread_setname_np(pthread_self(), thread_name);

  printf("Worker thread %d started\n", worker->thread_id);

  while (worker->running) {
    int event_count = epoll_wait(worker->epoll_fd, events, MAX_EVENTS, 1000);

    if (event_count < 0) {
      if (errno == EINTR) {
        continue;
      }

      perror("epoll_wait");
      break;
    }

    for (int i = 0; i < event_count; i++) {
      struct epoll_event event = events[i];
      connection_t *conn = (connection_t *)event.data.ptr;
      if (!conn) {
        continue;
      }

      if (event.events & (EPOLLERR | EPOLLHUP)) {
        // Connection error or hangup
        remove_connection_from_worker(worker, conn);
        free_connection(&g_server, conn);
        continue;
      }

      if (event.events & EPOLLIN) {
        // Data available for reading
        if (handle_client_data(worker, conn) != 0) {
          remove_connection_from_worker(worker, conn);
          free_connection(&g_server, conn);
          continue;
        }
      }

      if (event.events & EPOLLOUT) {
        if (handle_client_write(worker, conn) != 0) {
          remove_connection_from_worker(worker, conn);
          free_connection(&g_server, conn);
          continue;
        }
      }
    }

    // Check for connection timeouts
    time_t current_time = time(NULL);
    connection_t *conn = worker->connections;

    while (conn) {
      connection_t *next = conn->next;

      if (current_time - conn->last_activity > g_server.keepalive_timeout) {
        remove_connection_from_worker(worker, conn);
        free_connection(&g_server, conn);
      }

      conn = next;
    }
  }

  // Drain the worker's connection list before the thread exits so we don't
  // leak the per-connection buffers on shutdown.
  connection_t *conn = worker->connections;
  while (conn) {
    connection_t *next = conn->next;
    free_connection(&g_server, conn);
    conn = next;
  }
  worker->connections = NULL;
  worker->connection_count = 0;

  printf("Worker thread %d stopping\n", worker->thread_id);

  return NULL;
}

int handle_client_data(worker_thread_t *worker, connection_t *conn) {
  if (!worker || !conn) {
    return -1;
  }

  // Drain everything currently readable on the socket into the read buffer.
  // We're using edge-triggered epoll, so we must keep reading until EAGAIN.
  for (;;) {
    if (conn->read_buffer_pos >= sizeof(conn->read_buffer) - 1) {
      // Buffer full -- request larger than we can handle.
      send_error_response(conn, HTTP_PAYLOAD_TOO_LARGE, "Request too large");
      struct epoll_event ev;
      ev.events = EPOLLOUT | EPOLLET;
      ev.data.ptr = conn;
      epoll_ctl(worker->epoll_fd, EPOLL_CTL_MOD, conn->socket_fd, &ev);
      return 0;
    }

    ssize_t bytes_read;
    if (conn->ssl_enabled && conn->ssl) {
      bytes_read = SSL_read(conn->ssl,
                            conn->read_buffer + conn->read_buffer_pos,
                            (int)(sizeof(conn->read_buffer) - conn->read_buffer_pos - 1));
      if (bytes_read <= 0) {
        int ssl_error = SSL_get_error(conn->ssl, (int)bytes_read);
        if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
          break;
        }
        return -1;
      }
    } else {
      bytes_read = read(conn->socket_fd,
                        conn->read_buffer + conn->read_buffer_pos,
                        sizeof(conn->read_buffer) - conn->read_buffer_pos - 1);
      if (bytes_read == 0) {
        // Peer closed.
        return -1;
      }
      if (bytes_read < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          break;
        }
        if (errno == EINTR) {
          continue;
        }
        return -1;
      }
    }

    conn->read_buffer_pos += (size_t)bytes_read;
    conn->read_buffer[conn->read_buffer_pos] = '\0';
    worker->bytes_received += (uint64_t)bytes_read;
    conn->last_activity = time(NULL);
  }

  // Need at least the end-of-headers marker before we attempt a parse.
  if (!strstr(conn->read_buffer, "\r\n\r\n") && !strstr(conn->read_buffer, "\n\n")) {
    return 0; // wait for more data
  }

  conn->state = CONN_STATE_PROCESSING;

  int parse_result = parse_http_request(conn, &conn->request);
  if (parse_result != 0) {
    send_error_response(conn, HTTP_BAD_REQUEST, "Bad Request");
  } else {
    route_t *route = find_route(&g_server, conn->request.uri, conn->request.method);
    if (route && route->handler) {
      if (route->handler(conn, &conn->request, &conn->response) != 0) {
        // Handler reported an error; if it didn't already prepare a response,
        // send a generic one.
        if (conn->write_buffer_size == 0) {
          send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR, "Handler error");
        }
      }
    } else if (g_server.default_handler) {
      g_server.default_handler(conn, &conn->request, &conn->response);
    } else {
      send_error_response(conn, HTTP_NOT_FOUND, "Not Found");
    }
  }

  pthread_mutex_lock(&g_server.stats_mutex);
  g_server.total_requests++;
  pthread_mutex_unlock(&g_server.stats_mutex);
  worker->requests_processed++;

  // Reset read buffer for the next request on this connection.
  conn->read_buffer_pos = 0;
  conn->read_buffer[0] = '\0';

  // Switch to writing the response.
  struct epoll_event ev;
  ev.events = EPOLLOUT | EPOLLET;
  ev.data.ptr = conn;
  if (epoll_ctl(worker->epoll_fd, EPOLL_CTL_MOD, conn->socket_fd, &ev) < 0) {
    return -1;
  }

  return 0;
}

int handle_client_write(worker_thread_t *worker, connection_t *conn) {
  if (!conn || conn->state != CONN_STATE_WRITNG_RESPONSE) {
    return -1;
  }

  // Edge-triggered: write until we'd block.
  while (conn->write_buffer_pos < conn->write_buffer_size) {
    ssize_t bytes_written;

    if (conn->ssl_enabled && conn->ssl) {
      // SSL write
      bytes_written = SSL_write(conn->ssl,
                                conn->write_buffer + conn->write_buffer_pos,
                                (int)(conn->write_buffer_size - conn->write_buffer_pos));

      if (bytes_written <= 0) {
        int ssl_error = SSL_get_error(conn->ssl, (int)bytes_written);
        if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
          return 0; // Would block
        }
        return -1; // Error
      }
    } else {
      // Regular write
      bytes_written = write(
          conn->socket_fd,
          conn->write_buffer + conn->write_buffer_pos,
          conn->write_buffer_size - conn->write_buffer_pos);

      if (bytes_written < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          return 0; // Would block
        }
        if (errno == EINTR) {
          continue;
        }
        return -1; // Error
      }
      if (bytes_written == 0) {
        return -1;
      }
    }

    conn->write_buffer_pos += (size_t)bytes_written;
    worker->bytes_sent += (uint64_t)bytes_written;
  }

  // Response completely sent.
  if (conn->request.keep_alive && g_server.enable_keepalive) {
    // Reset for next request on this connection.
    conn->state = CONN_STATE_READING_REQUEST;
    conn->read_buffer_pos = 0;
    conn->write_buffer_pos = 0;
    conn->write_buffer_size = 0;

    if (conn->request.body) {
      free(conn->request.body);
    }
    if (conn->response.body) {
      free(conn->response.body);
    }
    memset(&conn->request, 0, sizeof(http_request_t));
    memset(&conn->response, 0, sizeof(http_response_t));

    // Re-arm for reads.
    struct epoll_event event;
    event.events = EPOLLIN | EPOLLET;
    event.data.ptr = conn;
    if (epoll_ctl(worker->epoll_fd, EPOLL_CTL_MOD, conn->socket_fd, &event) < 0) {
      return -1;
    }
  } else {
    // Close connection (caller will tear it down).
    return -1;
  }

  return 0;
}

int generate_http_response(connection_t *conn, http_response_t *response) {
  // Simple passthrough: the heavy lifting lives in send_http_response, which
  // already formats and stages the response in the connection's write buffer.
  if (!conn || !response) {
    return -1;
  }
  return send_http_response(conn, response);
}

int add_route(http_server_t *server, const char *pattern, http_method_t method, route_handler_t handler) {
  if (!server || !pattern || !handler) {
    return -1;
  }
  route_t *route = calloc(1, sizeof(route_t));
  if (!route) {
    return -1;
  }
  strncpy(route->pattern, pattern, sizeof(route->pattern) - 1);
  route->pattern[sizeof(route->pattern) - 1] = '\0';
  route->method = method;
  route->handler = handler;
  route->next = server->routes;
  server->routes = route;
  return 0;
}

route_t *find_route(http_server_t *server, const char *uri, http_method_t method) {
  if (!server || !uri) {
    return NULL;
  }
  for (route_t *r = server->routes; r; r = r->next) {
    if (r->method == method && strcmp(r->pattern, uri) == 0) {
      return r;
    }
  }
  return NULL;
}

void generate_websocket_accept_key(const char *key, char *accept_key) {
  if (!key || !accept_key) {
    return;
  }

  static const char *guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  char concatenated[256];
  snprintf(concatenated, sizeof(concatenated), "%s%s", key, guid);

  unsigned char sha1[SHA_DIGEST_LENGTH];
  SHA1((const unsigned char *)concatenated, strlen(concatenated), sha1);

  BIO *b64 = BIO_new(BIO_f_base64());
  BIO *mem = BIO_new(BIO_s_mem());
  if (!b64 || !mem) {
    if (b64) BIO_free(b64);
    if (mem) BIO_free(mem);
    accept_key[0] = '\0';
    return;
  }
  b64 = BIO_push(b64, mem);
  BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
  BIO_write(b64, sha1, SHA_DIGEST_LENGTH);
  BIO_flush(b64);

  BUF_MEM *bptr = NULL;
  BIO_get_mem_ptr(b64, &bptr);
  size_t copy_len = 0;
  if (bptr) {
    copy_len = bptr->length < 63 ? bptr->length : 63;
    memcpy(accept_key, bptr->data, copy_len);
  }
  accept_key[copy_len] = '\0';

  BIO_free_all(b64);
}

int handle_websocket_upgrade(connection_t *conn, http_request_t *request) {
  if (!conn || !request) {
    return -1;
  }
  if (!request->is_websocket_upgrade || request->websocket_key[0] == '\0') {
    return send_error_response(conn, HTTP_BAD_REQUEST, "Invalid WebSocket upgrade");
  }

  char accept_key[64];
  generate_websocket_accept_key(request->websocket_key, accept_key);

  int len = snprintf(conn->write_buffer, MAX_RESPONSE_SIZE,
                     "HTTP/1.1 101 Switching Protocols\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n"
                     "\r\n",
                     accept_key);
  if (len < 0 || (size_t)len >= MAX_RESPONSE_SIZE) {
    return -1;
  }

  conn->write_buffer_pos = 0;
  conn->write_buffer_size = (size_t)len;
  conn->state = CONN_STATE_WRITNG_RESPONSE;
  conn->websocket_handshake_complete = true;
  // The connection stays open for WebSocket framing once the handshake is
  // flushed; we mark the request as keep-alive so handle_client_write does not
  // tear it down after writing.
  conn->request.keep_alive = true;
  return 0;
}

int handle_websocket_frame(connection_t *conn, const char *data, size_t length) {
  // Minimal stub: this implementation does not yet parse WebSocket frames.
  // It exists so the symbol is defined and callers compile.
  (void)conn;
  (void)data;
  (void)length;
  return 0;
}

int init_gzip_compression(connection_t *conn) {
  if (!conn) {
    return -1;
  }
  if (conn->gzip_initialized) {
    return 0;
  }
  memset(&conn->gzip_stream, 0, sizeof(conn->gzip_stream));
  // 15 | 16 enables the gzip wrapper rather than raw deflate.
  if (deflateInit2(&conn->gzip_stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                   15 | 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
    return -1;
  }
  conn->gzip_initialized = true;
  return 0;
}

void compress_response_body(connection_t *conn, const char *input, size_t input_size) {
  // Minimal stub: feeding input through the deflate stream and assembling a
  // gzip-compressed response body is not yet wired into the response path.
  // We touch the parameters so the build stays warning-free.
  (void)conn;
  (void)input;
  (void)input_size;
}

void cleanup_gzip_compression(connection_t *conn) {
  if (!conn || !conn->gzip_initialized) {
    return;
  }
  deflateEnd(&conn->gzip_stream);
  conn->gzip_initialized = false;
}

const char *get_mime_type(const char *file_path) {
  if (!file_path) {
    return "application/octet-stream";
  }
  const char *ext = strrchr(file_path, '.');
  if (!ext) {
    return "application/octet-stream";
  }
  if (strcasecmp(ext, ".html") == 0 || strcasecmp(ext, ".htm") == 0) return "text/html";
  if (strcasecmp(ext, ".css") == 0) return "text/css";
  if (strcasecmp(ext, ".js") == 0) return "application/javascript";
  if (strcasecmp(ext, ".json") == 0) return "application/json";
  if (strcasecmp(ext, ".xml") == 0) return "application/xml";
  if (strcasecmp(ext, ".png") == 0) return "image/png";
  if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) return "image/jpeg";
  if (strcasecmp(ext, ".gif") == 0) return "image/gif";
  if (strcasecmp(ext, ".svg") == 0) return "image/svg+xml";
  if (strcasecmp(ext, ".ico") == 0) return "image/x-icon";
  if (strcasecmp(ext, ".txt") == 0) return "text/plain";
  if (strcasecmp(ext, ".pdf") == 0) return "application/pdf";
  return "application/octet-stream";
}

static int hex_to_int(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

char *url_decode(const char *url) {
  if (!url) {
    return NULL;
  }
  size_t len = strlen(url);
  char *decoded = malloc(len + 1);
  if (!decoded) {
    return NULL;
  }
  size_t j = 0;
  for (size_t i = 0; i < len; i++) {
    if (url[i] == '%' && i + 2 < len) {
      int hi = hex_to_int(url[i + 1]);
      int lo = hex_to_int(url[i + 2]);
      if (hi >= 0 && lo >= 0) {
        decoded[j++] = (char)((hi << 4) | lo);
        i += 2;
        continue;
      }
    }
    if (url[i] == '+') {
      decoded[j++] = ' ';
    } else {
      decoded[j++] = url[i];
    }
  }
  decoded[j] = '\0';
  return decoded;
}

void parse_query_string(const char *query, http_header_t *params, int *params_count) {
  if (!query || !params || !params_count) {
    return;
  }
  *params_count = 0;

  char buf[2048];
  strncpy(buf, query, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  char *saveptr = NULL;
  char *pair = strtok_r(buf, "&", &saveptr);
  while (pair && *params_count < MAX_HEADERS) {
    char *eq = strchr(pair, '=');
    if (eq) {
      *eq = '\0';
      strncpy(params[*params_count].name, pair, sizeof(params[*params_count].name) - 1);
      params[*params_count].name[sizeof(params[*params_count].name) - 1] = '\0';
      strncpy(params[*params_count].value, eq + 1, sizeof(params[*params_count].value) - 1);
      params[*params_count].value[sizeof(params[*params_count].value) - 1] = '\0';
      (*params_count)++;
    }
    pair = strtok_r(NULL, "&", &saveptr);
  }
}

int api_hello_handler(connection_t *conn, http_request_t *request, http_response_t *response) {
  const char *hello_msg = "{\"message\": \"Hello, World!\", \"timestamp\": \"2026-01-01T00:00:00Z\"}";

  response->status = HTTP_OK;
  strcpy(response->version, "HTTP/1.1");
  response->body = strdup(hello_msg);
  response->body_length = strlen(hello_msg);
  response->keep_alive = request->keep_alive;

  strcpy(response->headers[0].name, "Content-Type");
  strcpy(response->headers[0].value, "application/json");
  response->header_count = 1;

  return send_http_response(conn, response);
}

int api_echo_handler(connection_t *conn, http_request_t *request, http_response_t *response) {
  if (request->method != HTTP_POST) {
    return send_error_response(conn, HTTP_METHOD_NOT_ALLOWED, "Method not allowed");
  }

  size_t body_len = request->body_length;
  const char *src = request->body ? request->body : "";

  response->body = malloc(body_len + 1);
  if (!response->body) {
    return send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR, "Out of memory");
  }
  memcpy(response->body, src, body_len);
  response->body[body_len] = '\0';
  response->body_length = body_len;

  response->status = HTTP_OK;
  strcpy(response->version, "HTTP/1.1");
  response->keep_alive = request->keep_alive;

  strcpy(response->headers[0].name, "Content-Type");
  strcpy(response->headers[0].value, "text/plain");
  response->header_count = 1;

  return send_http_response(conn, response);
}

int api_status_handler(connection_t *conn, http_request_t *request, http_response_t *response) {
  char status_json[1024];
  snprintf(status_json, sizeof(status_json),
           "{"
           "\"server\": \"%s\","
           "\"total_connections\": %lu,"
           "\"total_requests\": %lu,"
           "\"active_connections\": %lu"
           "}",
           g_server.server_name,
           (unsigned long)g_server.total_connections,
           (unsigned long)g_server.total_requests,
           (unsigned long)g_server.active_connections_count);

  response->status = HTTP_OK;
  strcpy(response->version, "HTTP/1.1");
  response->body = strdup(status_json);
  response->body_length = response->body ? strlen(response->body) : 0;
  response->keep_alive = request->keep_alive;

  strcpy(response->headers[0].name, "Content-Type");
  strcpy(response->headers[0].value, "application/json");
  response->header_count = 1;

  return send_http_response(conn, response);
}

int websocket_chat_handler(connection_t *conn, http_request_t *request, http_response_t *response) {
  (void)response;
  if (request->is_websocket_upgrade) {
    return handle_websocket_upgrade(conn, request);
  }
  return send_error_response(conn, HTTP_BAD_REQUEST, "WebSocket upgrade required");
}

int http_server_cleanup(http_server_t *server) {
  if (!server) {
    return -1;
  }

  for (int i = 0; i < server->worker_count; i++) {
    server->workers[i].running = false;
  }
  for (int i = 0; i < server->worker_count; i++) {
    if (server->workers[i].thread) {
      pthread_join(server->workers[i].thread, NULL);
    }
    if (server->workers[i].epoll_fd > 0) {
      close(server->workers[i].epoll_fd);
    }
  }

  // Close listening socket
  if (server->listen_fd > 0) {
    close(server->listen_fd);
    server->listen_fd = -1;
  }

  // Cleanup SSL
  if (server->ssl_enabled) {
    cleanup_ssl(server);
  }

  // Free resources
  free(server->document_root);
  free(server->server_name);
  server->document_root = NULL;
  server->server_name = NULL;

  // Cleanup routes
  route_t *route = server->routes;
  while (route) {
    route_t *next = route->next;
    free(route);
    route = next;
  }
  server->routes = NULL;

  pthread_mutex_destroy(&server->stats_mutex);

  printf("HTTP server cleanup completed\n");
  return 0;
}
