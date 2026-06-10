# Codo

Codo is a from-scratch multi-threaded HTTP/1.1 server written in C11, built directly on top of POSIX sockets and `epoll`. It uses an acceptor + worker-pool model: the main thread accepts connections and round-robins them to a fixed pool of worker threads, each running its own private `epoll` loop over a linked list of connections.

## Features

- HTTP/1.1 with keep-alive
- Edge-triggered `epoll` per worker
- Route table with per-method handlers + default static-file handler
- Optional TLS via OpenSSL (auto-enabled when `server.crt` / `server.key` exist)
- WebSocket upgrade handshake (`Sec-WebSocket-Accept` via SHA1 + base64)
- gzip plumbing via zlib (scaffolded)
- Example JSON endpoints: `/api/hello`, `/api/echo`, `/api/status`, `/ws/chat`

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

## Layout

```
include/    public headers (one per subsystem)
src/        implementation (main, server, worker, connection, http_protocol,
            route, ssl, websocket, compression, mime, util, handlers)
build/      .o + .d files (auto-generated header deps via -MMD -MP)
bin/        output binary
tests/      reserved (no test harness yet)
```

## Status

Work in progress. Some subsystems (gzip compression of response bodies, full WebSocket framing, file streaming with `sendfile`) are scaffolded but not fully wired into the response path.
