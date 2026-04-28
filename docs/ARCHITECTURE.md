# Asclepius — Architecture

This document describes the runtime topology, the data flow for a single
clinical-AI inference, and the rationale behind each component.

## Component map

```
                       +----------------------+
                       |  Caller (your tool)  |
                       +-----------+----------+
                                   |
                                   v
                       +----------------------+
                       |  Runtime             |
                       |  - begin_inference   |
                       |  - policies()        |
                       |  - drift()           |
                       |  - consent()         |
                       |  - evaluation()      |
                       |  - metrics()         |
                       +----+-----+-----+-----+
              consent check | run | commit
                            v     v     v
            +-----------+--+----+--+----+-----------+
            | PolicyChain|        |    | Ledger    |
            | input/out  |        |    | (Merkle,  |
            | guards     |        |    | signed)   |
            +-----+------+        |    +-----------+
                  |               v          ^
                  v        +-----------+     |
            (model call)   | Drift     |-----+
                           | Monitor   | (drift events appended on rotate)
                           +-----------+
                                |
                                v
                       +-----------------+
                       | EvaluationHarness
                       | - ground truth  |
                       | - overrides     |
                       | - bundle export |
                       +-----------------+
```

The layers are deliberately decoupled: PolicyChain knows nothing about
Ledger, Ledger knows nothing about Drift. The Runtime is the only object
that wires them, and it does so through narrow interfaces. Callers can
swap any layer without touching the rest.

## Data flow for a single inference

1. **Caller** prepares an `InferenceSpec` (model, actor, patient, encounter,
   purpose, optional consent token).
2. **Runtime::begin_inference** validates consent. The check is either
   token-scoped (caller supplied a `consent_token_id`) or registry-wide
   (`permits(patient, purpose)` returns true).
3. The runtime mints an `inference_id` (time-sortable + counter), stamps
   `started_at`, and returns an `Inference` RAII handle.
4. **Inference::run(input, model_call)**:
   a. The input is fed through `PolicyChain::evaluate_input`. Each policy
      may `allow`, `modify` (rewrite the payload), or `block`. The first
      `block` short-circuits with a `policy_violation` error.
   b. The (possibly rewritten) input is hashed; only the hash is recorded
      to the ledger by default. PHI never enters the ledger unless the
      caller explicitly opts in.
   c. The model callback is invoked synchronously. Errors propagate as
      `ErrorCode::internal` and are recorded.
   d. The model output is fed through `PolicyChain::evaluate_output`. Same
      semantics as input.
   e. The output hash, latency, and status are added to a deferred
      `ledger_body` JSON blob.
5. **Inference::commit()** appends a single `inference.committed` entry to
   the ledger. The append is atomic: the new entry's `prev_hash` is the
   chain head, the body is canonical-JSON, an Ed25519 signature covers the
   header + body, and the SQLite row is written within a transaction.
6. **Inference::~Inference**: if the inference completed but was never
   committed, an `inference.aborted` entry is appended best-effort.

## Why a Merkle-chained signed ledger

