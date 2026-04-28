# Asclepius — Threat Model

What the audit substrate does and does not protect against. This is a
living document; every minor release should re-evaluate it.

## Assumptions

* The customer operates the Asclepius runtime inside their own trust
  boundary (their VPC, their hospital, their compliance footprint).
* The signing key is held by the runtime, with file-mode-0600 sidecar by
  default and HSM-backed in production.
* The wrapped model itself is not trusted to be benign. Output guardrails
  exist precisely because the model can be wrong, drifted, or adversarial.
* The clinician (`actor`) is authenticated by the customer's IdP before
  reaching Asclepius. We accept whatever subject claim the IdP issues.

## In-scope adversaries

### A1. Internal actor tampering with audit logs

An employee with database write access modifies a ledger entry to hide
that the model recommended a specific action.

**Mitigation:** The entry's signature is over the body and headers; modifying
either invalidates the signature. `Ledger::verify()` walks the entire chain
and detects mismatches at the offending sequence.

**Residual risk:** The actor could delete the entire database and replace
it with a chain they signed with a key they hold. Defense: pin the chain
head to an external system (a board-attested checkpointer, a trusted
notary, or a public ledger) on a regular cadence. Asclepius does not
implement notarization itself; it produces the artifacts that make
external notarization cheap.

### A2. Model produces malicious or PHI-laden output

The wrapped model outputs prompt-injection results, hallucinated PHI, or
clinically dangerous actions.

**Mitigation:** Output policies (PHI scrubber, schema validator, clinical
action filter, length limit) gate every output before it reaches a
clinician or downstream system. Blocks are recorded with rationale.

**Residual risk:** Sophisticated semantic attacks may slip past the
default scrubber. Compose with an ML-based scrubber (e.g. Presidio) for
production.

### A3. Replay of a stale model decision

An attacker replays an old `inference.committed` entry to confuse a
downstream auditor.

**Mitigation:** Entries are sequence-numbered and the chain is monotonic.
A replayed entry would not match the live chain.

### A4. Bundle forgery

An attacker generates a fake evidence bundle to mislead a regulator.

**Mitigation:** The bundle's `manifest.sig` is Ed25519-signed by the
ledger's signing key. The regulator's verifier needs only the public key
to detect a forgery. Bundles are also cross-checkable against the
chain: the `ledger_head` field in the manifest must match the actual
head of the customer's ledger.

### A5. Side-channel PHI leakage via ledger

An auditor with read access to the ledger learns PHI from the ledger
body.

**Mitigation:** By default, the ledger stores **hashes** of inputs and
outputs, not the content itself. The body of an `inference.committed`
entry contains identifiers, status, and hashes — no free text or imagery.
PHI-rich content storage is opt-in via a separate encrypted body-store.

### A6. Drift undetected

The model decays slowly enough that single-day metrics look fine.

**Mitigation:** PSI is computed against a fixed baseline (held-out
validation set), not against the previous day. Slow drift accumulates.
KS and EMD are also exposed for triangulation.

**Residual risk:** Drift detection is statistical; thresholds need
calibration per customer. Ship dashboards with severity transitions, not
just snapshots.

### A7. Emergency-override abuse

A clinician activates `EmergencyOverride` (DPDP § 7 break-glass) and
either fails to backfill the documented justification, or treats the
override as a permanent bypass for routine workflows.

**Mitigation:** Every activation writes a `consent.override.granted`
ledger entry immediately, with the actor, patient, free-text reason,
and a hard `backfill_deadline` (default 72h). The kernel surfaces
`overdue_backfills()` for dashboards and `is_overdue(token_id)` for
per-token alerts. Backfill takes an `evidence_id` the operator's
compliance regime accepts (signed clinical note, scanned consent, ABDM
artefact id) — the substrate is policy-neutral about *what* satisfies
the backfill but enforces *that* one is filed before the deadline.

**Residual risk:** A clinician who falsifies a backfill evidence id
still bypasses the deadline. The substrate cannot tell a real signed
note from a fabricated one — that is a downstream attestation question.
Mitigation: pair the backfill window with a sampled human-review
sweep over the actor population.

### A8. Family-graph forgery

A proxy (claimed adult child, parent, guardian, spouse) asserts
authority over a subject they have no real-world relation to, in order
to extract that subject's records via consent issued on the subject's
behalf.

**Mitigation:** `FamilyGraph::record_relation` writes a
`consent.family.recorded` ledger entry on every edge addition; the
audit trail is complete and append-only. Consents granted on a
subject's behalf are honoured iff a valid edge exists at consent-grant
time, not at consent-use time — backdating an edge does not
retroactively unlock past inferences. Proxy identity is enforced by
`PatientId` strong typedef, so a proxy with no `PatientId` cannot be
recorded.

**Residual risk:** The graph trusts the operator who records edges.
Asclepius does not adjudicate civil-status claims — that is the
identity provider's problem (ABDM consent manager, eIDAS, hospital
registration). Mitigation: edges should be sourced from the same
identity provider that issues the proxy's `PatientId`, never from the
clinical workflow.

## Out-of-scope (today)

* **Compromise of the signing key.** If an attacker exfiltrates `<db>.key`
  they can forge ledger entries and evidence bundles. Mitigation: HSM-back
  the keystore and rotate keys on a schedule (planned: Asclepius supports
  multiple `key_id`s in the same chain).
* **Compromise of the host.** Root on the box loses everything.
* **Insider with both DB write and key access.** No software defense is
  sufficient; rely on access controls and auditing.
* **Quantum adversaries.** Ed25519 is not post-quantum. The format is
  versioned; we will migrate to a post-quantum signer when one is
  standardized.
* **Bundle exfiltration.** A bundle contains hashes and metrics, not PHI
  by default. If a bundle is configured to include PHI, treat it as PHI.

## What `verify()` proves

`Ledger::verify()` returns `Ok` iff:

1. Every entry's `seq` is strictly monotonic from 1.
2. Every entry's `prev_hash` equals the previous entry's `entry_hash`.
3. Every entry's `payload_hash` matches `BLAKE2b256(event_type || RS || body)`.
4. Every entry's `signature` verifies against the ledger's public key.

It does **not** prove:

* That the recorded data reflects what really happened (garbage-in
  garbage-out — see A1's residual risk).
* That the public key belongs to the entity you think it does (key
  binding is a customer/governance concern).
* That the wrapped model was the model the entry says it was (model
  attestation requires hash-binding the model artifact, planned).

## Reporting

Suspected vulnerabilities: security@asclepius.health (placeholder; the
project is pre-public). PGP and our Vulnerability Disclosure Policy will
ship with the first public release.
