# Codo

Codo is a from-scratch HTTP/1.1 server written in C11 on top of POSIX sockets and `epoll` — no web framework, no external HTTP library. It ships with a persistent Todo REST API backed by its own B-tree storage engine (buffer pool, write-ahead log, transactions), and a companion TCP load balancer that can front several instances.

Everything except TLS (OpenSSL) and gzip (zlib) is implemented in-tree: the HTTP parser, the router, the middleware chain, the thread pool, the LRU cache, the WebSocket framing, and the storage engine.

```
make && ./bin/codo
# HTTP server starting on port 8080
```

Linux only — the hot paths use `epoll(7)` and `sendfile(2)`.

## How a request flows

Codo splits work across three thread groups so that a slow disk never stalls the network.

1. **Accept loop** (main thread) — accepts connections and round-robins them across the workers.
2. **Workers** (8 threads, `WORKER_THREADS`) — each owns a private edge-triggered `epoll` loop over its own connections. It parses the request, matches a route, and either runs the handler inline or hands it off.
3. **Storage pool** (16 threads, `STORAGE_POOL_THREADS`) — runs the *blocking* handlers.

The handoff is the interesting part. A handler that touches the storage engine (any Todo route) or the disk (the static-file handler) is registered with `add_route_offloaded()`. When one of those matches, the worker drops the connection out of its `epoll` interest set, flags it `offloaded`, and submits the handler to the pool; the pool thread runs it and re-arms `EPOLLOUT` when the response is ready. The worker's loop is never blocked on an `fsync` or a page read.

The pool has **two priority levels**: reads are queued above writes (`STORAGE_PRIORITY_READ` > `STORAGE_PRIORITY_WRITE`), so a `GET` doesn't wait behind a burst of WAL-fsyncing writes. It is deliberately oversubscribed relative to the core count — those threads spend most of their life parked in `fsync`/`pread`, so extra threads keep the CPU busy rather than idle.

Every handler runs inside a **middleware chain** (`logging` → `cors` → handler). Middleware is registered in `main.c` and wraps every route, including the default file handler.

## Features

**HTTP**
- HTTP/1.1 with keep-alive and a 30s idle timeout
- Edge-triggered `epoll`, one loop per worker thread
- Route table with per-method handlers, trailing-`*` wildcards (`/api/todos/*`), and a default static-file handler
- Composable middleware chain — request logging (with timing) and CORS, including `OPTIONS` preflight
- Optional TLS via OpenSSL, auto-enabled when the cert and key exist
- WebSocket upgrade + full RFC 6455 frame handling (masked frames, ping/pong, close) — `/ws/chat` is a working echo endpoint

**Performance**
- Thread-pool offload of blocking handlers, with read-over-write priority queueing
- Zero-copy static-file streaming via `sendfile(2)` on plaintext connections, no in-memory size cap
- gzip compression negotiated on `Accept-Encoding`, for compressible content types over 256 bytes
- Read-through LRU cache in front of the storage engine for single-todo reads
- Lock-free atomic counters on the accept/read/write/close paths

**Storage**
- Embedded B-tree engine: pages, buffer pool, write-ahead log, transactions, checkpointing
- Final checkpoint on `SIGINT`/`SIGTERM`, so todos survive a restart

## Build

Shared code compiles once into `libcommon.a`, the storage engine into `libstorage.a`, and each binary links what it needs (the server links both; the balancer only needs `libcommon.a`).

```sh
make              # both binaries -> bin/codo + bin/codo-balancer
make server       # just the HTTP server
make balancer     # just the load balancer
make debug        # -g -O0 -DDEBUG with ASan + UBSan
make release      # -O2 -DNDEBUG
make run          # build, then run ./bin/codo
make run-balancer # build, then run ./bin/codo-balancer
make clean
```

Requires `gcc`, OpenSSL and zlib headers (`libssl-dev`, `zlib1g-dev` on Debian/Ubuntu). Links against `-lssl -lcrypto -lz -lpthread`.

## Run

```sh
./bin/codo [port] [document_root]
```

Configuration is read from `.env` (override the path with `ENV_FILE`), then overridden by the real environment, then by CLI args. TLS turns on automatically when `SSL_ENABLED` is set and both the cert and key files exist.

| Key                  | Default          | Purpose                                                     |
| -------------------- | ---------------- | ----------------------------------------------------------- |
| `PORT`               | `8080`           | Listen port                                                  |
| `DOCUMENT_ROOT`      | `/var/www/html`  | Static file root (the bundled `.env` points it at `www`)      |
| `SSL_ENABLED`        | `true`           | Enable TLS when the cert and key are present                 |
| `SSL_CERT_FILE`      | `server.crt`     | TLS certificate                                              |
| `SSL_KEY_FILE`       | `server.key`     | TLS private key                                              |
| `CORS_ALLOW_ORIGIN`  | `*`              | Value echoed in `Access-Control-Allow-Origin`                |
| `DB_FILE`            | `codo.db`        | B-tree data file                                             |
| `WAL_FILE`           | `codo.wal`       | Write-ahead log                                              |
| `TODO_CACHE_CAPACITY`| `1024`           | Max entries in the todo read cache                           |
| `BALANCER_PORT`      | `8000`           | Listen port for `codo-balancer`                              |
| `BALANCER_BACKENDS`  | `127.0.0.1:8080` | Backends to front: `host:port[:weight]`, comma-separated     |

