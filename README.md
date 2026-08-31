# Codo

A from-scratch HTTP/1.1 server in C11 on POSIX sockets and `epoll` — no web
framework, no external HTTP library. It ships a persistent Todo REST API backed
by its own B-tree storage engine (buffer pool, write-ahead log, transactions)
and a companion TCP load balancer.

Everything except TLS (OpenSSL) and gzip (zlib) is in-tree: the HTTP parser, the
router, the middleware chain, the thread pool, the LRU cache, the WebSocket
framing, and the storage engine.

```sh
make && ./bin/codo
# HTTP server starting on port 8080
```

**Linux only** — the hot paths use `epoll(7)` and `sendfile(2)`. On Windows, use
WSL2.

> **[DESIGN.md](DESIGN.md)** explains how it works: the threading model, the
> offload handoff, the storage engine, and the known gaps.

## Features

**HTTP** — keep-alive with a 30s idle timeout · edge-triggered `epoll`, one loop
per worker · conditional requests (`ETag`, `Last-Modified`, `304`) · byte ranges
(`206`/`416`) over zero-copy `sendfile` · chunked request bodies, with a
request-smuggling guard · `Expect: 100-continue` · route table with per-method
handlers and trailing-`*` wildcards · optional TLS, auto-enabled when the cert
and key exist · WebSocket upgrade with full RFC 6455 framing.

**Middleware & policy** — one chain wrapping every route: request logging,
Prometheus metrics, CORS with preflight, per-IP token-bucket rate limiting,
API-key auth, JWT auth. Health endpoints at `/healthz` and `/readyz`.

**Performance** — blocking handlers offloaded to a priority thread pool (reads
queued above writes) · zero-copy static files · gzip negotiated on
`Accept-Encoding` · read-through LRU cache in front of the storage engine ·
lock-free atomic counters on the accept/read/write/close paths.

**Storage** — embedded B-tree with pages, buffer pool, WAL, transactions and
checkpointing · prefix range scans · a secondary index on a todo's owner, so
listing a user's todos reads only their rows · final checkpoint on
`SIGINT`/`SIGTERM`.

## Build

Shared code compiles into `libcommon.a`, the engine into `libstorage.a`, the web
API into `libapi.a`; each binary links what it needs.

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

Requires `gcc`, OpenSSL and zlib headers (`libssl-dev`, `zlib1g-dev` on
Debian/Ubuntu). Links against `-lssl -lcrypto -lz -lpthread`.

## Run

```sh
./bin/codo [port] [document_root]
```

Config is read from `.env` (override the path with `ENV_FILE`), then the real
environment, then CLI args. TLS turns on when `SSL_ENABLED` is set and both the
cert and key files exist.

### Configuration

| Key                   | Default          | Purpose                                             |
| --------------------- | ---------------- | --------------------------------------------------- |
| `PORT`                | `8080`           | Listen port                                          |
| `DOCUMENT_ROOT`       | `/var/www/html`  | Static file root (the bundled `.env` points at `www`) |
| `SSL_ENABLED`         | `true`           | Enable TLS when cert and key are present             |
| `SSL_CERT_FILE`       | `server.crt`     | TLS certificate                                      |
| `SSL_KEY_FILE`        | `server.key`     | TLS private key                                      |
| `CORS_ALLOW_ORIGIN`   | `*`              | Value echoed in `Access-Control-Allow-Origin`        |
| `RATE_LIMIT_ENABLED`  | `true`           | Per-IP token-bucket rate limiting                    |
| `RATE_LIMIT_RPS`      | `100`            | Sustained refill rate (tokens/sec per IP)            |
| `RATE_LIMIT_BURST`    | `200`            | Bucket size — largest instantaneous burst            |
| `API_KEYS`            | _(empty)_        | Comma-separated keys guarding writes; empty disables |
| `JWT_SECRET`          | _(random)_       | HMAC secret; unset = random per run, so tokens die with the process |
| `JWT_TTL_SECONDS`     | `3600`           | Lifetime of issued tokens                            |
| `DB_FILE`             | `codo.db`        | B-tree data file                                     |
| `WAL_FILE`            | `codo.wal`       | Write-ahead log                                      |
| `TODO_CACHE_CAPACITY` | `1024`           | Max entries in the todo read cache                   |
| `TRUST_PROXY_PROTOCOL`| `false`          | Accept a PROXY protocol v1 header and take the client address from it. Enable only when the listener is reachable solely by a trusted balancer — the header lets its sender claim any address. When on, the header is **mandatory**: a connection without one is dropped |
| `BALANCER_PORT`       | `8000`           | Listen port for `codo-balancer`                      |
| `BALANCER_BACKENDS`   | `127.0.0.1:8080` | Backends: `host:port[:weight]`, comma-separated. IPv4/IPv6 literals and DNS names |
| `PROXY_PROTOCOL`      | `false`          | Send a PROXY protocol v1 header to each backend, declaring the real client. Pair with `TRUST_PROXY_PROTOCOL` on the backend |

