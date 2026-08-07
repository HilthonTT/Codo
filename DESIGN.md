# Codo — Design

How Codo is put together and why. The [README](README.md) covers what it does
and how to run it; this document covers the mechanisms behind it.

- [Threading model](#threading-model)
- [The request path](#the-request-path)
- [Middleware chain](#middleware-chain)
- [Storage engine](#storage-engine)
- [The owner index](#the-owner-index)
- [The todo read cache](#the-todo-read-cache)
- [Auth](#auth)
- [Load balancer](#load-balancer)
- [Module seams](#module-seams)
- [Known gaps](#known-gaps)

---

## Threading model

Work is split across three thread groups so that a slow disk never stalls the
network.

| Group | Threads | Owns |
| ----- | ------- | ---- |
| Accept loop | 1 (main) | The listening socket |
| Workers | 8 (`WORKER_THREADS`) | One private edge-triggered `epoll` set each, plus the connections assigned to it |
| Storage pool | 16 (`STORAGE_POOL_THREADS`) | The blocking handlers |

The accept loop takes connections and round-robins them across workers. Each
worker then owns its connections outright: no connection is ever touched by two
workers, so the per-connection state machine needs no locking.

The storage pool is deliberately **oversubscribed** relative to the core count.
Its threads spend nearly all their time parked in `fsync`/`pread`, so extra
threads keep the CPU busy instead of idle.

The pool has **two priority levels** (`STORAGE_POOL_PRIORITIES`): reads are
queued above writes (`STORAGE_PRIORITY_READ` > `STORAGE_PRIORITY_WRITE`), so a
`GET` does not wait behind a burst of WAL-fsyncing writes.

> Work stealing is implemented in `common/src/thread_pool.c` but is switched off
> at the only call site. Idle workers park on the *global* queue's condition
> variable, so a task pushed only to a per-worker local queue would never wake a
> sleeping worker. Until the worker loop blocks on a shared "work available"
> condition, every submission is routed through the global queue.

## The request path

```
accept  ->  worker epoll  ->  parse  ->  route match  ->  middleware chain  ->  handler
                                              |
                                              +-- offloaded route? --> storage pool
```

1. **Accept.** The main thread accepts and hands the fd to a worker.
2. **Read + parse.** The worker's edge-triggered loop reads until it has a
   complete request, then `parse_http_request()` fills an `http_request_t`.
3. **Route match.** `find_route()` resolves the path against the route table,
   which supports per-method handlers and a trailing-`*` wildcard
   (`/api/todos/*`). A miss falls through to the static-file handler.
4. **Run.** The handler runs inside the middleware chain — inline on the worker,
   or on the storage pool if the route was registered as offloaded.

### The offload handoff

This is the part that keeps the worker loop responsive. A handler that touches
the storage engine (every Todo route) or the disk (the static-file handler) is
registered with `add_route_offloaded()` instead of `add_route()`.

When one of those matches:

1. The worker **drops the connection out of its `epoll` interest set** and flags
   it `offloaded`, so no further events fire for it.
2. It submits the handler to the storage pool at the route's priority.
3. A pool thread runs the handler to completion — blocking on `fsync`/`pread` is
   fine here, it is what these threads are for.
4. When the response is staged, the pool thread **re-arms `EPOLLOUT`** on the
   worker's epoll set.

The worker's loop is never blocked on disk I/O. The connection is invisible to
epoll for the duration, which is what makes it safe for a different thread to be
writing into its buffers.

If the pool could not be created at startup, `add_route_offloaded()` routes
degrade to running inline — correct, just less responsive under storage load.

### Static files

Static responses stream through `sendfile(2)` on plaintext connections — zero
copy, no in-memory size cap. Each response carries a strong `ETag` (mtime + size)
and `Last-Modified`, and answers `304` to a matching `If-None-Match` /
`If-Modified-Since`. A gzipped response carries a weak `W/` etag, since the
encoded bytes differ from the file's.

A single `Range: bytes=` header (including the `N-` and `-N` suffix forms)
produces `206 Partial Content` with `Content-Range`, validated against
`If-Range`, and `416` when unsatisfiable — served through the same `sendfile`
window.

### Request body edge cases

- `Transfer-Encoding: chunked` is decoded, tolerating extensions and trailers.
- A message carrying **both** `Content-Length` and `Transfer-Encoding` is
  refused with `400` — a request-smuggling guard. Any non-chunked transfer
  coding gets `501`.
- `Expect: 100-continue` gets its interim `100 Continue` written as soon as the
  headers arrive, so a client that waits before sending a large body is not
  stuck until its retry timeout.

## Middleware chain

Installed once by `register_default_middleware()`, wrapping **every** route
including the default file handler:

```
logging -> metrics -> cors -> rate_limit -> auth -> jwt -> handler
```

The order is deliberate, outermost first:

| Stage | Why it sits here |
| ----- | ---------------- |
| `logging` | Outermost, so it times the whole chain |
| `metrics` | Records the final status and latency of *every* request, including ones a later stage short-circuits |
| `cors` | Answers `OPTIONS` preflight before rate limiting or auth can reject it, so browsers are not penalized for preflight |
| `rate_limit` | Sheds excess load before the server spends effort on auth |
| `auth` | API-key guard on mutating routes outside the JWT paths |
| `jwt` | Innermost: verifies bearer tokens and hands the verified identity to the handler |

CORS is applied via a single `cors_origin` field on the response rather than by
each handler adding a header, so API replies, static files and error responses
all carry it.

### Rate limiting

A token bucket per client IP, refilled at `RATE_LIMIT_RPS`/second up to
`RATE_LIMIT_BURST`; each request spends one token. An empty bucket yields `429`
with a `Retry-After` giving the seconds until a token frees.

Buckets live in a **fixed open-addressing table with bounded, LRU-evicting
probes**. That is what keeps the limiter O(1) with a constant memory footprint
regardless of how many distinct IPs are seen — the alternative, a growing map
keyed by IP, is itself a memory-exhaustion vector.

## Storage engine

An embedded B-tree engine (`libstorage.a`), a process-wide singleton. Public API
in `storage/include/storage.h`; internals are private to
`storage/src/storage_internal.h`.

| Layer | File | Responsibility |
| ----- | ---- | -------------- |
| Engine | `engine.c` | Lifecycle, checkpointing, counters |
| WAL | `wal.c` | Write-ahead log append + flush |
| Pager | `pager.c` | Buffer pool, page I/O, checksums |
| B-tree | `btree.c` | Page-level key search/insert/delete |
| Transactions | `txn.c` | Begin/commit/abort |
| CRUD | `db.c` | Tree descent over the layers above; full and prefix scans |

All reads and writes go through a transaction handle from
`begin_transaction()`. Every length read off disk is treated as untrusted:
traversals validate that a pair stays inside its page before dereferencing it.

A final checkpoint runs on `SIGINT`/`SIGTERM`, so committed data survives a
restart.

### Prefix scans

`db_scan_prefix()` is what makes the owner index worth having. Keys are sorted,
so it descends straight to the first matching key and stops at the first key
that no longer matches — the cost tracks the range, not the tree.

The callback runs while the page holding the current pair is **read-locked**, so
it must not call back into the engine. Collect what you need and do the lookups
after the scan returns.

## The owner index

A collection `GET` used to scan the whole B-tree and discard every row that was
not yours — work proportional to *everybody's* todos.

Each todo now also gets an index entry keyed:

```
idx:user:<uid>:<id>      # both ids zero-padded to a fixed width
```

The padding is the trick: it makes the tree's lexicographic order match numeric
order, so one user's entries form a single contiguous range. Listing becomes a
`db_scan_prefix` over that range plus a lookup per row, and the cost tracks the
caller's own todos.

When no filter needs to look inside a row (no `completed`, no `q`), only the
requested page is read at all: `?limit=10` reads ten rows however many the user
has, and `X-Total-Count` comes from the entry count without touching a row.

**The row stays the authority.** Entries are written in the same transaction as
the row they describe, and a listing re-checks each row's `user_id` — so a stale
entry can never leak another user's todo.

**The index is derived state**, so `todo_index_reconcile()` rebuilds it from the
rows at startup, in the same pass that seeds the id counter: missing entries are
added, entries whose row is gone or whose owner changed are dropped, and the
repair is logged.

If that repair cannot finish — most plausibly because the page filled up, since
the index roughly doubles what a todo costs and page splits are not implemented
— the index is marked unavailable and listings **fall back to the full scan**
rather than answering from a partial index that would silently hide todos.

Because the engine's abort path has no undo, writers compensate by hand: a todo
whose index entry fails to write deletes the row again rather than committing a
todo no listing would show.

## The todo read cache

A single-todo `GET` would otherwise cost a transaction plus a B-tree descent
through the buffer pool, so it reads through a fixed-capacity LRU cache
(`TODO_CACHE_CAPACITY`). Writes keep it in step: create and update store the new
JSON, delete drops the entry.

A cache fill that races a concurrent write is **discarded rather than
resurrecting a stale value**: the cache carries a generation counter that every
write bumps, and a fill whose snapshot is stale is dropped.

The collection `GET` is deliberately **not** cached. It builds a result set that
any write would invalidate, so caching it would trade the read for a
near-permanent miss. Its rows are read through the transaction rather than the
LRU cache, so one listing is a consistent snapshot.

## Auth

### User accounts and JWT

Passwords are hashed with **PBKDF2-HMAC-SHA256**, 100k iterations, 16-byte
random per-user salt. Only the salt and hash are stored, in the same B-tree as
the todos under `user:<name>` keys.

Login runs the same KDF **even for unknown usernames** and compares hashes in
constant time, so neither timing nor the response distinguishes a wrong password
from a missing user.

Derived key material lives in an `mlock`'d arena (`common/src/crypto.c`) rather
than on the heap, so it is never written to swap; releases zero the blocks
first.

Tokens are HS256 and carry `sub` (username), `uid`, `iat` and `exp`. The
verifier **recomputes the signature unconditionally and never reads the token's
`alg` header**, so `alg:none`-style downgrades do not apply.

`JWT_SECRET` signs them. Unset, a random secret is generated at startup (with a
warning) — every issued token then dies with the process, and instances behind
the balancer will not accept each other's tokens.

### Per-user isolation

Every `/api/todos*` request needs a bearer token, and each user only sees their
own todos. Fetching, replacing or deleting another user's todo answers `404` —
indistinguishable from a todo that does not exist, so ids cannot be probed.

### API keys

`API_KEYS` guards mutating verbs (`POST`/`PUT`/`DELETE`/`PATCH`) on the
remaining routes. Keys are presented as `X-API-Key` or `Authorization: Bearer`
and compared in constant time. The JWT-owned paths are exempt, so a request
never needs two credentials. Reads stay public; empty `API_KEYS` disables it.

## Load balancer

`bin/codo-balancer` is a single-threaded `epoll` TCP proxy. It accepts a client,
picks a backend, opens a non-blocking connection, and shuttles bytes both ways —
pausing reads on one side when the other side is not writable yet.

Each fd registers an `io_ctx_t` as its epoll `data.ptr`, so the loop knows which
connection and which side (`IO_CLIENT` / `IO_BACKEND` / `IO_LISTEN`) an event
belongs to without a lookup.

Four selection strategies: smooth weighted round-robin (nginx-style, the
default), least-connections, IP hash, and random.

Health checking is **passive**. A backend is marked unhealthy after 3
consecutive failures (connect refused, read/write errors, `EPOLLERR`/`HUP`) and
re-admitted 30s later to prove itself. Unhealthy backends are skipped during
selection.

## Module seams

Five components; `common/`, `storage/` and `api/` are libraries, `server/` and
`balancer/` are the binaries.

```
common/  ->  libcommon.a      shared infrastructure
storage/ ->  libstorage.a     B-tree engine
api/     ->  libapi.a         the web API
server/  ->  bin/codo         links all three
balancer/->  bin/codo-balancer  links libcommon.a only
```

### api / server

`server/` is the HTTP engine and knows nothing about what is served: it accepts
connections, parses requests, runs middleware, matches routes and serves files.
`api/` is everything Codo actually exposes, behind three calls:

```c
api_init(db_file, wal_file);  // open the engine, seed ids, build the cache
api_mount(&g_server);         // register every route (offloaded where blocking)
api_shutdown();               // drop the cache, checkpoint, close the engine
```

The dependency runs one way: `api/` includes the core's headers to register
routes on `http_server_t`; the core never includes `api/`. The single exception
is `server/src/main.c`, which is pure wiring. **Adding an endpoint means
touching `api/`, not `server/`.**

The policy middleware (`rate_limit`, `auth`, `jwt`) and user accounts stay on
the core side: they are transport-level concerns guarding *every* route. The
auth *routes* are registered by `api_mount()` alongside the other endpoints, so
the wiring still enters through the same seam.

### What is shared vs. server-only

`common/` holds everything that does not depend on `http_server_t`. Two seams
keep it free of server types:

- `http_protocol.c` fills the `Server:` header via `http_server_name()`, a hook
  each binary defines for itself.
- `connection.c` provides `cleanup_connection()` and socket helpers (no server
  state); `allocate_connection()` / `free_connection()`, which touch the pool,
  live in the server's `connection_pool.c`.

The balancer is split the same way: `balancer/src/main.c` only loads config and
calls into the library; backend-spec parsing lives in `balancer_config.c`.

### Feature-test macros

`-D_GNU_SOURCE` is set once in the Makefile rather than at the top of every
translation unit. `-std=c11` defines `__STRICT_ANSI__`, which hides the
Linux-only interfaces the hot paths are built on (`epoll`, `sendfile`,
`accept4`, `MAP_ANONYMOUS`, `getrandom`) — and one file forgetting the define is
a build break that only shows up on some glibc versions.

## Known gaps

These are real, and deliberately documented rather than hidden.

- **Linux only.** `epoll` and `sendfile` are used directly; there is no
  portability layer.
- **No test suite.** Correctness is covered only by the CI smoke tests and the
  sanitizer build.
- **No B-tree page split.** A leaf's capacity is one page, so the whole database
  is bounded by a single page's worth of rows, and a full page fails the insert.
  The owner index roughly doubles what one todo costs — which is why a reconcile
  that cannot finish degrades to a full scan.
- **No WAL crash recovery.** The WAL is written and flushed, but not replayed at
  startup: a hard kill between a commit and a checkpoint can lose committed
  writes.
- **The abort path has no undo** (`storage/src/txn.c`), so a multi-write
  transaction cannot be rolled back by the engine. Writers that need it
  compensate by hand — see [the owner index](#the-owner-index).
- **`db_update()` only handles same-length values**, so a todo update is a
  delete + insert inside one transaction.
- **Work stealing is off.** See [Threading model](#threading-model).
- The JSON parser is hand-rolled and object-shaped: it reads top-level string
  and boolean fields, and collapses `\uXXXX` escapes to `?`.
- **Engine and pool counters have no HTTP surface.** `print_storage_statistics()`
  and `thread_pool_statistics()` are the only readers of the storage and
  thread-pool instrumentation, and both dump to stdout; neither is called on any
  request path. Exposing them through `/metrics` is the natural next step.
- **The balancer records per-backend counters it never reads.**
  `total_requests`, `failed_requests` and the response-time EWMA are maintained
  but not exposed anywhere.
