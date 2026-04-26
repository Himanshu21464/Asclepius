# Changelog

This file mirrors the cryptographically chained version history at
[asclepius.health/changelog.html](https://asclepius.health/changelog.html).
Each entry references the previous entry's `entry_hash` — the changelog is
itself a Merkle log.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [0.1.0] — 2026-04-25 — *the substrate, open-sourced*

> entry_hash: `92b4abe6e71bed1a184b684f943ee2ddad84fd016210855a12469b3bb23f4151`
> prev_hash:  `0000…0000` (genesis)
> signed by:  ed25519 · key_id `7ad15100a824332a`
> artifact:   `libasclepius-0.1.0.tar.gz` · sha256 `c4a11e…b9`

First public release. Apache 2.0. The substrate is feature-complete along
its primary axis: every clinical-AI inference becomes a signed Merkle
ledger row, guarded by a composable policy chain, gated by consent,
observed for drift, and exportable as a regulator-verifiable evidence
bundle.

### Added
- C++20 runtime with `Runtime`, `Inference`, `Ledger`, `PolicyChain`,
  `DriftMonitor`, `ConsentRegistry`, `EvaluationHarness`.
- Append-only Merkle ledger over SQLite WAL. BLAKE2b-256 + Ed25519.
  `asclepius ledger verify` walks the chain in O(n).
- Policies: `phi_scrubber`, `schema_validator`, `clinical_action_filter`,
  `length_limit`.
- USTAR evidence bundles, signed manifest, offline-verifiable.
- Python bindings (pybind11), CLI (`asclepius`), four worked examples
  (wrap_scribe, drift_demo, evidence_bundle, bench).
- 52 doctest cases / 687 assertions, all green.

### Known limitations
- HTTP/gRPC sidecar (`asclepius-svc`) is reserved-only — the proto is
  published.
- Postgres backend is single-node SQLite-only for now.
- HSM-backed signing is a vended interface; no reference adapter yet.

---

## [0.0.9] — 2026-04-12 — *internal* — L1 audit suite green

> entry_hash: `5b1e44a90c07e4b18927c81d6ef02a4471f0a2839a26f6c9301bb7ef1140a8c2`
> prev_hash:  `e9f1a07f8de2ad7b603c19f4d6c0e1a2b51bce7a4c0f7019f50bdfb74e1ea30c`

Pinned the on-disk ledger format (spec.md v1) and ran the L1 conformance
suite end-to-end against three reference workloads.

### Changed
- Canonical-JSON encoding spec frozen; sorted keys, no whitespace, UTF-8.
- Sign-input layout fixed: `seq | ts | prev | payload | actor | event_type | tenant | body`.
- Drift severity bands tuned against published clinical-AI deployment data.

---

## [0.0.7] — 2026-03-19 — *internal* — evidence bundles ship

> entry_hash: `e9f1a07f8de2ad7b603c19f4d6c0e1a2b51bce7a4c0f7019f50bdfb74e1ea30c`
> prev_hash:  `b3047d22fe1c8a4e5a6b8901c2edf5a9e3a1de30c6d2f74b08e03a7ba9e8c45f`

USTAR tar emit + offline-verify. `asclepius evidence bundle` and
`asclepius evidence verify` CLI commands. The bundle's manifest is signed
by the same key as the ledger.

---

## [0.0.5] — 2026-02-28 — *internal* — drift on the ground floor

> entry_hash: `b3047d22fe1c8a4e5a6b8901c2edf5a9e3a1de30c6d2f74b08e03a7ba9e8c45f`
> prev_hash:  `a14eb27d3a591f6c0ebd02c8e94a01ff1c6d8e2b9a04e3b76025f0a1d2c4e89b`

`DriftMonitor` with PSI / KS / EMD on per-feature reference vs. current
histograms. Severity bands match conventional thresholds.

---

## [0.0.3] — 2026-02-04 — *internal* — policy chain composed

> entry_hash: `a14eb27d3a591f6c0ebd02c8e94a01ff1c6d8e2b9a04e3b76025f0a1d2c4e89b`
> prev_hash:  `76a0930fe16cf2b1b0d1c1e0a8f47ab09e2b0daa3c5ee61e7b5ddff80c1a4327`

First version of the `PolicyChain` with `allow / modify / block` verdicts.
PHI scrubber + JSON-Schema-subset validator.

---

## [0.0.1] — 2026-01-08 — *internal* — genesis

> entry_hash: `76a0930fe16cf2b1b0d1c1e0a8f47ab09e2b0daa3c5ee61e7b5ddff80c1a4327`
> prev_hash:  `0000…0000` (genesis)

First commit. Append-only signed log over SQLite. `verify()` walks the
chain. No policies, no consent, no drift — just the substrate's spine.

---

[0.1.0]: https://github.com/Himanshu21464/Asclepius/releases/tag/v0.1.0
