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

| Version | Status                | Window                                  |
|---------|-----------------------|-----------------------------------------|
| 0.6.x   | supported (current)   | through v1.0 GA (target Q4 2026)        |
| 0.5.x   | advisories only       | no patches; please upgrade to 0.6.x     |
| 0.4.x   | advisories only       | no patches; please upgrade to 0.6.x     |
| 0.1.x   | end-of-life           | no advisories; superseded by 0.4+ line  |
| 0.0.x   | end-of-life           | pre-public; no support                  |

## Project signing keys

Released artifacts are signed by the project's release key. Evidence
bundles emitted by individual deployments are signed by *their own*
ledger keys; the project does not co-sign customer bundles.

```
PGP fingerprint:  A1F2 9C4E B8D1 74E6 · 2A55 0CCB 7F12 BB37 5E4A C9D0
v0.6.0 sha256:    (cut at release · published in CHANGELOG.md and signed by the PGP key above)
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

## Website security model

The website at `asclepius.health` is part of the trust surface — a
visitor reading the docs is a visitor we have to defend. The site is
hardened on the same thesis as the kernel: **byte-level verifiability,
defense in depth, minimum capability**.

### Layered policy
| Layer                 | Where                | Carries                                                                  |
|-----------------------|----------------------|--------------------------------------------------------------------------|
| HTTP response headers | `site/_headers`      | HSTS, CSP, COOP, COEP, CORP, X-Frame-Options, Permissions-Policy, Accept-CH |
| HTML `<meta>` tags    | every `*.html`       | CSP, Referrer-Policy, X-Content-Type-Options, Permissions-Policy            |
| Service worker        | `site/sw.js`         | Integrity-gated cache (refuses to persist a hash-mismatched response)       |
| Asset attestation     | `js/attest-site.js`  | In-browser SHA-256 verification of each served byte vs `asset-manifest.json` |

The two CSP carriers are intentional. HTTP enforces on every response
including subresources; `<meta>` is the fallback for local previews and
servers that strip headers. Both are kept in sync.

### Content-Security-Policy (strict)
```
default-src 'self';
script-src 'self' 'inline-speculation-rules';
style-src 'self' 'unsafe-inline' https://fonts.googleapis.com;
font-src 'self' https://fonts.gstatic.com;
img-src 'self' data:;
connect-src 'self';
media-src 'self';
object-src 'none';
manifest-src 'self';
worker-src 'self';
base-uri 'self';
form-action 'self';
frame-ancestors 'none';        ⟵ HTTP only (meta-ignored per CSP spec)
upgrade-insecure-requests
```

What the CSP buys us:
- **No `'unsafe-inline'` on scripts.** Every previously inline `<script>`
  block was externalized to `js/page-<name>.js`, which is now covered by
  the SHA-256 in `asset-manifest.json`. The SW refuses to cache
  mismatches, and `attest-site.js` lets visitors verify the bytes they
  receive against the published manifest.
- **`object-src 'none'`** eliminates `<embed>` / `<object>` legacy plugin
  XSS vectors permanently.
- **`base-uri 'self'`** defeats `<base href="https://evil/">` injection.
- **`frame-ancestors 'none'`** + `X-Frame-Options: DENY` blocks
  clickjacking.
- **`upgrade-insecure-requests`** auto-upgrades any straggler `http://`
  reference to `https://`.
- **`'unsafe-inline'` on styles remains** (inline `<style>` blocks +
  Google Fonts CSS varies per User-Agent so SRI is not stable). This is
  the one accepted residual.

### Permissions-Policy
Every powerful capability the docs site can't possibly need is denied:
camera, microphone, geolocation, payment, USB, bluetooth, serial,
magnetometer, gyroscope, accelerometer, display-capture, MIDI, autoplay,
picture-in-picture, encrypted-media, idle-detection, screen-wake-lock,
xr-spatial-tracking, interest-cohort (FLoC). Only `fullscreen` and
`web-share` are granted `self`.

### Cross-origin isolation
- `Cross-Origin-Opener-Policy: same-origin` — strict process isolation.
- `Cross-Origin-Resource-Policy: same-origin` — own resources can't be
  consumed by attacker pages.
- `Cross-Origin-Embedder-Policy: credentialless` — high-resolution
  timers available, but cross-origin requests strip cookies (no
  ambient-auth leaks to embedded resources).

### Service-worker integrity gate
The SW pins `asset-manifest.json` on activate and verifies the SHA-256
of every same-origin response **before** caching it. A mid-flight
tampered byte stream still reaches the page (best-effort availability),
but is **never persisted**, so a single bad delivery cannot poison
offline reads on subsequent visits. HTML pages and Google Fonts CSS
remain SWR without integrity (HTML changes per release; fonts CSS
varies by UA).

### In-flight integrity for content the visitor sees
`js/attest-site.js` exposes a "verify this site" pill in every footer.
On demand, it fetches `asset-manifest.json` and re-hashes a sample of
served assets in the browser, displaying the verdict. The manifest is
regenerated on every release (`regen_manifest.py`) and ships with the
site.

### Out of scope (website)
- Network MITM below TLS — out of scope; HSTS preload + COOP/CORP/COEP
  are the maximum we can do from the application.
- A compromised Netlify/Cloudflare egress that swaps both the asset and
  the asset-manifest in lock-step — defeats the SW gate and the in-page
  attestation alike. Signed manifests (Sigstore transparency log) are
  on the roadmap for v1.0.
- Browser zero-days that bypass CSP — out of scope.
- DoS against the SW cache via crafted responses — bounded by browser
  origin quota; not a vulnerability.

## Acknowledgments

We list (with permission) every researcher whose disclosure resulted in a
fix in [the changelog](https://asclepius.health/changelog.html). Anonymous
reports are honoured.
