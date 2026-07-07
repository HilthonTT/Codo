#include "balancer.h"

#include <netinet/tcp.h>
#include <sys/uio.h>

// Forward declarations for the file-local proxy helpers.
static void flush_client_to_backend(load_balancer_t *lb, connection_t *conn);
static void flush_backend_to_client(load_balancer_t *lb, connection_t *conn);

// Passive-health-check helpers. Every failure attributable to the backend
// bumps consecutive_failures; once we cross the threshold the backend is
// removed from rotation. Any successful data exchange resets the counter.
// Callers must NOT already hold backend_mutex.
static void mark_backend_failure(load_balancer_t *lb, backend_t *backend)
{
    if (!backend)
    {
        return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    pthread_mutex_lock(&lb->backend_mutex);
    backend->failed_requests++;
    backend->consecutive_failures++;
    // Use CLOCK_MONOTONIC so the recovery timer isn't affected by wall-clock
    // jumps (NTP steps, manual date changes).
    backend->last_health_check = (time_t)ts.tv_sec;
    if (backend->consecutive_failures >= HEALTH_FAILURE_THRESHOLD)
    {
        if (backend->health_status == 1)
        {
            fprintf(stderr,
                    "backend %s:%d marked unhealthy after %d consecutive failures\n",
                    inet_ntoa(backend->addr.sin_addr),
                    ntohs(backend->addr.sin_port),
                    backend->consecutive_failures);
        }
        backend->health_status = 0;
    }
    pthread_mutex_unlock(&lb->backend_mutex);
}

static void mark_backend_success(load_balancer_t *lb, backend_t *backend)
{
    if (!backend)
        return;
    pthread_mutex_lock(&lb->backend_mutex);
    if (backend->consecutive_failures != 0 || backend->health_status != 1)
    {
        backend->consecutive_failures = 0;
        backend->health_status = 1;
    }
    pthread_mutex_unlock(&lb->backend_mutex);
}

// Send a minimal HTTP 503 to a raw client fd. Best-effort; the fd is closed
// by the caller. Used when no healthy backend is available.
static void send_503(int client_fd)
{
    static const char body[] =
        "HTTP/1.1 503 Service Unavailable\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 19\r\n"
        "Connection: close\r\n"
        "\r\n"
        "No backend available";
    ssize_t total = 0, len = (ssize_t)sizeof(body) - 1;
    while (total < len)
    {
        ssize_t n = write(client_fd, body + total, (size_t)(len - total));
        if (n > 0)
        {
            total += n;
            continue;
        }
        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break; // client can't keep up; give up rather than block
        break;
    }
}

static void update_epoll_interest(load_balancer_t *lb, connection_t *conn)
{
    uint32_t client_events = 0, backend_events = 0;

    bool c2b_pending = conn->client_buffer_len > conn->client_buffer_sent;
    bool b2c_pending = conn->backend_buffer_len > conn->backend_buffer_sent;

    // only read a fresh chunk from a side once its outbound buffer is drained.
    if (!c2b_pending)
    {
        client_events |= EPOLLIN; // willing to read from client
    }
    if (!b2c_pending)
    {
        backend_events |= EPOLLIN;
    }

    // only wait for writeability when bytes are actually queued for that fd.
    if (c2b_pending)
    {
        backend_events |= EPOLLOUT; // data waiting for backend
    }
    if (b2c_pending)
    {
        client_events |= EPOLLOUT; // data waiting for client
    }

    modify_epoll_events(lb, &conn->client_ctx, client_events);
    modify_epoll_events(lb, &conn->backend_ctx, backend_events);
}

// High-performance load balancer main loop
int load_balancer_main_loop(load_balancer_t *lb)
{
    int nfds, i;

    while (true)
    {
        nfds = epoll_wait(lb->epoll_fd, lb->events, MAX_EVENTS, -1);
        if (nfds == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }
            perror("epoll_wait");
            break;
        }

        for (i = 0; i < nfds; i++)
        {
            struct epoll_event *event = &lb->events[i];
            io_ctx_t *ctx = (io_ctx_t *)event->data.ptr;

            // New client connection on the listen socket.
            if (ctx->role == IO_LISTEN)
            {
                handle_new_connection(lb);
                continue;
            }

            connection_t *conn = ctx->conn;

            // Hangup / error tears the whole connection down regardless of the
            // other reported events, so handle it first and move on.
            if (event->events & (EPOLLHUP | EPOLLERR))
            {
                handle_connection_error(lb, conn);
                continue;
            }

            // A handler may close (and free) conn, so dispatch a single side per
            // event. epoll will re-report any side that is still ready.
            if (event->events & EPOLLIN)
            {
                if (ctx->role == IO_CLIENT)
                {
                    handle_client_data(lb, conn);
                }
                else
                {
                    handle_backend_data(lb, conn);
                }
            }
            else if (event->events & EPOLLOUT)
            {
                if (ctx->role == IO_CLIENT)
                {
                    handle_client_write(lb, conn);
                }
                else
                {
                    handle_backend_write(lb, conn);
                }
            }
        }
    }

    return 0;
}

