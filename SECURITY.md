# Security Policy

## Supported versions

Codo is a personal project under active development and has no released
versions. Only the tip of `main` is supported; fixes land there.

## Reporting a vulnerability

**Please do not open a public issue.** Report privately through GitHub's
[private vulnerability reporting](https://github.com/HilthonTT/Codo/security/advisories/new)
on this repository.

Please include:

- what the flaw is, and which component (server, balancer, storage engine, auth)
- a request or input that triggers it — a `curl` line is ideal
- what an attacker gets out of it
- the commit you tested against

You should get an acknowledgement within a week. Since this is a spare-time
project, a fix may take longer; you will be kept in the loop either way, and
credited in the fix unless you would rather not be.

## Scope

In scope: anything reachable over the network by an unauthenticated or
authenticated client — request parsing, the router, the middleware chain, TLS
setup, WebSocket framing, JWT verification, password handling, and the storage
engine's handling of untrusted on-disk data.

Out of scope, because they are known and documented in
[DESIGN.md](DESIGN.md#known-gaps):

- **No WAL crash recovery.** A hard kill between a commit and a checkpoint can
  lose committed writes; the WAL is not replayed at startup.
- **No transaction undo on abort.** Multi-write transactions cannot be rolled
  back by the engine.
- **No B-tree page split.** The database is bounded by one page; inserts fail
  once it fills. This is a capacity limit, not a memory-safety issue.
- **`JWT_SECRET` unset** generates a random per-process secret (and warns).
  Running multiple instances behind the balancer without setting it means
  tokens do not validate across instances — configuration, not a flaw.
- Resource exhaustion from a client that is *within* the configured rate limit
  and connection cap.

## Hardening notes for operators

- Set `JWT_SECRET` to a high-entropy value, identical on every instance.
- Set `API_KEYS` if any non-JWT mutating route is exposed.
- Leave `RATE_LIMIT_ENABLED=true` on anything public.
- Terminate TLS with a real certificate; `SSL_ENABLED` only turns on when both
  the cert and key exist, so a missing cert silently serves plaintext.
