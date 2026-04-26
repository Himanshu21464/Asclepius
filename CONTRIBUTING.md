# Contributing to Asclepius

Thanks for thinking about contributing. Asclepius is a **trust substrate** —
the bytes it emits are the product. That shapes how we work.

## Before you write code

1. **Skim the [threat model](docs/THREAT_MODEL.md)** — every change has to
   honor what `verify()` is allowed to prove.
2. **Skim the [spec](docs/SPEC.md)** — the on-disk and on-wire formats are
   versioned; touching them needs a major bump.
3. **Read the [conformance ladder](https://asclepius.health/conformance.html)**
   — most useful PRs target a specific level.

## Substrate, not app

Asclepius is *not* an AI scribe, an MLOps platform, or an EHR. Features
that drift toward those will be politely declined. See the
[non-goals](https://asclepius.health/roadmap.html) section of the roadmap
for what we will *never* become.

## Workflow

```sh
# 1. fork + clone
git clone https://github.com/<you>/asclepius && cd asclepius

# 2. build + test (Linux: install libsodium-dev libsqlite3-dev first)
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build -j
ctest --test-dir build --output-on-failure

# 3. site sanity (if you touched site/)
node site/tests/run.mjs
cd site && python3 build_docs.py

# 4. branch, commit, push
git checkout -b feat/your-thing
git push -u origin feat/your-thing

# 5. open a PR — fill out the template
```

## Style

### C++

* C++20. `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion`
  must be clean. ASan+UBSan in Debug.
* `Result<T, Error>` for fallible APIs — no exceptions in the hot path.
* Strong typedefs for IDs (`ActorId`, `PatientId`, `EncounterId`,
  `ModelId`, `TenantId`).
* Pimpl for stable ABI on `Runtime`, `Ledger`, `KeyStore`, `Inference`.
* No copies for handle types; move-only.
* No emojis in source.
* Comments only for the *why*, never the *what*.

### JavaScript

* Vanilla ES2020. No build step. No framework.
* IIFE module pattern. `'use strict';` at the top.
* Feature-detect everything (`'IntersectionObserver' in window`,
  `window.fetch`, `crypto.subtle`).
* Respect `prefers-reduced-motion` and `forced-colors`.
* No third-party scripts at runtime.

### CSS

* CSS variables for design tokens. No SCSS/postcss.
* Both `[data-theme="dark"]` and `[data-theme="light"]` overrides for any
  new color.
* Universal `:focus-visible` rings on interactive elements.

## What needs spec / threat-model updates

Open a docs PR alongside the code PR if you touch:

- the layout of `LedgerEntry` or `LedgerEntryHeader`
- the canonical-JSON encoding of any field
- the sign-input bytes for a ledger entry
- the USTAR layout of an evidence bundle
- the SQLite schema
- any new `event_type` constant
- the consent / scope check in `Runtime::begin_inference`

## What needs a benchmark refresh

If you change anything in the inference hot path (`PolicyChain`, `Ledger::append`,
`KeyStore::sign`), rerun `examples/04_bench/example_bench --n 30000` and
update `site/assets/bench.json`.

## What needs a CHANGELOG entry

Anything user-visible. Both `CHANGELOG.md` (project root) and
`site/changelog.html` (the public page); the site changelog is itself a
Merkle log, so each new entry references the previous `entry_hash`.

## Sign your work

Every commit message body should end with `Signed-off-by: Your Name <email>`
to certify the [DCO](https://developercertificate.org/). Run `git commit -s`
to add it automatically.

## Security

Do **not** open a public GitHub issue for vulnerabilities. Use
[security@asclepius.health](mailto:security@asclepius.health) — see
[SECURITY.md](SECURITY.md) for the disclosure policy.

## Code of conduct

Be kind. Be specific. Don't claim verification you haven't run.
Don't merge code you haven't tested. Don't ship a substrate you wouldn't
deploy yourself.
