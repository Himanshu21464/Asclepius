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
| `inference.committed`       | `Inference::commit`         | one row per successful inference (or one with `status` != `ok`) |
| `inference.aborted`         | `~Inference`                | inference ran but was never committed                            |
| `evaluation.ground_truth`   | `EvaluationHarness`         | ground truth attached for an inference                           |
| `evaluation.override`       | `EvaluationHarness`         | clinician overrode the model output                              |
| `consent.granted`           | `ConsentRegistry`           | a consent token was issued                                       |
| `consent.revoked`           | `ConsentRegistry`           | a consent token was revoked                                      |
| `consent.family.recorded`   | `FamilyGraph::record_relation` | a proxy-for-subject family edge was added (India profile, ADR-013) |
| `consent.family.removed`    | `FamilyGraph::remove_relation` | a family edge was removed                                       |
| `consent.override.granted`  | `EmergencyOverride::activate`  | DPDP § 7 break-glass activated; carries actor, patient, reason, deadline |
| `consent.override.attested` | `EmergencyOverride::backfill`  | break-glass backfilled within window with an `evidence_id`     |
| `consent.override.expired`  | `EmergencyOverride` (sweeper)  | a backfill window passed without an attestation; surfaces in `overdue_backfills()` |
| `consent.artefact.issued`   | `append_consent_artefact_issued` | ABDM-shaped consent artefact emitted to a HIU (India profile, ADR-013, round 90) |
| `consent.artefact.revoked`  | `append_consent_artefact_revoked` | ABDM-shaped consent artefact recall                                              |
| `override.emergency.activated`  | `append_emergency_override_activated`  | round 98 helper: DPDP § 7 break-glass activated with deadline           |
| `override.emergency.backfilled` | `append_emergency_override_backfilled` | round 98 helper: backfill filed with an `evidence_id`                   |
| `human.attestation`         | `append_human_attestation`     | Ed25519-signed clinician sign-off (round 92, ADR-013); body = `attestation_to_json` shape |
| `consult.tele.closed`       | `append_tele_consult`          | round 95: two-party signed `TeleConsultEnvelope` for SpecHub-style specialist consult |
| `bill.audited`              | `append_bill_audit`            | round 95: auditor-signed `BillAuditBundle` with line-finding severity vs reference (CGHS-2025) |
| `sample.collected`          | `append_sample_integrity` (1/2)| round 96: VanRoute-style sample chain-of-custody, collection event                 |
| `sample.resulted`           | `append_sample_integrity` (2/2)| round 96: same `SampleIntegrityBundle`, result event                               |
| `rx.parsed`                 | well-known (round 92)          | a prescription was parsed (caller-provided body)                                   |
| `rx.substitution`           | well-known (round 92)          | a generic-for-branded substitution recorded                                        |
| `triage.decision`           | well-known (round 92)          | a triage classifier produced a decision; pairs with `CalibrationMonitor`            |
| `care.path.allow`           | `append_care_path` (decision=allow) | round 96: `CarePathAttestation` signed proof of access::Constraint match     |
| `care.path.deny`            | `append_care_path` (decision=deny)  | round 96: signed proof of access::Constraint deny + reason                   |
| `policy.config_change`      | runtime config (planned)    | a policy was added, removed, or reconfigured                     |
| `model.registered`          | governance (planned)        | a model id+version was registered for use                        |

Round 92's `events::` namespace declares twelve canonical
event-type strings that callers should prefer over free-form spelling.
`is_well_known_event(string_view)` and `well_known_events()` are the
discoverability primitives. The round 98 typed-append helpers
(`append_human_attestation`, `append_consent_artefact_issued`,
`append_consent_artefact_revoked`, `append_tele_consult`,
`append_bill_audit`, `append_sample_integrity`, `append_care_path`,
`append_emergency_override_activated`,
`append_emergency_override_backfilled`) write the canonical event_type
and a deterministic body shape so dashboards and conformance tests can
parse without per-call schema knowledge.

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
