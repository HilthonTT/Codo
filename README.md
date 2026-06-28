# Codo

Codo is a from-scratch multi-threaded HTTP/1.1 server written in C11, built directly on top of POSIX sockets and `epoll`. It uses an acceptor + worker-pool model: the main thread accepts connections and round-robins them to a fixed pool of worker threads, each running its own private `epoll` loop over a linked list of connections.

It ships with a **Todo CRUD web API** backed by a built-in B-tree storage engine (buffer pool, write-ahead log, transactions, checkpointing), so todos persist across restarts.

## Features

- HTTP/1.1 with keep-alive
- Edge-triggered `epoll` per worker
- Route table with per-method handlers + default static-file handler
- Trailing-`*` wildcard routes (e.g. `/api/todos/*`) in addition to exact matches
- Optional TLS via OpenSSL (auto-enabled when `server.crt` / `server.key` exist)
- WebSocket upgrade handshake (`Sec-WebSocket-Accept` via SHA1 + base64)
- gzip plumbing via zlib (scaffolded)
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

```
make             # default build
make debug       # -g -O0 -DDEBUG with ASan + UBSan
make release     # -O2 -DNDEBUG
make run         # build then run ./bin/codo
make clean
```

Output binary: `bin/codo`. Link deps: `-lssl -lcrypto -lz -lpthread`.

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

The storage engine runs a final checkpoint on shutdown (`SIGINT`/`SIGTERM`), so todos written in one run are visible on the next.

## Layout

```
include/    public headers (one per subsystem)
src/        implementation (main, server, worker, connection, http_protocol,
            route, ssl, websocket, compression, mime, util, handlers,
            btree_storage, todo_handlers)
build/      .o + .d files (auto-generated header deps via -MMD -MP)
bin/        output binary
scripts/    bash + curl helpers for the Todo API (create/list/get/update/delete)
tests/      reserved (no test harness yet)
```

The Todo API lives in `src/todo_handlers.c` (HTTP/JSON layer) on top of
`src/btree_storage.c` (the storage engine). `main.c` initializes the engine,
seeds the id counter, and registers the routes.

## Status

Work in progress. Some subsystems (gzip compression of response bodies, full WebSocket framing, file streaming with `sendfile`) are scaffolded but not fully wired into the response path.