Compile-time limits (thread counts, buffer sizes, connection caps) live in
`common/include/config.h`.

## Endpoints

| Method   | Path                 | Description                                 |
| -------- | -------------------- | ------------------------------------------- |
| `GET`    | `/api/hello`         | Hello-world JSON                            |
| `POST`   | `/api/echo`          | Echoes the request body back                |
| `GET`    | `/api/status`        | Server status                               |
| `GET`    | `/api/stats`         | Network counters                            |
| `GET`    | `/api/cache`         | Todo cache hit/miss counters                |
| `GET`    | `/metrics`           | Prometheus text exposition format           |
| `GET`    | `/healthz`           | Liveness — `{"status":"ok"}`                |
| `GET`    | `/readyz`            | Readiness — proves the engine serves a txn  |
| `GET`    | `/ws/chat`           | WebSocket echo endpoint                     |
| `POST`   | `/api/auth/register` | Create a user, returns a JWT                |
| `POST`   | `/api/auth/login`    | Log in, returns a JWT                       |
| `GET`    | `/api/auth/me`       | Current user (Bearer token required)        |
| —        | `/api/todos[/id]`    | Todo CRUD, JWT-protected — see below        |
| `GET`    | `/*`                 | Static files from `DOCUMENT_ROOT`           |

### Auth

Register or log in to get an HS256 JWT, then present it as
`Authorization: Bearer <token>`. Usernames are 3–63 chars of `[A-Za-z0-9_.-]`;
passwords at least 8 characters, hashed with PBKDF2-HMAC-SHA256.

```sh
curl -X POST localhost:8080/api/auth/register -d '{"username":"alice","password":"wonderland123"}'
# 201 {"id":1,"username":"alice","token":"eyJ...","token_type":"Bearer","expires_in":3600}

TOKEN=$(curl -s -X POST localhost:8080/api/auth/login \
  -d '{"username":"alice","password":"wonderland123"}' \
  | sed 's/.*"token":"\([^"]*\)".*/\1/')
```

Set `JWT_SECRET` in production, and the *same* value on every instance behind
the balancer. Set `API_KEYS` to additionally guard mutating verbs on the
non-JWT routes (`X-API-Key: <key>`); `401` with no key, `403` with a wrong one.

### Todo API

A todo is `{"id", "user_id", "title", "completed"}`. `POST`/`PUT` must supply
`title`; `completed` defaults to `false`. Ids are server-assigned. Every route
needs a bearer token, and each user only ever sees their own todos — another
user's todo answers `404`, indistinguishable from one that does not exist.

| Method   | Path              | Description                       | Success |
| -------- | ----------------- | --------------------------------- | ------- |
| `GET`    | `/api/todos`      | List your todos (filterable)      | `200`   |
| `POST`   | `/api/todos`      | Create a todo owned by you        | `201`   |
| `GET`    | `/api/todos/{id}` | Fetch a single todo               | `200`   |
| `PUT`    | `/api/todos/{id}` | Replace a todo                    | `200`   |
| `DELETE` | `/api/todos/{id}` | Delete a todo                     | `204`   |

Errors: `400` (bad `title` or non-numeric id), `401` (missing/invalid/expired
token), `404` (no such todo, or not yours), `413` (too large), `429` (over the
rate limit).

```sh
curl -X POST -H "Authorization: Bearer $TOKEN" localhost:8080/api/todos \
  -d '{"title":"buy milk","completed":false}'
# 201 {"id":1,"user_id":1,"title":"buy milk","completed":false}

curl -H "Authorization: Bearer $TOKEN" localhost:8080/api/todos
curl -X PUT -H "Authorization: Bearer $TOKEN" localhost:8080/api/todos/1 \
  -d '{"title":"buy oat milk","completed":true}'
curl -X DELETE -H "Authorization: Bearer $TOKEN" localhost:8080/api/todos/1
```