// Bind/listen, create the epoll instance and register the listen socket.
int load_balancer_init(load_balancer_t *lb, int port)
{
    memset(lb, 0, sizeof(*lb));
    lb->listen_fd = -1;
    lb->epoll_fd = -1;
    pthread_mutex_init(&lb->backend_mutex, NULL);

    lb->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (lb->epoll_fd == -1)
    {
        perror("epoll_create1");
        return -1;
    }

    lb->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (lb->listen_fd == -1)
    {
        perror("socket");
        return -1;
    }

    int opt = 1;
    setsockopt(lb->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (set_nonblocking(lb->listen_fd) == -1)
    {
        perror("set_nonblocking");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    if (bind(lb->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1)
    {
        perror("bind");
        return -1;
    }

    if (listen(lb->listen_fd, SOMAXCONN) == -1)
    {
        perror("listen");
        return -1;
    }

    // Register the listen socket with its own context so the main loop can
    // recognise it via data.ptr like every other fd.
    lb->listen_ctx.fd = lb->listen_fd;
    lb->listen_ctx.role = IO_LISTEN;
    lb->listen_ctx.conn = NULL;
    lb->strategy = LB_STRATEGY_ROUND_ROBIN;

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = &lb->listen_ctx;
    if (epoll_ctl(lb->epoll_fd, EPOLL_CTL_ADD, lb->listen_fd, &ev) == -1)
    {
        perror("epoll_ctl: listen_fd");
        return -1;
    }

    return 0;
}

// Register a backend server (resolved as a dotted-quad IPv4 address).
int add_backend(load_balancer_t *lb, const char *host, int port, int weight)
{
    if (lb->backend_count >= MAX_BACKENDS)
    {
        fprintf(stderr, "too many backends (max %d)\n", MAX_BACKENDS);
        return -1;
    }

    backend_t *backend = &lb->backends[lb->backend_count];
    memset(backend, 0, sizeof(*backend));

    backend->addr.sin_family = AF_INET;
    backend->addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &backend->addr.sin_addr) != 1)
    {
        fprintf(stderr, "invalid backend host: %s\n", host);
        return -1;
    }

    backend->weight = weight > 0 ? weight : 1;
    backend->current_weight = 0;
    backend->health_status = 1; // assume healthy until a health check says otherwise

    lb->backend_count++;
    return 0;
}

// Handle new client connection
void handle_new_connection(load_balancer_t *lb)
{
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd, backend_fd;
    backend_t *backend;
    connection_t *conn;

    // Accept client connection
    client_fd = accept(lb->listen_fd, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd == -1)
    {
        perror("accept");
        return;
    }

    // Set non-blocking
    if (set_nonblocking(client_fd) == -1)
    {
        perror("set_nonblocking(client_fd)");
        close(client_fd);
        return;
    }

    // Enable TCP_NODELAY on the client side so short request/response pairs
    // aren't delayed by Nagle's algorithm.
    int one = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    // Select backend using load balancing algorithm
    backend = select_backend(lb, &client_addr);
    if (!backend)
    {
        // All backends are unhealthy — reply 503 instead of a silent close so
        // clients get an actionable error.
        send_503(client_fd);
        close(client_fd);
        return;
    }

    // Connect to backend
    backend_fd = create_backend_connection(backend);
    if (backend_fd == -1)
    {
        // Immediate connect() failure (not EINPROGRESS) — that's the backend's
        // fault, so count it against health.
        mark_backend_failure(lb, backend);
        close(client_fd);
        return;
    }
    setsockopt(backend_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    // Create connection structure
    conn = malloc(sizeof(connection_t));
    if (!conn)
    {
        close(client_fd);
        close(backend_fd);
        return;
    }
    memset(conn, 0, sizeof(connection_t));
    conn->client_fd = client_fd;
    conn->backend_fd = backend_fd;
    conn->backend = backend;
    clock_gettime(CLOCK_MONOTONIC, &conn->start_time);

    // Add to epoll
    if (add_connection_to_epoll(lb, conn) == -1)
    {
        close(client_fd);
        close(backend_fd);
        free(conn);
        return;
    }

    // Update backend stats
    pthread_mutex_lock(&lb->backend_mutex);
    backend->current_connections++;
    backend->total_requests++;
    pthread_mutex_unlock(&lb->backend_mutex);
}

backend_t *select_backend(load_balancer_t *lb, const struct sockaddr_in *client_addr)
{
    switch (lb->strategy)
    {
    case LB_STRATEGY_ROUND_ROBIN:
        return backend_round_robin_select(lb);
    case LB_STRATEGY_LEAST_CONN:
        return least_connection_select(lb);
    case LB_STRATEGY_IP_HASH:
        return ip_hash_select(lb, client_addr);
    case LB_STRATEGY_RANDOM:
        return random_select(lb);
    default:
        return backend_round_robin_select(lb);
    }
}

// Client -> backend: read a chunk from the client and forward it.
void handle_client_data(load_balancer_t *lb, connection_t *conn)
{
    ssize_t bytes_read;

    bytes_read = read(conn->client_fd, conn->client_buffer, BUFFER_SIZE);
    if (bytes_read <= 0)
    {
        if (bytes_read == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
        {
            close_connection(lb, conn);
        }
        return;
    }

    conn->client_buffer_len = (size_t)bytes_read;
    conn->client_buffer_sent = 0;
    flush_client_to_backend(lb, conn);
}

// Backend -> client: read a chunk from the backend and forward it.
void handle_backend_data(load_balancer_t *lb, connection_t *conn)
{
    ssize_t bytes_read;

    bytes_read = read(conn->backend_fd, conn->backend_buffer, BUFFER_SIZE);
    if (bytes_read <= 0)
    {
        if (bytes_read == 0)
        {
            // Clean EOF from backend — normal close of the response.
            close_connection(lb, conn);
        }
        else if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
            // Read error from backend — attribute to backend health.
            mark_backend_failure(lb, conn->backend);
            close_connection(lb, conn);
        }
        return;
    }

    // We successfully received data from this backend — reset failure count.
    mark_backend_success(lb, conn->backend);

    conn->backend_buffer_len = (size_t)bytes_read;
    conn->backend_buffer_sent = 0;
    flush_backend_to_client(lb, conn);
}

// The client socket is writable again: drain whatever the backend produced.
void handle_client_write(load_balancer_t *lb, connection_t *conn)
{
    flush_backend_to_client(lb, conn);
}

// The backend socket is writable again (also fires when a non-blocking connect
// completes): drain whatever the client sent.
void handle_backend_write(load_balancer_t *lb, connection_t *conn)
{
    flush_client_to_backend(lb, conn);
}

void handle_connection_error(load_balancer_t *lb, connection_t *conn)
{
    // We don't know which side raised the error, but check the backend fd's
    // pending socket error explicitly — a refused/reset backend is the most
    // common cause and the only one worth demoting a backend for.
    int soerr = 0;
    socklen_t slen = sizeof(soerr);
    if (getsockopt(conn->backend_fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) == 0 && soerr != 0)
    {
        mark_backend_failure(lb, conn->backend);
    }
    close_connection(lb, conn);
}

// Push the buffered client->backend bytes to the backend. If the backend blocks
// we stop reading the client and wait for the backend to become writable; once
// fully drained we restore normal read interest on both sides.
static void flush_client_to_backend(load_balancer_t *lb, connection_t *conn)
{
    while (conn->client_buffer_sent < conn->client_buffer_len)
    {
        ssize_t n = write(conn->backend_fd,
                          conn->client_buffer + conn->client_buffer_sent,
                          conn->client_buffer_len - conn->client_buffer_sent);
        if (n > 0)
        {
            conn->client_buffer_sent += (size_t)n;
            continue;
        }

        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            update_epoll_interest(lb, conn);
            return;
        }

        // Write to backend failed for a non-transient reason — the backend
        // is the responsible party, so this counts against its health.
        mark_backend_failure(lb, conn->backend);
        close_connection(lb, conn);
        return;
    }

    // Fully forwarded: resume reading the client, stop waiting for writability.
    conn->client_buffer_len = 0;
    conn->client_buffer_sent = 0;
    update_epoll_interest(lb, conn);
}

// Mirror of flush_client_to_backend for the backend->client direction.
static void flush_backend_to_client(load_balancer_t *lb, connection_t *conn)
{
    while (conn->backend_buffer_sent < conn->backend_buffer_len)
    {
        ssize_t n = write(conn->client_fd,
                          conn->backend_buffer + conn->backend_buffer_sent,
                          conn->backend_buffer_len - conn->backend_buffer_sent);
        if (n > 0)
        {
            conn->backend_buffer_sent += (size_t)n;
            continue;
        }

        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            update_epoll_interest(lb, conn);
            return;
        }

        close_connection(lb, conn);
        return;
    }

    conn->backend_buffer_len = 0;
    conn->backend_buffer_sent = 0;
    update_epoll_interest(lb, conn);
}

