# Security policy

Asclepius is a trust substrate. A vulnerability in this code is a
vulnerability in every clinical-AI deployment that uses it. We treat
reports the way the project asks others to: with cryptographic care,
written promptly, fixed at the root.

## Reporting a vulnerability

Email **[security@asclepius.health](mailto:security@asclepius.health)**.
Encrypt with the PGP key (fingerprint below) if the finding involves an
active exploit.

If you are not sure whether something is in scope, send it anyway — we'd
rather triage a non-issue than miss one.

## Scope

### In scope

- Cryptographic flaws: chain forgery, signature bypass, hash collisions,
  key-derivation issues.
- Memory safety in the C++ core: buffer overflow, use-after-free,
  double-free.
- Injection: SQL injection in `asclepius-cli`, prompt-injection that
  escapes the policy chain.
- Verification bypass: a tampered bundle that `verify_bundle()` accepts.
- Consent / scope bypass: an inference reaching the model despite missing
  or revoked consent.
- Side channels: PHI leakage through ledger metadata, drift histograms,
  or evidence bundles.

### Out of scope

- The wrapped model's outputs (that's what the policy chain is for —
  report a missing policy as an enhancement instead).
- Operational misconfigurations on a customer deployment, unless the
  project documentation explicitly recommended the misconfiguration.
- Denial of service via legitimate ledger growth — write a benchmark, not
  a CVE.
- Theoretical post-quantum risk (we know; v1.0 ships ML-DSA migration).

## Supported versions

| Version | Status                | Window               |
|---------|-----------------------|----------------------|
| 0.1.x   | supported             | through Q4 2026      |
| 0.0.x   | advisories only       | no patches           |

## Project signing keys

Released artifacts are signed by the project's release key. Evidence
bundles emitted by individual deployments are signed by *their own*
ledger keys; the project does not co-sign customer bundles.

```
PGP fingerprint:  A1F2 9C4E B8D1 74E6 · 2A55 0CCB 7F12 BB37 5E4A C9D0
v0.1.0 sha256:    c4a11ee3742d8a915d0bd9ea80c7edc0e21fa3b78a1c9e60d4c6f72a5bb09c12
```

## What to expect

| Stage                   | Window                                              |
|-------------------------|-----------------------------------------------------|
| Acknowledgement         | within 48 business hours                            |
| Triage + severity       | within 5 business days                              |
| Fix on `main` + release | critical: 14 days; high: 30; medium: 60             |
| Coordinated disclosure  | 90 days from triage, or 7 days after the patch      |
| Credit                  | with permission, in the changelog                   |

We will work with you to extend the disclosure window if there is a
responsible reason to.

## Public-facing security artefacts

- [/.well-known/security.txt](https://asclepius.health/.well-known/security.txt) — RFC 9116
- [security.html](https://asclepius.health/security.html) — public security policy
- [threat-model.html](https://asclepius.health/threat-model.html) — adversaries A1–A6

## Acknowledgments

We list (with permission) every researcher whose disclosure resulted in a
fix in [the changelog](https://asclepius.health/changelog.html). Anonymous
reports are honoured.