**Filtering & pagination.** `GET /api/todos` accepts `completed=true|false`,
`q=<substring>` (case-insensitive, on the title), `offset` and `limit`. The body
stays a bare JSON array; the match count *before* pagination comes back in
`X-Total-Count`. Todos are returned in ascending id order, which is what makes
`offset`/`limit` a stable window.

```sh
curl -i -H "Authorization: Bearer $TOKEN" \
  "localhost:8080/api/todos?completed=false&q=buy&limit=10&offset=0"
# 200, X-Total-Count: 7, body is the first 10 matching todos
```

Listing a user's todos reads only their rows, via a secondary index on the
owner — see [the owner index](DESIGN.md#the-owner-index).

### Helper scripts

`scripts/` wraps each operation in a small `curl` script. They target
`http://localhost:8080` unless `BASE_URL` is set, and need `bash` + `curl`. The
todo scripts require a JWT in `TOKEN`.

```sh
TOKEN=$(./scripts/login.sh alice wonderland123 | sed 's/.*"token":"\([^"]*\)".*/\1/')
TOKEN=$TOKEN ./scripts/list_todos.sh
```

| Script           | Arguments                    |
| ---------------- | ---------------------------- |
| `register.sh`    | `<username> <password>`      |
| `login.sh`       | `<username> <password>`      |
| `create_todo.sh` | `"<title>" [completed]`      |
| `list_todos.sh`  | _(none)_                     |
| `get_todo.sh`    | `<id>`                       |
| `update_todo.sh` | `<id> "<title>" [completed]` |
| `delete_todo.sh` | `<id>`                       |

`create_todo.sh` and `update_todo.sh` JSON-escape the title, so titles with
spaces or quotes are safe to pass as one argument.

## Demo site

`www/` is a self-hosted demo served out of the document root: `index.html`
exercises the Todo API from the browser, `docs.html` renders the API reference,
`openapi.json` is the OpenAPI description. The bundled `.env` already sets
`DOCUMENT_ROOT=www`, so `make run` then <http://localhost:8080> gives a working
UI.

## Load balancer

A single-threaded `epoll` TCP proxy fronting one or more `codo` instances, with
four selection strategies (smooth weighted round-robin by default,
least-connections, IP hash, random) and passive health checking.

```sh
make balancer
./bin/codo-balancer 8000 127.0.0.1:8080,127.0.0.1:8081:2
# or from the environment:
BALANCER_PORT=8000 BALANCER_BACKENDS=127.0.0.1:8080,127.0.0.1:8081:2 ./bin/codo-balancer
```

It reads the same `.env`; CLI args override it (`argv[1]` port, `argv[2]` backend
list). Each backend is `host:port` with an optional `:weight` (default `1`).

## Layout

```
common/     shared infrastructure -> libcommon.a
            config, env, util, stats, metrics, mime, connection, http_protocol,
            compression (gzip), thread_pool, lru
storage/    embedded B-tree engine -> libstorage.a
            engine, wal, pager, btree, txn, db
api/        the web API on top of the core -> libapi.a
            handlers, todo_handlers, todo_index
server/     the HTTP/1.1 server core -> bin/codo
            server, worker, connection_pool, route, middleware, rate_limit,
            auth, jwt, user_auth, ssl_util, websocket
balancer/   epoll TCP load balancer -> bin/codo-balancer
            balancer, selection, hash
www/        bundled demo site + OpenAPI description
scripts/    bash + curl helpers for the Todo API
```

The module seams — and why `api/` never leaks into `server/` — are described in
[DESIGN.md](DESIGN.md#module-seams).

## CI

GitHub Actions runs on every push and PR to `main`:

- **Build & smoke test** — boots the server on a throwaway port and asserts that
  static files serve, the API answers, unknown paths `404`, and the logging
  middleware recorded both.
- **Owner index smoke test** — exercises the index end to end: per-user
  isolation, paging, deletion, and a restart with the index intact.
- **Sanitizer build** — the whole tree with ASan + UBSan.
- **CodeQL** — security-and-quality analysis for C.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Vulnerabilities go through
[SECURITY.md](SECURITY.md), not a public issue.

## License

[MIT](LICENSE).
