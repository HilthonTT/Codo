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

Every handler runs inside a **middleware chain** (`logging` → `metrics` → `cors` → `rate_limit` → `auth` → `jwt` → handler). Middleware is registered in `main.c` and wraps every route, including the default file handler. The order is deliberate: logging (outermost) times the whole chain; metrics records the final status and latency of every request, including ones a later stage short-circuits; CORS answers `OPTIONS` preflight before rate limiting or auth can reject it; rate limiting sheds excess load before the server spends effort on auth; API-key auth guards mutating routes outside the JWT paths; the JWT middleware (innermost) verifies bearer tokens for the per-user routes and hands the verified identity to the handler.

## Features

**HTTP**
- HTTP/1.1 with keep-alive and a 30s idle timeout
- Edge-triggered `epoll`, one loop per worker thread
- Conditional requests on static files: strong `ETag` (mtime + size) and `Last-Modified` on every file response, answered with `304 Not Modified` on a matching `If-None-Match` / `If-Modified-Since` (gzipped responses carry a weak `W/` etag, since the encoded bytes differ)
- Byte ranges: a single `Range: bytes=` (including `N-`, `-N` suffix forms) returns `206 Partial Content` with `Content-Range`, validated against `If-Range`, `416` when unsatisfiable — served through the same zero-copy `sendfile` window
- Chunked request bodies: `Transfer-Encoding: chunked` is decoded (extensions and trailers tolerated); a message carrying both `Content-Length` and `Transfer-Encoding` is refused with `400` as a request-smuggling guard, and any non-chunked coding gets `501`
- `Expect: 100-continue`: the interim `100 Continue` is written as soon as the headers arrive, so clients that wait before sending a large body aren't stuck for their retry timeout
- Route table with per-method handlers, trailing-`*` wildcards (`/api/todos/*`), and a default static-file handler
- Composable middleware chain — request logging (with timing), metrics, CORS (including `OPTIONS` preflight), rate limiting, API-key auth, and JWT auth
- Optional TLS via OpenSSL, auto-enabled when the cert and key exist
- WebSocket upgrade + full RFC 6455 frame handling (masked frames, ping/pong, close) — `/ws/chat` is a working echo endpoint

**Middleware & policy**
- Token-bucket **rate limiting** per client IP, refilled at `RATE_LIMIT_RPS`/s up to a `RATE_LIMIT_BURST` allowance; over-limit requests get `429 Too Many Requests` with `Retry-After`. Buckets live in a fixed open-addressing table with bounded, LRU-evicting probes, so the check stays O(1) and the memory footprint is constant
- **User accounts + JWT auth** for the Todo API: `POST /api/auth/register` and `POST /api/auth/login` issue HS256 JWTs; passwords are hashed with PBKDF2-HMAC-SHA256 (100k iterations, per-user salt, via the crypto framework in `common/src/crypto.c`) and every `/api/todos*` request needs a `Authorization: Bearer <jwt>` — each user sees only their own todos
- **API-key auth** on mutating verbs (`POST`/`PUT`/`DELETE`/`PATCH`) outside the JWT paths: keys come from `API_KEYS`, presented as `X-API-Key` or `Authorization: Bearer`, checked in constant time; reads stay public and auth is off entirely when no keys are set
- **Prometheus metrics** at `/metrics` — per-method/status request counters, a request-latency histogram, and process gauges (uptime, active/total connections) in the text exposition format
- **Health endpoints** — `/healthz` (liveness, inline) and `/readyz` (readiness, proves the storage engine can serve a transaction)

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
| `RATE_LIMIT_ENABLED` | `true`           | Enable per-IP token-bucket rate limiting                     |
| `RATE_LIMIT_RPS`     | `100`            | Sustained refill rate (tokens/sec per client IP)             |
| `RATE_LIMIT_BURST`   | `200`            | Bucket size — the largest instantaneous burst allowed        |
| `API_KEYS`           | _(empty)_        | Comma-separated keys guarding writes; empty disables auth    |
| `JWT_SECRET`         | _(random)_       | HMAC secret signing the JWTs; unset = random per run, so tokens die with the process |
| `JWT_TTL_SECONDS`    | `3600`           | Lifetime of issued tokens                                    |
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
| `GET`    | `/metrics`        | Prometheus metrics (see below)                 |
| `GET`    | `/healthz`        | Liveness probe — `{"status":"ok"}`             |
| `GET`    | `/readyz`         | Readiness probe (checks the storage engine)    |
| `GET`    | `/ws/chat`        | WebSocket echo endpoint                        |
| `POST`   | `/api/auth/register` | Create a user, returns a JWT — see below    |
| `POST`   | `/api/auth/login` | Log in, returns a JWT — see below              |
| `GET`    | `/api/auth/me`    | Current user (requires a Bearer token)         |
| —        | `/api/todos[/id]` | Todo CRUD, JWT-protected — see below           |
| `GET`    | `/*`              | Static files from `DOCUMENT_ROOT`              |