A linear, append-only log signed by a single key is enough to detect
tampering by any party who lacks the secret key. The Merkle chain (each
entry includes the previous entry's hash) makes detection cheap: verifying
the chain is O(n), and pinning the head out-of-band gives external proof
of state at a point in time.

We did not use a blockchain. There is no decentralized trust to bootstrap;
the trust anchor is the operator's public key. A blockchain would buy us
nothing and cost us latency and complexity.

## Why SQLite

SQLite's WAL mode gives durable, single-writer-many-reader semantics with
no daemon. The substrate is single-node by design — sidecars get one DB
file each, one trust anchor each, one chain each. *One backend, less
drift.* A previous prototype shipped a parallel Postgres backend; we
ripped it in v0.4.0 after benchmarks came in 30× in SQLite's favour and
the substrate-not-app posture made shared-write semantics irrelevant.

## Why Ed25519 + BLAKE2b

* **BLAKE2b-256** for hashing: faster than SHA-256, stronger margin,
  first-class libsodium support, 32-byte digest is unwieldy enough to
  prevent accidental collisions.
* **Ed25519** for signatures: 32-byte public key, 64-byte signature,
  deterministic, fast verification, side-channel resistant, no parameter
  decisions to get wrong.

We did not use ECDSA P-256 or RSA. ECDSA leaks the private key on RNG
failures during signing; RSA is slower and bigger. Ed25519 is the right
default in 2026.

## Why C++20

* **Inline latency.** Pre/post guards run on every inference. We need
  microseconds, not milliseconds.
* **Cryptographic throughput.** Hashing and signing across millions of
  daily inferences requires native crypto without a GIL.
* **Determinism.** Replay must reproduce byte-equal artifacts. C++ with
  canonical JSON serialization gives that.
* **Long-lived ABI.** The C ABI exposed by the library is stable; passes
  evolve underneath.

The Python and (planned) Rust bindings are convenience shells over the
same core.

## Storage model

The SQLite schema has two tables:

```
asclepius_meta(key TEXT PRIMARY KEY, val BLOB)
asclepius_ledger(
  seq          INTEGER PRIMARY KEY,
  ts_ns        INTEGER NOT NULL,
  prev_hash    BLOB    NOT NULL,
  payload_hash BLOB    NOT NULL,
  actor        TEXT    NOT NULL,
  event_type   TEXT    NOT NULL,
  tenant       TEXT    NOT NULL DEFAULT '',
  body         TEXT    NOT NULL,    -- canonical JSON
  signature    BLOB    NOT NULL,
  key_id       TEXT    NOT NULL,
  entry_hash   BLOB    NOT NULL
)
```

`payload_hash` is `BLAKE2b256(event_type || RS || canonical_json(body))`.
`entry_hash` is `BLAKE2b256(seq | ts | prev | payload | actor | event_type | tenant | body | signature | key_id)`.
The signature covers (seq, ts, prev, payload, actor, event_type, tenant, body),
not the entry_hash itself.

## Evidence bundles

A bundle is a USTAR tar containing:

```
manifest.json         hashes of every other file, key id, window, version
ledger_excerpt.jsonl  the in-window subset of ledger entries
metrics.json          per-model rates (overrides, blocks, accuracy if known)
drift.json            DriftMonitor report at window close
overrides.jsonl       captured override events
ground_truth.jsonl    captured ground-truth events
public_key            32 bytes; the ledger's signer
manifest.sig          64 bytes; Ed25519 signature over manifest.json
```

Verification is offline: confirm `manifest.sig` against the contained
`public_key`, then re-hash each named file and confirm against the
manifest. No network access required.

## Threading model

* `Ledger::append` takes a per-ledger mutex — single-writer.
* `Ledger::at`, `tail`, `range`, `verify` use SQLite's connection-internal
  locking and are safe to call from any thread holding a reference to the
  Ledger.
* `PolicyChain::evaluate_*` is read-only on the chain; pushing policies
  is not safe to do concurrently with evaluation. Configure the chain
  before starting traffic.
* `DriftMonitor` is fully thread-safe.
* `Runtime::begin_inference` is thread-safe; concurrent inferences are the
  expected case.

## What is NOT in the runtime

* **PHI storage.** The ledger holds hashes by default. Customers who need
  to inspect PHI use the encrypted-at-rest body-store (planned).
* **Model serving.** Asclepius wraps a model callback; it doesn't load
  weights, route requests, or speak to vLLM. That's the customer's job.
* **A FHIR server.** Asclepius is FHIR-aware (parse references, extract
  scope from a bundle), not FHIR-implementing. Use HAPI or Aidbox for
  full FHIR.
* **An identity provider.** Actors are opaque strings; they come from
  whatever IdP the customer already runs.

## Profile primitives (added in v0.6 — ADR-013)

Three vendor-neutral types layer on `ConsentRegistry` to express
multi-party and break-glass consent shapes the bare token model does
not. They are deliberately *not* wired into the Runtime as required
components — a deployment that does not instantiate them is
unaffected. The first profile that exercises them is India / ABDM, but
the types are regime-agnostic.

* **`ConsentArtefact`** — JSON-serialisable wire shape for an outbound
  consent record. Bidirectional mapping with `ConsentToken` via
  `artefact_from_token` / `token_from_artefact`. Externally signable.
* **`FamilyGraph`** — proxy-for-subject relation graph. The registry
  consults the graph alongside the token: a token issued by a proxy on
  a subject's behalf is honoured iff a valid edge exists at *grant
  time*. Edge writes emit `consent.family.recorded` /
  `consent.family.removed` ledger events.
* **`EmergencyOverride`** — DPDP § 7 break-glass with a configurable
  mandatory backfill window (default 72h). Activation emits
  `consent.override.granted` with actor / patient / reason / deadline;
  backfill emits `consent.override.attested` with a caller-supplied
  `evidence_id`; the sweeper emits `consent.override.expired` for any
  token whose deadline passes without attestation.

The associated threat model is documented at `THREAT_MODEL.md` § A7
(emergency-override abuse) and § A8 (family-graph forgery). The wire
events are listed in `SPEC.md` § Standard event types.

## Future work

* **Object-store body archive.** Encrypted-at-rest storage of full inputs
  and outputs for customers that need replay-with-content.
* **HTTP/gRPC sidecar (`asclepius-svc`).** For non-C++ tools.
* **Policy DSL.** Today policies are C++ classes. A YAML/JSON DSL with a
  linter (`asclepius policy lint`) is on the roadmap.
* ~~**Conformance suite.** Levels L1 (audit), L2 (drift), L3 (prospective
  evidence) with a third-party-runnable test battery.~~ Shipped in
  v0.5–v0.6: L1 / L2 / L3 plus the L2-Medical scaffold against the
  ACL 2025 Asclepius-Med benchmark. See `/conformance`.
