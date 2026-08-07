## What and why

<!-- What does this change, and what problem does it solve? Link the issue if there is one. -->

Closes #

## Component

- [ ] HTTP server (`bin/codo`)
- [ ] Load balancer (`bin/codo-balancer`)
- [ ] Storage engine (`storage/`)
- [ ] Web API / Todo resource (`api/`)
- [ ] Shared infrastructure (`common/`)
- [ ] Build system / CI / docs

## Verification

<!-- Paste the commands you ran and what you saw. "It builds" is not verification. -->

- [ ] `make clean && make` — builds with **zero** warnings
- [ ] `make clean && make debug` — ASan + UBSan build is clean
- [ ] Exercised the change against a running server (paste the `curl` and the response)

```
# commands + output
```

## Impact

- [ ] No change to the wire format, on-disk format, or existing status codes
- [ ] New/changed `.env` keys or CLI args — **documented in the README**
- [ ] New endpoint — registered in `api/`, added to the README endpoint table
- [ ] Touches the threading model (accept loop / workers / storage pool) — explain below

<!-- If any box above is unchecked because it does not apply, say so. If a change
     affects on-disk compatibility, describe the migration or state that existing
     codo.db / codo.wal files must be discarded. -->
