# Contributing to Codo

Thanks for taking the time. Codo is a from-scratch HTTP/1.1 server in C11 — the
point of the project is that the interesting parts are readable, so a change
that adds a dependency or hides a mechanism behind a library is usually the
wrong change.

## Getting set up

Codo is **Linux only** (`epoll`, `sendfile`, `accept4`, `getrandom`). On Windows
use WSL2.

```sh
sudo apt-get install -y build-essential libssl-dev zlib1g-dev
make            # bin/codo + bin/codo-balancer
make run        # build and start the server on :8080
```

`.env` in the repo root is the default config; see the table in the
[README](README.md#configuration). CLI args override the environment, which
overrides `.env`.

Generate `compile_commands.json` for clangd/IntelliSense (it is git-ignored, as
it embeds absolute paths):

```sh
bear -- make        # or: compiledb make
```

## Before you open a PR

Both of these run in CI, so run them locally first:

```sh
make clean && make         # must build with zero warnings
make clean && make debug   # ASan + UBSan build
```

The build uses `-Wall -Wextra -Wpedantic`. **A new warning is a failed review** —
fix the cause rather than casting it away.

There is no unit-test suite yet. Correctness is covered by the CI smoke tests,
which boot the server on a throwaway port and exercise static files, the API,
auth, and the owner index end to end. If you change request handling or storage
behavior, add a case to `.github/workflows/ci.yml` alongside the existing ones.

## Style

- C11, 2-space indent, Allman braces (opening brace on its own line). Match the
  file you are editing.
- Comments explain **why**, not what. The existing comments are the model: they
  document the reason a lock is held, why an order is deliberate, or what
  breaks if you change it. Do not add comments that restate the code.
- Keep `common/` free of server types. It holds what does not depend on
  `http_server_t`; see the seams described in [DESIGN.md](DESIGN.md).
- New endpoints belong in `api/`, not `server/`. The server core knows nothing
  about what is being served.

## Commit messages and PRs

- One logical change per PR. A cleanup and a feature in the same diff is two PRs.
- Explain the *why* in the PR description, and say how you verified it (which
  commands you ran, what output you saw).
- If a change is user-visible — a new `.env` key, a new endpoint, a changed
  status code — update the README in the same PR.

## Things worth working on

The known gaps are listed at the end of [DESIGN.md](DESIGN.md). The largest are
B-tree page splits (today the database is bounded by a single page), WAL crash
recovery, transaction undo on abort, and an actual test suite.

## Security

Do not open a public issue for a vulnerability. See [SECURITY.md](SECURITY.md).
