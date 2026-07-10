# Codo

Codo is a from-scratch multi-threaded HTTP/1.1 server written in C11, built directly on top of POSIX sockets and `epoll`. It uses an acceptor + worker-pool model: the main thread accepts connections and round-robins them to a fixed pool of worker threads, each running its own private `epoll` loop over a linked list of connections.

It ships with a **Todo CRUD web API** backed by a built-in B-tree storage engine (buffer pool, write-ahead log, transactions, checkpointing), so todos persist across restarts.

## Features

- HTTP/1.1 with keep-alive
- Edge-triggered `epoll` per worker
- Route table with per-method handlers + default static-file handler
- Trailing-`*` wildcard routes (e.g. `/api/todos/*`) in addition to exact matches
- Optional TLS via OpenSSL (auto-enabled when `server.crt` / `server.key` exist)
- WebSocket upgrade handshake (`Sec-WebSocket-Accept` via SHA1 + base64) plus full RFC 6455 frame handling (masked client frames, ping/pong, close, echo)
- gzip response compression via zlib, negotiated on `Accept-Encoding` for compressible content types (`Content-Encoding: gzip` + `Vary: Accept-Encoding`)
- Zero-copy static-file streaming with `sendfile(2)` on plaintext connections (no in-memory size cap)
- B-tree storage engine (pages + buffer pool + WAL + transactions) mounted as a JSON Todo API
- Live network statistics (atomic counters) exposed at `GET /api/stats`
- Example JSON endpoints: `/api/hello`, `/api/echo`, `/api/status`, `/api/stats`, `/ws/chat`

## Todo API

A todo is a JSON object: `{"id": <number>, "title": <string>, "completed": <bool>}`.
On `POST`/`PUT`, the body must supply `title` (required) and may supply `completed`
(defaults to `false`); `id` is assigned by the server. Ids are numeric and are
seeded from the highest id already on disk at startup.

| Method   | Path              | Description                       | Success      |
| -------- | ----------------- | --------------------------------- | ------------ |
| `GET`    | `/api/todos`      | List all todos (JSON array)       | `200`        |
| `POST`   | `/api/todos`      | Create a todo from the JSON body  | `201`        |
| `GET`    | `/api/todos/{id}` | Fetch a single todo               | `200`        |
| `PUT`    | `/api/todos/{id}` | Replace a todo                    | `200`        |
| `DELETE` | `/api/todos/{id}` | Delete a todo                     | `204`        |

Error responses: `400` (missing/invalid `title` or non-numeric id), `404` (no such todo).

```bash
# Create
curl -X POST localhost:8080/api/todos -d '{"title":"buy milk","completed":false}'
# -> 201 {"id":1,"title":"buy milk","completed":false}

# List / fetch / update / delete
curl localhost:8080/api/todos
curl localhost:8080/api/todos/1
curl -X PUT localhost:8080/api/todos/1 -d '{"title":"buy oat milk","completed":true}'
curl -X DELETE localhost:8080/api/todos/1
```

### Helper scripts

The `scripts/` folder wraps each CRUD operation in a small `curl` script so you can
drive the API without typing `curl` by hand. Each script targets `http://localhost:8080`
by default; set the `BASE_URL` environment variable to point somewhere else. They
require `bash` and `curl` (on Windows, run them from WSL or Git Bash).

| Script             | Endpoint                | Arguments                          | Notes                                                     |
| ------------------ | ----------------------- | ---------------------------------- | --------------------------------------------------------- |
| `create_todo.sh`   | `POST /api/todos`       | `"<title>" [completed]`            | `completed` defaults to `false`; prints the created todo  |
| `list_todos.sh`    | `GET /api/todos`        | _(none)_                           | Prints the JSON array of all todos                        |
| `get_todo.sh`      | `GET /api/todos/{id}`   | `<id>`                             | Prints the matching todo (or a `404` error body)          |
| `update_todo.sh`   | `PUT /api/todos/{id}`   | `<id> "<title>" [completed]`       | Replaces the todo; prints the updated todo                |
| `delete_todo.sh`   | `DELETE /api/todos/{id}`| `<id>`                             | Prints the HTTP status (`204` on success, `404` if absent)|

`create_todo.sh` and `update_todo.sh` JSON-escape the title for you, so titles with
spaces or quotes are safe to pass as a single argument.