Compile-time limits (thread counts, buffer sizes, connection caps) live in `common/include/config.h`.

## Endpoints

| Method   | Path              | Description                                    |
| -------- | ----------------- | ---------------------------------------------- |
| `GET`    | `/api/hello`      | Hello-world JSON                               |
| `POST`   | `/api/echo`       | Echoes the request body back                   |
| `GET`    | `/api/status`     | Server status                                  |
| `GET`    | `/api/stats`      | Network counters (see below)                   |
| `GET`    | `/api/cache`      | Todo cache hit/miss counters                   |
| `GET`    | `/ws/chat`        | WebSocket echo endpoint                        |
| —        | `/api/todos[/id]` | Todo CRUD — see below                          |
| `GET`    | `/*`              | Static files from `DOCUMENT_ROOT`              |

### Todo API

A todo is `{"id": <number>, "title": <string>, "completed": <bool>}`. On `POST`/`PUT` the body must supply `title`; `completed` defaults to `false`. Ids are assigned by the server and seeded at startup from the highest id already on disk.

| Method   | Path              | Description                      | Success |
| -------- | ----------------- | -------------------------------- | ------- |
| `GET`    | `/api/todos`      | List all todos (JSON array)      | `200`   |
| `POST`   | `/api/todos`      | Create a todo from the JSON body | `201`   |
| `GET`    | `/api/todos/{id}` | Fetch a single todo              | `200`   |
| `PUT`    | `/api/todos/{id}` | Replace a todo                   | `200`   |
| `DELETE` | `/api/todos/{id}` | Delete a todo                    | `204`   |

Errors: `400` (missing/invalid `title`, or a non-numeric id), `404` (no such todo), `413` (todo too large).

```sh
curl -X POST localhost:8080/api/todos -d '{"title":"buy milk","completed":false}'
# 201 {"id":1,"title":"buy milk","completed":false}

curl localhost:8080/api/todos
curl localhost:8080/api/todos/1
curl -X PUT localhost:8080/api/todos/1 -d '{"title":"buy oat milk","completed":true}'
curl -X DELETE localhost:8080/api/todos/1
```

**Caching.** A single-todo `GET` otherwise costs a transaction plus a B-tree descent through the buffer pool, so it reads through a fixed-capacity LRU cache instead. Writes keep the cache in step: create and update store the new JSON, delete drops the entry. A cache fill that races a concurrent write is discarded rather than resurrecting a stale value — the cache carries a generation counter that every write bumps, and a fill whose snapshot is stale is dropped.

The collection `GET` is deliberately **not** cached: it's a full scan that any write would invalidate, so caching it would trade a scan for a near-permanent miss.

```sh
curl localhost:8080/api/cache
# {"entries":3,"capacity":1024,"hits":41,"misses":7,"evictions":0,"hit_rate":0.8542}
```

### Network stats

`GET /api/stats` snapshots process-wide atomic counters updated on the accept / read / write / close paths (`common/src/stats.c`, which also carries `diagnose_network_issue` and `debug_packet_dump` for debugging).

```sh
curl localhost:8080/api/stats
# {"bytes_sent":926,"bytes_received":513,"packets_sent":4,"packets_received":5,
#  "connections_accepted":5,"connections_closed":4,"errors":0}
```

### Helper scripts

`scripts/` wraps each CRUD operation in a small `curl` script. They target `http://localhost:8080` unless you set `BASE_URL`, and need `bash` + `curl` (on Windows: WSL or Git Bash).

| Script             | Arguments                    | Notes                                          |
| ------------------ | ---------------------------- | ---------------------------------------------- |
| `create_todo.sh`   | `"<title>" [completed]`      | Prints the created todo                        |
| `list_todos.sh`    | _(none)_                     | Prints the JSON array of all todos             |
| `get_todo.sh`      | `<id>`                       | Prints the matching todo, or a `404` body      |
| `update_todo.sh`   | `<id> "<title>" [completed]` | Replaces the todo, prints the result           |
| `delete_todo.sh`   | `<id>`                       | Prints the status (`204`, or `404` if absent)  |

`create_todo.sh` and `update_todo.sh` JSON-escape the title, so titles with spaces or quotes are safe to pass as one argument.

```sh
chmod +x scripts/*.sh
scripts/create_todo.sh "buy milk"              # 201 {"id":1,...}
scripts/update_todo.sh 1 "buy oat milk" true   # {"id":1,"title":"buy oat milk","completed":true}
BASE_URL=http://localhost:9000 scripts/list_todos.sh
```