void close_connection(load_balancer_t *lb, connection_t *conn)
{
    struct timespec end_time;
    long long response_time;

    // Calculate response time in ms. Normalize the nsec borrow so we don't
    // produce a negative contribution when end.tv_nsec < start.tv_nsec.
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    long sec = (long)(end_time.tv_sec - conn->start_time.tv_sec);
    long nsec = end_time.tv_nsec - conn->start_time.tv_nsec;
    if (nsec < 0)
    {
        sec -= 1;
        nsec += 1000000000L;
    }
    response_time = (long long)sec * 1000 + nsec / 1000000;
    if (response_time < 0)
    {
        response_time = 0; // clock went backwards; don't poison the EMA
    }

    // Update backend statistics
    pthread_mutex_lock(&lb->backend_mutex);
    conn->backend->current_connections--;

    // Update average response time (exponential moving average)
    if (conn->backend->avg_response_time == 0)
    {
        conn->backend->avg_response_time = response_time;
    }
    else
    {
        conn->backend->avg_response_time =
            (conn->backend->avg_response_time * 9 + response_time) / 10;
    }
    pthread_mutex_unlock(&lb->backend_mutex);

    // Remove from epoll and close sockets
    epoll_ctl(lb->epoll_fd, EPOLL_CTL_DEL, conn->client_fd, NULL);
    epoll_ctl(lb->epoll_fd, EPOLL_CTL_DEL, conn->backend_fd, NULL);
    close(conn->client_fd);
    close(conn->backend_fd);
    free(conn);
}

