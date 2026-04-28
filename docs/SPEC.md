# Asclepius — On-Disk and On-Wire Specification

Version 0.6.0 — 2026-04. This is a living document; until 1.0 the formats
may break compatibility but breakage will be called out in CHANGELOG.

## Hash function

* `BLAKE2b-256` (libsodium `crypto_generichash` with default key, 32-byte
  output). Length is **32 bytes / 64 hex chars**.

## Signature scheme

* `Ed25519` (libsodium `crypto_sign_detached`).
* Public key:    32 bytes.
* Secret key:    64 bytes (libsodium representation).
* Signature:     64 bytes.
* Key id:        first 16 lowercase hex chars of the public key.

## Time

* Wall-clock nanoseconds since the Unix epoch as a 64-bit signed integer.
* ISO-8601 string form: `YYYY-MM-DDTHH:MM:SS.fffffffffZ`.

## Canonical JSON

All ledger bodies and bundle artifacts are serialized with:

* UTF-8 encoding,
* Object keys sorted lexicographically (nlohmann::json `dump(-1)` produces
  the indentation-free form; key sorting is the default in dump),
* No insignificant whitespace.

Two canonical encodings are byte-equal iff the underlying values are
JSON-equal.

## Ledger entry

Logical structure:

```
LedgerEntry := {
  seq:          u64,
  ts:           Time,
  prev_hash:    Hash,
  payload_hash: Hash,
  actor:        utf8,
  event_type:   utf8,
  tenant:       utf8,
  body:         CanonicalJSON,
  signature:    Sig64,
  key_id:       utf8 (16 hex)
}
```

Derived fields:

```
payload_hash = BLAKE2b256(event_type || 0x1E || canonical_json(body))
sign_input   = u64_le(seq) || u64_le(ts_ns) ||
               prev_hash || payload_hash ||
               actor || ',' || event_type || ',' || tenant || ',' || body
signature    = Ed25519_sign(sk, sign_input)
entry_hash   = BLAKE2b256(
                  seq_dec || '|' || ts_ns_dec || '|' ||
                  prev_hash || '|' || payload_hash || '|' ||
                  actor || '|' || event_type || '|' || tenant || '|' ||
                  body || '|' || signature || '|' || key_id
               )
```

The chain invariant is: for entry at `seq = N`, `prev_hash = entry_hash(N-1)`.
The first entry has `prev_hash = 0x00...00` and `seq = 1`.

## SQLite schema

```sql
PRAGMA journal_mode = WAL;
PRAGMA synchronous  = NORMAL;

CREATE TABLE asclepius_meta (
  key TEXT PRIMARY KEY,
  val BLOB
);

CREATE TABLE asclepius_ledger (
  seq          INTEGER PRIMARY KEY,
  ts_ns        INTEGER NOT NULL,
  prev_hash    BLOB    NOT NULL,
  payload_hash BLOB    NOT NULL,
  actor        TEXT    NOT NULL,
  event_type   TEXT    NOT NULL,
  tenant       TEXT    NOT NULL DEFAULT '',
  body         TEXT    NOT NULL,
  signature    BLOB    NOT NULL,
  key_id       TEXT    NOT NULL,
  entry_hash   BLOB    NOT NULL
);

CREATE INDEX idx_ledger_ts ON asclepius_ledger(ts_ns);
```

The signing key is stored in a sidecar file `<db>.key` with mode 0600
containing `ASCLEPIUS-KEY-v1:<pk_hex>:<sk_hex>`. Production deployments
should override the sidecar with a HSM-fronted KeyStore.

## Standard event types

| event_type                  | source            | semantics                                                       |
|-----------------------------|-------------------|-----------------------------------------------------------------|
| `inference.committed`       | `Inference::commit`     | one row per successful inference (or one with `status` != `ok`) |
| `inference.aborted`         | `~Inference`            | inference ran but was never committed                            |
| `evaluation.ground_truth`   | `EvaluationHarness`     | ground truth attached for an inference                           |
| `evaluation.override`       | `EvaluationHarness`     | clinician overrode the model output                              |
| `consent.granted`           | `ConsentRegistry` (planned) | a consent token was issued                                      |
| `consent.revoked`           | `ConsentRegistry` (planned) | a consent token was revoked                                     |
| `policy.config_change`      | runtime config (planned)  | a policy was added, removed, or reconfigured                    |
| `model.registered`          | governance (planned)      | a model id+version was registered for use                       |

The `body` shape per event_type is fixed; future versions will publish a
JSON-Schema for each.

## Evidence bundle (USTAR tar)

```
manifest.json
ledger_excerpt.jsonl
metrics.json
drift.json
overrides.jsonl
ground_truth.jsonl
public_key            (32 raw bytes)
manifest.sig          (64 raw bytes)
```

`manifest.json`:

```json
{
  "asclepius_version": "0.1.0",
  "window":  { "start": "...", "end": "..." },
  "files":   {
    "ledger_excerpt.jsonl": "<hex32>",
    "metrics.json":         "<hex32>",
    "drift.json":           "<hex32>",
    "overrides.jsonl":      "<hex32>",
    "ground_truth.jsonl":   "<hex32>"
  },
  "ledger_head": "<hex32>",
  "key_id":      "<16 hex>",
  "exported_at": "ISO8601"
}
```

`manifest.sig` = Ed25519 signature over the bytes of `manifest.json` using
the ledger's signer.

A verifier needs only `manifest.json`, `manifest.sig`, and `public_key`
to authenticate the bundle, then re-hashes the named files to confirm
their integrity.

## Versioning policy

* Patch (`0.1.x`): bug fixes; binary-compatible.
* Minor (`0.x.0`): backward-compatible additions; new event types,
  optional fields. Old verifiers ignore unknown fields.
* Major (`x.0.0`): incompatible changes. A migration script ships with
  every major bump. Bundles emitted by version `N` continue to verify
  with the same major version forever.
