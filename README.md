<p align="center">
  <img src="assets/logo.svg" width="112" alt="Codo logo">
</p>

<h1 align="center">Codo</h1>

<p align="center">
  A from-scratch HTTP/1.1 server in C11 — <code>epoll</code>, its own B-tree storage engine, a Todo REST API and a TCP load balancer.<br>
  <a href="DESIGN.md">Design</a> · <a href="#endpoints">API</a> · <a href="CONTRIBUTING.md">Contributing</a> · <a href="SECURITY.md">Security</a>
</p>

<p align="center">
  <img alt="C11" src="https://img.shields.io/badge/C-11-4aa8ff">
  <img alt="Linux" src="https://img.shields.io/badge/platform-Linux%20%7C%20WSL2-17212b">
  <img alt="License: MIT" src="https://img.shields.io/badge/license-MIT-2ec26a">
</p>

No web framework, no HTTP library. Everything except TLS (OpenSSL) and gzip
(zlib) is in-tree: HTTP parser, router, middleware, thread pool, LRU cache,
WebSocket framing and a storage engine with buffer pool, WAL and transactions.

```sh
make && ./bin/codo          # → http://localhost:8080 serves the demo UI in www/
```

**Linux only** (`epoll(7)`, `sendfile(2)`) — use WSL2 on Windows. Needs `gcc`,
`libssl-dev` and `zlib1g-dev`.

## Features

- **HTTP** — keep-alive (30s idle), edge-triggered `epoll` per worker, `ETag`/`304`,
  byte ranges over zero-copy `sendfile`, chunked bodies with a smuggling guard,
  `100-continue`, RFC 6455 WebSockets.
- **Middleware** — logging, Prometheus metrics, CORS, per-IP token-bucket rate
  limiting, API-key and JWT auth, `/healthz` + `/readyz`.
- **Performance** — blocking handlers offloaded to a priority thread pool, gzip
  on `Accept-Encoding`, read-through LRU cache, lock-free counters.
- **Storage** — embedded B-tree with WAL, transactions, checkpointing, prefix
  scans and a secondary owner index.
- **Balancer** — `epoll` TCP proxy: weighted round-robin, least-connections,
  IP hash or random; passive health checks; PROXY protocol v1.

## Build & run

```sh
make                  # bin/codo + bin/codo-balancer
make debug            # -O0 with ASan + UBSan
make release          # -O2 -DNDEBUG
./bin/codo [port] [document_root]
./bin/codo-balancer 8000 127.0.0.1:8080,127.0.0.1:8081:2   # host:port[:weight]
```

Config comes from `.env` (path override: `ENV_FILE`), then the environment,
then CLI args. Compile-time limits live in `common/include/config.h`.

<details>
<summary><b>Configuration keys</b></summary>

| Key | Default | Purpose |
| --- | --- | --- |
| `PORT` | `8080` | Listen port |
| `DOCUMENT_ROOT` | `/var/www/html` | Static file root (bundled `.env`: `www`) |
| `SSL_ENABLED` | `true` | Loads `SSL_CERT_FILE` / `SSL_KEY_FILE` (`server.crt` / `server.key`). TLS handshakes are not wired up yet — terminate TLS in front |
| `CORS_ALLOW_ORIGIN` | `*` | `Access-Control-Allow-Origin` value |
| `RATE_LIMIT_ENABLED` | `true` | Per-IP token bucket |
| `RATE_LIMIT_RPS` / `RATE_LIMIT_BURST` | `100` / `200` | Refill rate and bucket size |
| `API_KEYS` | _(empty)_ | Comma-separated keys guarding writes on non-JWT routes |
| `JWT_SECRET` | _(random)_ | HMAC secret — set it, and the same on every instance |
| `JWT_TTL_SECONDS` | `3600` | Token lifetime |
| `DB_FILE` / `WAL_FILE` | `codo.db` / `codo.wal` | Storage files |
| `TODO_CACHE_CAPACITY` | `1024` | Todo read-cache entries |
| `TRUST_PROXY_PROTOCOL` | `false` | Require a PROXY v1 header and take the client address from it. Only behind a trusted balancer |
| `BALANCER_PORT` | `8000` | Balancer listen port |
| `BALANCER_BACKENDS` | `127.0.0.1:8080` | `host:port[:weight]`, comma-separated; IPv4, `[IPv6]` or DNS |
| `PROXY_PROTOCOL` | `false` | Balancer sends PROXY v1 to backends |

</details>

## Endpoints

| Method | Path | Description |
| --- | --- | --- |
| `POST` | `/api/auth/register` · `/api/auth/login` | Get an HS256 JWT |
| `GET` | `/api/auth/me` | Current user |
| `GET` `POST` | `/api/todos` | List (`completed`, `q`, `offset`, `limit`; total in `X-Total-Count`) / create |
| `GET` `PUT` `DELETE` | `/api/todos/{id}` | Read / replace / delete |
| `GET` | `/api/hello` `/api/status` `/api/stats` `/api/cache` | Diagnostics |
| `POST` | `/api/echo` | Echo body |
| `GET` | `/metrics` `/healthz` `/readyz` | Prometheus, liveness, readiness |
| `GET` | `/ws/chat` | WebSocket echo |
| `GET` | `/*` | Static files from `DOCUMENT_ROOT` |

Todos are `{"id","user_id","title","completed"}`, require
`Authorization: Bearer <token>`, and are per-user — another user's todo is a
`404`. Usernames: 3–63 chars of `[A-Za-z0-9_.-]`; passwords ≥ 8 chars
(PBKDF2-HMAC-SHA256).

```sh
curl -X POST localhost:8080/api/auth/register -d '{"username":"alice","password":"wonderland123"}'
TOKEN=$(./scripts/login.sh alice wonderland123 | sed 's/.*"token":"\([^"]*\)".*/\1/')
curl -X POST -H "Authorization: Bearer $TOKEN" localhost:8080/api/todos -d '{"title":"buy milk"}'
curl -H "Authorization: Bearer $TOKEN" "localhost:8080/api/todos?completed=false&q=milk&limit=10"
```

`scripts/` has a `curl` wrapper per operation (`BASE_URL`, `TOKEN` env vars).
The demo UI, API docs and OpenAPI spec live in `www/`.

## Layout

```
common/    shared infra: HTTP protocol, connections, thread pool, LRU, gzip  → libcommon.a
storage/   B-tree engine: pager, WAL, txn                                    → libstorage.a
api/       handlers, todo API, owner index                                   → libapi.a
server/    HTTP core: workers, routing, middleware, auth, JWT, TLS, WS       → bin/codo
balancer/  epoll TCP load balancer                                           → bin/codo-balancer
```

CI builds and smoke-tests the server and owner index, runs an ASan/UBSan build,
and runs CodeQL. Licensed under [MIT](LICENSE).