## Demo site

`www/` is a small self-hosted demo served straight out of the document root: `index.html` exercises the Todo API from the browser, `docs.html` renders the API reference, and `openapi.json` is the OpenAPI description. The bundled `.env` already sets `DOCUMENT_ROOT=www`, so `make run` and then <http://localhost:8080> gives you a working UI.

## Load balancer

`bin/codo-balancer` is a TCP load balancer that fronts one or more `codo` instances. It runs a single-threaded `epoll` loop: it accepts a client, picks a backend, opens a non-blocking connection to it, and proxies bytes in both directions, pausing reads on one side when the other side isn't writable yet. Each fd registers an `io_ctx_t` as its epoll `data.ptr`, so the loop knows which connection and which side (`IO_CLIENT` / `IO_BACKEND` / `IO_LISTEN`) an event belongs to.

Four selection strategies are implemented — smooth weighted round-robin (nginx-style, the default), least-connections, IP hash, and random — chosen by the `strategy` field on the balancer. Health checking is **passive**: a backend is marked unhealthy after 3 consecutive failures (connect refused, read/write errors, `EPOLLERR`/`HUP`) and re-admitted 30s later to prove itself again. Unhealthy backends are skipped during selection.

```sh
make balancer
./bin/codo-balancer 8000 127.0.0.1:8080,127.0.0.1:8081:2
# or from the environment:
BALANCER_PORT=8000 BALANCER_BACKENDS=127.0.0.1:8080,127.0.0.1:8081:2 ./bin/codo-balancer
```

It reads the same `.env` as the server. CLI args override it: `argv[1]` is the listen port, `argv[2]` the backend list. Each backend is `host:port` with an optional `:weight` suffix (default `1`); a higher weight takes proportionally more connections.

## Layout

Four components, each with its own `include/` + `src/`. `common/` and `storage/` are libraries; `server/` and `balancer/` are the binaries built on top of them.

```
common/     shared infrastructure -> libcommon.a
  config, env, util, stats, mime, connection (socket helpers + teardown),
  http_protocol (request parser + response writer), compression (gzip),
  thread_pool (priority task queues; work stealing exists but is off),
  lru (the read cache), lockfree (queue/hashtable/skiplist -- see note below)
storage/    embedded B-tree engine -> libstorage.a
  storage.h  public API: engine lifecycle, transactions, insert/search/update/delete/scan
  engine     singleton init/cleanup, checkpoint, statistics
  wal        write-ahead log append + flush
  pager      buffer pool, page IO, free-page management, checksums
  btree      page-level key search/insert/delete
  txn        transaction begin/commit/abort
  db         CRUD/scan API (tree descent over the layers above)
server/     the HTTP/1.1 server -> bin/codo
  main, server (accept loop), worker (epoll loop + offload), connection_pool,
  route, middleware, handlers, todo_handlers, ssl_util, websocket
balancer/   epoll TCP load balancer -> bin/codo-balancer
  main, balancer (event loop + proxying), selection (strategies), hash
www/        bundled demo site + OpenAPI description
scripts/    bash + curl helpers for the Todo API
build/      objects, dep files, libcommon.a, libstorage.a
bin/        output binaries
```

The Todo API lives in `server/src/todo_handlers.c` (the HTTP/JSON layer) on top of the storage engine's public API. The engine's internals — pages, buffer pool, WAL records, locking — are private to `storage/src/storage_internal.h`.

### What is shared vs. server-only

`common/` holds everything that doesn't depend on `http_server_t`. Two deliberate seams keep it free of server types:

- `http_protocol.c` fills the `Server:` response header via `http_server_name()`, a hook each binary defines for itself.
- `connection.c` provides `cleanup_connection()` and socket helpers (no server state); `allocate_connection()` / `free_connection()`, which touch the pool, live in the server's `connection_pool.c`.

## CI

GitHub Actions (`.github/workflows/ci.yml`) runs on every push and PR to `main`:

- **Build & smoke test** — builds, boots the server on a throwaway port, and asserts that a static file serves, `/api/hello` returns `200`, an unknown path `404`s, and the logging middleware recorded both.
- **Sanitizer build** — compiles the whole tree with ASan + UBSan (`make debug`).

## Notes and limitations

- **Linux only.** `epoll` and `sendfile` are used directly; there's no portability layer.
- **No test suite.** Correctness is currently covered only by the CI smoke test and the sanitizer build.
- **`common/src/lockfree.c` is not wired up.** It implements a lock-free queue, hash table and skip list with hazard-pointer reclamation, and compiles into `libcommon.a`, but nothing links against it yet — it's groundwork, not a load-bearing part of the request path.
- `db_update()` only handles same-length values, so a todo update is implemented as a delete + insert inside one transaction.
- The JSON parser is hand-rolled and object-shaped: it reads top-level string and boolean fields, and collapses `\uXXXX` escapes to `?`.