int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int create_backend_connection(backend_t *backend)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1)
        return -1;

    if (set_nonblocking(fd) == -1)
    {
        close(fd);
        return -1;
    }

    // Non-blocking connect: EINPROGRESS is expected, not an error
    if (connect(fd, (struct sockaddr *)&backend->addr, sizeof(backend->addr)) == -1 && errno != EINPROGRESS)
    {
        close(fd);
        return -1;
    }

    return fd;
}

// Register both ends of a freshly-accepted connection with epoll.
int add_connection_to_epoll(load_balancer_t *lb, connection_t *conn)
{
    conn->client_ctx.fd = conn->client_fd;
    conn->client_ctx.role = IO_CLIENT;
    conn->client_ctx.conn = conn;

    conn->backend_ctx.fd = conn->backend_fd;
    conn->backend_ctx.role = IO_BACKEND;
    conn->backend_ctx.conn = conn;

    struct epoll_event ev;

    ev.events = EPOLLIN;
    ev.data.ptr = &conn->client_ctx;
    if (epoll_ctl(lb->epoll_fd, EPOLL_CTL_ADD, conn->client_fd, &ev) == -1)
    {
        perror("epoll_ctl: client_fd");
        return -1;
    }

    ev.events = EPOLLIN;
    ev.data.ptr = &conn->backend_ctx;
    if (epoll_ctl(lb->epoll_fd, EPOLL_CTL_ADD, conn->backend_fd, &ev) == -1)
    {
        perror("epoll_ctl: backend_fd");
        epoll_ctl(lb->epoll_fd, EPOLL_CTL_DEL, conn->client_fd, NULL);
        return -1;
    }

    return 0;
}

// Change the epoll interest set for one side of a connection. The owning
// io_ctx_t must be re-supplied as data.ptr on every EPOLL_CTL_MOD.
int modify_epoll_events(load_balancer_t *lb, io_ctx_t *ctx, uint32_t events)
{
    struct epoll_event ev;
    ev.events = events;
    ev.data.ptr = ctx;
    return epoll_ctl(lb->epoll_fd, EPOLL_CTL_MOD, ctx->fd, &ev);
}