```bash
# Make them executable once
chmod +x scripts/*.sh

scripts/create_todo.sh "buy milk"             # -> 201 {"id":1,"title":"buy milk","completed":false}
scripts/create_todo.sh "walk the dog" true    # title + completed
scripts/list_todos.sh                         # [ ...all todos... ]
scripts/get_todo.sh 1                         # {"id":1,...}
scripts/update_todo.sh 1 "buy oat milk" true  # {"id":1,"title":"buy oat milk","completed":true}
scripts/delete_todo.sh 1                      # HTTP 204

# Point the scripts at a different host/port
BASE_URL=http://localhost:9000 scripts/list_todos.sh
```

## Network stats

`GET /api/stats` returns a snapshot of process-wide network counters (lock-free
atomics updated on the accept / read / write / close paths):

```bash
curl localhost:8080/api/stats
# {"bytes_sent":926,"bytes_received":513,"packets_sent":4,"packets_received":5,
#  "connections_accepted":5,"connections_closed":4,"errors":0}
```

The counters live in `src/stats.c` (`include/stats.h`), which also carries socket
diagnostics helpers (`diagnose_network_issue`, `debug_packet_dump`) for debugging.

## Build

The repo is a small workspace: shared code is compiled once into a static
`libcommon.a`, the storage engine into `libstorage.a`, and each binary links
what it needs (the server links both; the balancer only `libcommon.a`).

```
make             # build both binaries (bin/codo + bin/codo-balancer)
make server      # build only the HTTP server
make balancer    # build only the load balancer
make debug       # both, -g -O0 -DDEBUG with ASan + UBSan
make release     # both, -O2 -DNDEBUG
make run         # build then run ./bin/codo
make run-balancer# build then run ./bin/codo-balancer
make clean
```

Output binaries: `bin/codo` (server) and `bin/codo-balancer` (balancer).
Link deps: `-lssl -lcrypto -lz -lpthread`.

## Run

```
./bin/codo [port] [document_root]
```

Defaults: port `8080`, document root `/var/www/html`. If `server.crt` and `server.key` are present in the working directory, TLS is enabled automatically.

Configuration is read from a `.env` file (override the path with `ENV_FILE`), then overridden by CLI args. Relevant keys:

| Key             | Default          | Purpose                              |
| --------------- | ---------------- | ------------------------------------ |
| `PORT`          | `8080`           | Listen port                          |
| `DOCUMENT_ROOT` | `/var/www/html`  | Static file root                     |
| `SSL_ENABLED`   | `true`           | Enable TLS when cert/key exist       |
| `DB_FILE`       | `codo.db`        | B-tree data file (Todo storage)      |
| `WAL_FILE`      | `codo.wal`       | Write-ahead log file                 |
| `BALANCER_PORT` | `8000`           | Listen port for `codo-balancer`      |
| `BALANCER_BACKENDS` | `127.0.0.1:8080` | Backends fronted by `codo-balancer` (`host:port[:weight]`, comma-separated) |

The storage engine runs a final checkpoint on shutdown (`SIGINT`/`SIGTERM`), so todos written in one run are visible on the next.

## Layout

The codebase is split into four components, each with its own `include/` +
`src/`. `common/` and `storage/` are libraries; `server/` and `balancer/` are
the two binaries built on top of them.

```
common/     shared infrastructure compiled into libcommon.a
  include/  config, http_types, env, util, stats, connection,
            compression, http_protocol, mime
  src/      env, util, stats, mime, compression, http_protocol,
            connection (cleanup + socket helpers)
storage/    embedded B-tree storage engine compiled into libstorage.a
  include/  storage.h — the public API (engine lifecycle, transactions,
            db_insert/search/update/delete/scan)
  src/      storage_internal.h (private structs, shared by the .c files below)
            engine   — singleton, init/cleanup, checkpoint, statistics
            wal      — write-ahead log append + flush
            pager    — buffer pool, page IO, free-page management, checksums
            btree    — page-level key search/insert/delete
            txn      — transaction begin/commit/abort
            db       — CRUD/scan API (tree descent over the layers above)
server/     the HTTP/1.1 server (bin/codo)
  include/  server, worker, route, handlers, todo_handlers,
            ssl_util, websocket, middleware
  src/      main, server, worker, connection_pool, route, handlers,
            todo_handlers, ssl_util, websocket, middleware
balancer/   epoll TCP load balancer (bin/codo-balancer)
  include/  balancer, types, selection, hash
  src/      main, balancer, selection, hash
build/      .o + .d files per component + libcommon.a + libstorage.a
bin/        output binaries (codo, codo-balancer)
scripts/    bash + curl helpers for the Todo API (create/list/get/update/delete)
tests/      reserved (no test harness yet)
```

