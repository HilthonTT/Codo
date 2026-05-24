#include <stdio.h>
#include <stdbool.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <zlib.h>
#include <pthread.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>

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
  HTTP_UNTRACE,
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
int add_route(http_server_t *server, const char pattern, http_method_t method, route_handler_t handler);
route_t *find_route(http_server_t *server, const char *uri, http_method_t method);
int default_file_handler(connection_t *conn, http_request_t *request, http_response_t *response);

// Connection management
connection_t *allocate_connection(http_server_t *server);
void free_connection(http_server_t *server, connection_t *conn);
void cleanup_connection(connection_t* conn);
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
char *get_mime_type(const char *file_path);
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

void signal_handler(int signum) {
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

  // TODO: Start the server

  printf("HTTP server started on port %d\n", port);
  printf("Document root: %s\n", document_root);

  // Main loop
  while (g_running) {
    sleep(10);
  }

  // Stop and cleanup
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

void cleanup_ssl(http_server_t *server) {
  // TODO: actually cleanup ssl
  return;
}

int default_file_handler(connection_t *conn, http_request_t *request, http_response_t *response) {
  if (request->method != HTTP_GET && request->method != HTTP_HEAD) {
    return send_error_response(conn, HTTP_METHOD_NOT_ALLOWED, "Method not allowed");
  }

  // Construct file path
  char file_path[2048];
  snprintf(file_path, sizeof(file_path), "%s%s", g_server.document_root, request->uri);

  // Check if path is safe
  if (!is_valid_uri(request->uri)) {
    return send_error_response(conn, HTTP_FORBIDDEN, "Access denied");
  }

  // Check if file exists
  struct stat file_stat;
  if (stat(file_path, &file_stat) < 0) {
    return send_error_response(conn, HTTP_FORBIDDEN, "File not found");
  }

  // Send file
  return send_file_response(conn, file_path);
}

int send_file_response(connection_t *conn, const char *file_path) {
  // TODO: Implement this later
  return 0;
}

int send_error_response(connection_t *conn, http_status_t status, const char *message) {
  // TODO: Implement this later
  return 0;
}

bool is_valid_uri(const char *uri) {
  // TODO: Implement this laters
  return false;
}

int http_server_cleanup(http_server_t *server) {
  if (!server) {
    return -1;
  }

  for (int i = 0; i < server->worker_count; i++) {
    server->workers[i].running = false;
    pthread_join(server->workers[i].thread, NULL);
    close(server->workers[i].epoll_fd);
  }

  // Close listening port
  if (server->listen_fd > 0) {
    close(server->listen_fd);
  }

  // Cleanup SSL
  if (server->ssl_enabled) {
    cleanup_ssl(server);
  }

  // Free resources
  free(server->document_root);
  free(server->server_name);

  // Cleanup routes
  route_t *route = server->routes;
  while (route) {
    route_t *next = route->next;
    free(route);
    route = next;
  }

  pthread_mutex_destroy(&server->stats_mutex);

  printf("HTTP server cleanup completed\n");
  return 0;
}
