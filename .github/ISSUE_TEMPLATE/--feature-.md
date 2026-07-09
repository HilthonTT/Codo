---
name: " [FEATURE]"
about: Suggest an idea for the server, balancer, or storage engine
title: ''
labels: ''
assignees: HilthonTT

---

**Is your feature request related to a problem? Please describe.**
A clear and concise description of what the problem is. Ex. I'm always frustrated when [...]
 
**Describe the solution you'd like**
A clear and concise description of what you want to happen.
 
**Component**
Which part would this touch?
 
- [ ] HTTP server (`bin/codo`)
- [ ] Load balancer (`bin/codo-balancer`)
- [ ] B-tree storage engine / Todo API
- [ ] TLS / WebSocket
- [ ] Compression (gzip / zlib path)
- [ ] Build system / tooling
**Describe alternatives you've considered**
A clear and concise description of any alternative solutions or features you've considered.
 
**Scope / design notes**
- Does this fit the acceptor + worker-pool / per-worker `epoll` model, or does it change it?
- Any impact on the wire format, the storage engine (pages / WAL / checkpointing), or on-disk compatibility?
- Should it live in `common/` (shared) or in `server/` / `balancer/`?
- New `.env` keys or CLI args needed?
**Additional context**
Add any other context, references (RFCs, HTTP spec sections), or sketches about the feature request here.