The Todo API lives in `server/src/todo_handlers.c` (HTTP/JSON layer) on top of
the storage engine's public API (`storage/include/storage.h`). The engine's
internals — pages, buffer pool, WAL records, locking — are private to
`storage/src/`. `server/src/main.c` initializes the engine, seeds the id
counter, and registers the routes.

### What is shared vs. server-only

`common/` holds everything that does not depend on `http_server_t`: config and
env loading, the HTTP request parser / response writer (`http_protocol.c`),
generic socket helpers and per-connection teardown (`connection.c`), network
stats, MIME lookup, and gzip plumbing. The server adds the pieces bound to its
own state: the connection pool (`connection_pool.c`, which tracks live
connections in `http_server_t`), TLS context setup (`ssl_util.c`), the
worker/epoll loop, routing, and the Todo API.

Two deliberate seams keep `common/` free of server types:

- `http_protocol.c` fills the `Server:` response header via `http_server_name()`,
  a hook each binary defines for itself (the server returns its configured name).
- `connection.c` provides `cleanup_connection()` + socket helpers (no server
  state); `allocate_connection()` / `free_connection()`, which touch the pool,
  live in the server's `connection_pool.c`.

## Balancer

`balancer/` is a TCP load balancer (`bin/codo-balancer`) that fronts one or
more `codo` instances. It runs a single-threaded `epoll` event loop:
`load_balancer_init()` binds the listen socket and creates the epoll instance,
`lb_handle_new_connection()` accepts a client, picks a backend, and opens a
non-blocking connection to it, and the loop then proxies bytes in both
directions (`client -> backend` via `client_buffer`, `backend -> client` via
`backend_buffer`), pausing reads on one side when the other side's socket is not
yet writable.

Backend selection uses smooth weighted round-robin (`select_backend()`),
skipping any backend whose `health_status` is not healthy. Each fd registers an
`io_ctx_t` as its epoll `data.ptr`, so the loop knows which connection and which
side (`IO_CLIENT` / `IO_BACKEND` / `IO_LISTEN`) every event belongs to.

### Configuration

The balancer reads the same `.env` file as the server (override with
`ENV_FILE`). CLI args override the environment: `argv[1]` is the listen port and
`argv[2]` is the backend list.

| Key                 | Default          | Purpose                                          |
| ------------------- | ---------------- | ------------------------------------------------ |
| `BALANCER_PORT`     | `8000`           | Listen port for `codo-balancer`                  |
| `BALANCER_BACKENDS` | `127.0.0.1:8080` | Comma-separated backend list: `host:port[:weight]` |

```sh
make balancer
BALANCER_PORT=8000 BALANCER_BACKENDS=127.0.0.1:8080,127.0.0.1:8081:2 ./bin/codo-balancer
# or override on the command line:
./bin/codo-balancer 8000 127.0.0.1:8080,127.0.0.1:8081:2
```

Each backend is `host:port` with an optional `:weight` suffix (default `1`); a
higher weight receives proportionally more connections.

## Status

Work in progress, but the previously-scaffolded response-path features are now
wired end-to-end:

- **gzip compression** — `send_http_response()` compresses the body when the
  client sends `Accept-Encoding: gzip`, the content type is compressible
  (text/JSON/JS/XML/SVG), and the body is at least 256 bytes, emitting
  `Content-Encoding: gzip` and `Vary: Accept-Encoding`. Static text assets are
  read-and-compressed; binary/opaque files skip it.
- **`sendfile` streaming** — static files on plaintext connections are streamed
  zero-copy straight from the fd to the socket in `handle_client_write()`, with
  no in-memory size cap. TLS connections (which can't hand a raw fd to OpenSSL)
  and small compressible assets bound for gzip take the buffered path instead.
- **WebSocket framing** — the RFC 6455 frame engine (`ws_process_frames()`)
  decodes masked client frames and replies with echoes, pongs, and close frames;
  `/ws/chat` is a working echo endpoint.
