---
name: "[BUG]"
about: Report a crash, wrong response, or unexpected behavior in the server or balancer
title: ''
labels: bug
assignees: HilthonTT

---

**Describe the bug**
A clear and concise description of what the bug is.
 
**Component**
Which part is affected?
 
- [ ] HTTP server (`bin/codo`)
- [ ] Load balancer (`bin/codo-balancer`)
- [ ] B-tree storage engine / Todo API
- [ ] TLS / WebSocket handshake
- [ ] Build system (Makefile)
**To Reproduce**
Steps to reproduce the behavior:
 
1. Build with '...' (e.g. `make debug`)
2. Run '...' (e.g. `./bin/codo 8080 ./www`)
3. Send request '...' (paste the `curl` command)
4. See error
```bash
# Paste the exact curl / request and the response you got
```
 
**Expected behavior**
A clear and concise description of what you expected to happen (status code, body, headers, ...).
 
**Logs / output**
Paste any server output, stack traces, or ASan/UBSan reports. A `make debug` build (ASan + UBSan) gives the most useful output for crashes.
 
```
# Paste logs here
```
 
**Environment**
- OS / distro (e.g. Ubuntu 24.04, WSL2 on Windows 11):
- Compiler + version (`gcc --version` / `clang --version`):
- Build target used (`make debug` / `make release` / `make`):
- OpenSSL / zlib versions (if relevant):
- TLS enabled? (were `server.crt` / `server.key` present?):
- Relevant `.env` keys (`PORT`, `DOCUMENT_ROOT`, `DB_FILE`, `WAL_FILE`, `BALANCER_BACKENDS`, ...):
**Additional context**
- Is it reproducible every time, or intermittent (e.g. only under concurrent load / many keep-alive connections)?
- Does it involve a fresh data file, or an existing `codo.db` / `codo.wal`?
- Add any other context about the problem here.