### Todo API

A todo is `{"id": <number>, "user_id": <number>, "title": <string>, "completed": <bool>}`. On `POST`/`PUT` the body must supply `title`; `completed` defaults to `false`. Ids are assigned by the server and seeded at startup from the highest id already on disk.

Every `/api/todos` route requires an `Authorization: Bearer <jwt>` header (see [Auth](#auth)), and each user only ever sees their own todos: the list is filtered to the authenticated user, and fetching, replacing, or deleting another user's todo answers `404` — indistinguishable from a todo that doesn't exist, so ids can't be probed.

| Method   | Path              | Description                      | Success |
| -------- | ----------------- | -------------------------------- | ------- |
| `GET`    | `/api/todos`      | List your todos (JSON array, filterable — see below) | `200`   |
| `POST`   | `/api/todos`      | Create a todo owned by you       | `201`   |
| `GET`    | `/api/todos/{id}` | Fetch a single todo              | `200`   |
| `PUT`    | `/api/todos/{id}` | Replace a todo                   | `200`   |
| `DELETE` | `/api/todos/{id}` | Delete a todo                    | `204`   |

Errors: `400` (missing/invalid `title`, or a non-numeric id), `401` (missing, invalid, or expired token), `404` (no such todo — or not yours), `413` (todo too large); over the rate limit, any request is `429`.

**Filtering & pagination.** `GET /api/todos` accepts query parameters, applied during the scan:

| Param       | Example              | Effect                                             |
| ----------- | -------------------- | -------------------------------------------------- |
| `completed` | `?completed=true`    | Keep only todos with that completed state          |
| `q`         | `?q=milk`            | Case-insensitive substring match on the title      |
| `offset`    | `?offset=20`         | Skip the first N matches                            |
| `limit`     | `?limit=10`          | Return at most N matches                            |

The response body stays a bare JSON array (so existing clients and the demo UI are unaffected); the count of matches *before* pagination is returned in an `X-Total-Count` header.

```sh
curl -i -H "Authorization: Bearer $TOKEN" \
  "localhost:8080/api/todos?completed=false&q=buy&limit=10&offset=0"
# 200, X-Total-Count: 7, body is the first 10 matching todos
```

```sh
# grab a token first (see Auth below)
TOKEN=$(curl -s -X POST localhost:8080/api/auth/login \
  -d '{"username":"alice","password":"wonderland123"}' \
  | sed 's/.*"token":"\([^"]*\)".*/\1/')

curl -X POST -H "Authorization: Bearer $TOKEN" localhost:8080/api/todos \
  -d '{"title":"buy milk","completed":false}'
# 201 {"id":1,"user_id":1,"title":"buy milk","completed":false}

curl -H "Authorization: Bearer $TOKEN" localhost:8080/api/todos
curl -H "Authorization: Bearer $TOKEN" localhost:8080/api/todos/1
curl -X PUT -H "Authorization: Bearer $TOKEN" localhost:8080/api/todos/1 \
  -d '{"title":"buy oat milk","completed":true}'
curl -X DELETE -H "Authorization: Bearer $TOKEN" localhost:8080/api/todos/1
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

### Metrics & health

`GET /metrics` renders the Prometheus text exposition format: a request counter labelled by method and status class, a latency histogram (`codo_request_duration_seconds`) over the classic bucket ladder, and process gauges. The `metrics` middleware records every request as the chain unwinds, so short-circuited replies (a `429`, a `401`) are counted too.

```sh
curl localhost:8080/metrics
# codo_requests_total{method="GET",status="2xx"} 41
# codo_requests_total{method="POST",status="4xx"} 2
# codo_request_duration_seconds_bucket{le="0.01"} 38
# codo_request_duration_seconds_bucket{le="+Inf"} 43
# codo_request_duration_seconds_sum 0.184392
# codo_request_duration_seconds_count 43
# codo_uptime_seconds 128
# codo_active_connections 1
```

`GET /healthz` is a cheap liveness check answered inline. `GET /readyz` is a readiness check that begins and commits a storage transaction, so it only returns `200` once the engine can actually serve traffic (`503` otherwise) — point a load balancer's health check at it.

### Rate limiting

Each client IP gets a token bucket, refilled at `RATE_LIMIT_RPS` tokens/second up to `RATE_LIMIT_BURST`; every request spends one token. When the bucket is empty the request is rejected with `429 Too Many Requests` and a `Retry-After` telling the client how many seconds until a token frees up. Buckets live in a fixed-size table, so the limiter adds constant memory and an O(1) check on the request path. Preflight `OPTIONS` is answered by the CORS middleware before the limiter runs, so browsers aren't penalized for it. Set `RATE_LIMIT_ENABLED=false` to turn it off.

```sh
# with RATE_LIMIT_BURST=2, the third rapid request is shed:
curl -i localhost:8080/healthz   # 200
curl -i localhost:8080/healthz   # 200
curl -i localhost:8080/healthz   # 429 Too Many Requests, Retry-After: 1
```

### Auth

**User accounts (JWT).** The Todo API is per-user: register or log in to get an HS256 JWT, then present it as `Authorization: Bearer <token>` on every `/api/todos` request. Usernames are 3–63 characters of `[A-Za-z0-9_.-]`; passwords are at least 8 characters, hashed with PBKDF2-HMAC-SHA256 (100k iterations, 16-byte random salt) — only the salt and hash are stored, in the same B-tree as the todos under `user:<name>` keys. Login runs the same KDF even for unknown usernames and compares hashes in constant time, so neither timing nor the response distinguishes a wrong password from a missing user.

| Method | Path                 | Body                              | Success | Errors |
| ------ | -------------------- | --------------------------------- | ------- | ------ |
| `POST` | `/api/auth/register` | `{"username":..,"password":..}`   | `201` + token | `400` invalid fields, `409` name taken |
| `POST` | `/api/auth/login`    | `{"username":..,"password":..}`   | `200` + token | `400`, `401` bad credentials |
| `GET`  | `/api/auth/me`       | — (Bearer token required)         | `200` `{"id":..,"username":..}` | `401` |

```sh
curl -X POST localhost:8080/api/auth/register -d '{"username":"alice","password":"wonderland123"}'
# 201 {"id":1,"username":"alice","token":"eyJ...","token_type":"Bearer","expires_in":3600}

curl -X POST localhost:8080/api/auth/login -d '{"username":"alice","password":"wonderland123"}'
# 200 {"token":"eyJ...","token_type":"Bearer","expires_in":3600,"user":{"id":1,"username":"alice"}}

curl -H "Authorization: Bearer eyJ..." localhost:8080/api/auth/me
# 200 {"id":1,"username":"alice"}
```

Tokens carry `sub` (username), `uid` (user id), `iat`, and `exp`, and expire after `JWT_TTL_SECONDS` (default one hour). The signing secret comes from `JWT_SECRET`; when unset a random secret is generated at startup (and a warning printed), which means every issued token dies with the process — set it in production, and set the *same* value on every instance behind the balancer. The verifier recomputes the HS256 signature unconditionally and never reads the token's `alg` header, so `alg:none`-style downgrades don't apply.

**API keys.** Set `API_KEYS` to a comma-separated list to lock down mutating requests (`POST`/`PUT`/`DELETE`/`PATCH`) on the remaining routes (e.g. `/api/echo`); reads stay public, and the JWT-owned paths (`/api/todos*`, `/api/auth/*`) are exempt so a request never needs two credentials. The key is presented as `X-API-Key: <key>` or `Authorization: Bearer <key>` and compared in constant time. A protected request with no key is `401`; a wrong key is `403`. With `API_KEYS` empty (the default), API-key auth is disabled.

```sh
# API_KEYS=secret123
curl -X POST localhost:8080/api/echo -d 'hello'
# 401 {"error":"missing API key"}
curl -X POST -H 'X-API-Key: secret123' localhost:8080/api/echo -d 'hello'
# 200 hello
```

### Helper scripts

`scripts/` wraps each operation in a small `curl` script. They target `http://localhost:8080` unless you set `BASE_URL`, and need `bash` + `curl` (on Windows: WSL or Git Bash). The todo scripts require a JWT in the `TOKEN` env var:

```sh
TOKEN=$(./scripts/login.sh alice wonderland123 | sed 's/.*"token":"\([^"]*\)".*/\1/')
TOKEN=$TOKEN ./scripts/list_todos.sh
```

| Script             | Arguments                    | Notes                                          |
| ------------------ | ---------------------------- | ---------------------------------------------- |
| `register.sh`      | `<username> <password>`      | Creates a user, prints the JSON incl. token    |
| `login.sh`         | `<username> <password>`      | Prints the JSON incl. token                    |
| `create_todo.sh`   | `"<title>" [completed]`      | Prints the created todo                        |
| `list_todos.sh`    | _(none)_                     | Prints the JSON array of your todos            |
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
  config, env, util, stats, metrics (Prometheus request counters + histogram),
  mime, connection (socket helpers + teardown), http_protocol (request parser +
  response writer), compression (gzip), thread_pool (priority task queues; work
  stealing exists but is off), lru (the read cache), lockfree (queue/hashtable/
  skiplist -- see note below)
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
  route, middleware, rate_limit (token-bucket limiter), auth (API-key guard),
  handlers, todo_handlers, ssl_util, websocket
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
