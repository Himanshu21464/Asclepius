# Storage backends

Asclepius ships with two interchangeable backends: **SQLite WAL** and
**PostgreSQL**. Same chain bytes, same entry hashes, same signatures —
the auditor's view is identical regardless of which one you pick. The
choice is operational, not a substrate-level commitment.

## Pick by URI

```
Ledger::open(path)                              # SQLite at <path>
Ledger::open_uri("file:///var/asc/log.db")      # SQLite (alias)
Ledger::open_uri("postgres://u:p@h/dbname")     # PostgreSQL
Ledger::open_uri("postgresql://u:p@h/dbname")   # alias
```

Same applies to `Runtime::open` / `Runtime::open_uri` and to every CLI
subcommand that takes a `<db>` argument.

## Decision matrix

| Property                         | SQLite WAL | PostgreSQL    |
|----------------------------------|------------|---------------|
| Append latency (median)          | **40 µs**  | 1.3 ms        |
| End-to-end inference p50         | **96 µs**  | 1.5 ms        |
| Operator footprint               | **zero**   | PG server + DBA |
| Single-binary deployment         | **yes**    | no            |
| Auditor handoff (`tar xf`)       | **yes**    | needs `pg_dump` |
| Cross-process subscription       | no         | yes (LISTEN/NOTIFY, planned) |
| HA / point-in-time recovery      | rsync the file | streaming replication |
| Multi-tenant via separate DBs    | painful    | trivial (`CREATE DATABASE`) |
| Concurrent writers per ledger    | 1 (file lock) | 1 (in-process mutex) |

Numbers from `examples/04_bench --n 1000` on the same machine, the same
day, against PostgreSQL 18.3.

## When to pick SQLite

- **Single-tenant sidecar** — one process per ledger, embedded next to
  the EHR or model server.
- **Lab / pilot deployments** — no infra team, no DBA.
- **Auditor handoff** — `tar xf db.sqlite` and the chain is inspectable
  with stock tools.
- **Edge / on-prem appliance** — small disk footprint, single process,
  rsync-able.

This is the default. `Ledger::open(path)` selects it.

## When to pick PostgreSQL

- **Multi-tenant SaaS hosting** many customer ledgers in one infra.
- **HA / replication required** — streaming replication, point-in-time
  recovery, cross-DC failover.
- **Cross-process subscription** — when several worker processes need
  to react to new entries (LISTEN/NOTIFY, planned).
- **Existing PG ops shop** — when DBA, backups, monitoring are already
  PG-shaped and a SQLite file would be the new exception.

`Runtime::open_uri("postgres://...")` selects it.

## What's identical between them

- Entry-hash bytes (same canonical-JSON, same signing key → same hash)
- Signatures (Ed25519, deterministic — same input + key produces the
  same signature regardless of backend)
- Verification semantics (`Ledger::verify()` accepts both equally)
- Chain integrity properties (append-only, gap-free seq, prev_hash chain)

Cross-backend determinism is enforced by a CI test:
`tests/test_postgres_backend.cpp::"[postgres] cross-backend determinism"`
appends a 3-entry chain to SQLite, exports JSONL, imports into PG, and
asserts every entry_hash, body, signature, and key_id matches
byte-for-byte.

## Migrating between backends

Two ways. Both preserve the chain — destination's `verify()` passes
against the source's signing key.

### Native cross-backend copy

```sh
asclepius ledger migrate /path/to/source.db postgres://user:pw@host/dbname
```

Signing key is auto-copied to the destination's expected location so
subsequent `verify` works without further setup.

### JSONL export + import (substrate-agnostic intermediary)

```sh
asclepius ledger export-jsonl /path/to/source.db /tmp/chain.jsonl
asclepius ledger import-jsonl /tmp/chain.jsonl postgres://user:pw@host/dbname \
    --key /path/to/source.key
```

JSONL is human-readable, `jq`-able, and portable across substrate
versions. Useful for cold archive or cross-environment transfer.

## What's not pluggable

- The signing scheme (Ed25519 only, see ADR-001 in
  `site/decisions.html`)
- The hash function (BLAKE2b-256, see ADR-002)
- The canonical-JSON encoding (ADR-005)
- The keystore (in-process; HSM/PKCS#11 backend planned)

A backend is just a place to put pre-signed bytes. It can't change
those primitives without breaking the substrate's promise.
