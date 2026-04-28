# Asclepius — project notes for future iterations

This file is read on every Claude Code session in this directory; it's the
single source of truth for *how* this project is organized and the
conventions it follows. The content of the project lives in the source.

## What it is

Asclepius is a **trust substrate for clinical AI**: an open, vendor-neutral
C++20 runtime that wraps any clinical-AI tool with audit-grade safety —
signed Merkle ledger, composable policy chain, drift detection, consent
registry, regulator-grade evidence bundles. Apache 2.0. Tagline: *"the
Linux kernel for clinical AI."*

It belongs to a portfolio of "Future Project Briefs" alongside Vita, Kepler,
Mnemos, and NeuroKernel. Each brief is a substrate-layer infrastructure
play, not an app — the project pattern is "X for Y" (LLVM for biology,
Kubernetes for self-driving labs, Linux kernel for clinical AI).

## Layout

```
.                       project root — Apache 2.0
├── Asclepius.tex/.pdf  Future Project Brief (LaTeX + compiled PDF)
├── README.md           public-facing project README
├── CLAUDE.md           this file
├── CMakeLists.txt      C++20 build, FetchContent deps, sanitizers
├── cmake/              Compiler warnings, sanitizers, dep finder
├── include/asclepius/  public C++ headers (the API contract)
├── src/                core implementations
│   ├── core/           Result/Error/Time/Id/hashing
│   ├── policy/         PHI scrubber, schema, action filter, length
│   ├── audit/          Merkle ledger, KeyStore, SQLite backend
│   ├── telemetry/      DriftMonitor (PSI/KS/EMD), MetricRegistry
│   ├── consent/        ConsentRegistry, Purpose enum
│   ├── evaluation/     EvaluationHarness, USTAR bundle exporter
│   ├── runtime/        Runtime, Inference (RAII handle)
│   └── interop/        FHIR-aware metadata helpers
├── apps/cli/           `asclepius` CLI binary
├── apps/svc/           HTTP/gRPC sidecar (placeholder)
├── bindings/python/    pybind11 → `import asclepius`
├── examples/01..04/    end-to-end demos (wrap_scribe, drift, bundle, bench)
├── tests/              doctest unit + integration (1388 cases / 35086 asserts)
│   └── benchmarks/asclepius_med/  L2-Medical conformance scaffold (4 cases / 128 asserts)
├── docs/               markdown sources for ARCHITECTURE / SPEC / THREAT_MODEL
├── proto/              gRPC proto schemas
└── site/               product website (static HTML+CSS+vanilla JS)
```

## C++ runtime conventions

* **C++20** with strict warnings (`-Wall -Wextra -Wpedantic -Wshadow
  -Wconversion -Wsign-conversion …`); ASan + UBSan in Debug.
* **Result<T, Error>** for fallible APIs (no exceptions in the hot path).
* **Strong typedefs** for IDs (`ActorId`, `PatientId`, `EncounterId`,
  `ModelId`, `TenantId`) — never raw strings.
* **Pimpl** for stable ABI on `Runtime`, `Ledger`, `KeyStore`, `Inference`.
* **No copies** for handle types; move-only.
* **BLAKE2b-256** hashing (libsodium `crypto_generichash`).
* **Ed25519** signing (libsodium `crypto_sign_*`).
* **SQLite WAL** for the single-node ledger backend.

## Site conventions

The website at `site/` is the product face. Single-file static pages, no
build step beyond `python3 build_docs.py` for re-rendering markdown.

* **Type stack:** Instrument Serif (display) + Newsreader (prose) +
  JetBrains Mono (mono). No Inter, no Roboto.
* **Palette:** purple-tinted obsidian (`#0A0710` bg, `#B57BFF` primary)
  with surgical green (`#34D399`). Full light theme via
  `[data-theme="light"]` overrides.
* **Numbering:** sections use `§01`, `§02` etc. and named `kbd` eyebrows.
* **Voice:** terse, opinionated, italic-accented headlines ("The *Linux
  kernel* for clinical AI."). Don't slip into marketing-speak.
* **Cryptographic claim integrity:** every speed/throughput claim on
  `benchmarks.html` is backed by `assets/bench.json`, produced by
  `examples/04_bench/main.cpp`. If you change the claim, regenerate the
  JSON.

### Site files (39 HTML pages)

**Landing + product**
| File | Purpose |
|---|---|
| `index.html` | manifesto, crisis, substrate, anatomy, quickstart, bench, compare, FAQ, why-now, open |
| `demo.html` | interactive Web-Crypto-signed Merkle ledger playground |
| `404.html` | "the chain broke at this seq" |

**Documentation**
| File | Purpose |
|---|---|
| `docs.html` | hub linking architecture/spec/threat-model |
| `architecture.html`, `spec.html`, `threat-model.html` | rendered from `../docs/*.md` via `build_docs.py` |
| `glossary.html` | every term defined once, deep-linkable |
| `api.html` | API reference — C++/Python/Rust/gRPC |
| `cheatsheet.html` | quick-reference card |
| `deploy.html` | production patterns (HSM, sidecar, k8s, systemd) |
| `learn.html` | guided tour |
| `playbooks.html` | operator runbooks for common scenarios |
| `integrations.html` | EHR / model / dashboard integration matrix |
| `decisions.html` | architecture decision records (ADRs) |
| `use-cases.html` | scenario gallery |

**Release / status / proof**
| File | Purpose |
|---|---|
| `benchmarks.html` | real performance numbers, live-bound to `assets/bench.json` via `js/bench.js` |
| `changelog.html` | release history with h-entry microformats |
| `roadmap.html` | quarters of milestones + L1/L2/L3 conformance ladder |
| `conformance.html` | concrete L1/L2/L3 test list with pass/fail |
| `status.html` | build / verifier / chain dashboard |
| `crypto.html` | in-tab hash/sign/verify educational tools |
| `labs.html` | pilot sites + research collaborators |
| `compare.html` | vs MLflow / Datadog / Epic / in-house |
| `explorer.html`, `sandbox.html`, `verify-bundle.html` | extended interactive surfaces |
| `sitegraph.html` | site-link visualizer (consumes `search-index.json`) |

**Community + policy**
| File | Purpose |
|---|---|
| `contributing.html` | PR / RFC / DCO flow |
| `governance.html` | three-tier change model, roles, votes |
| `principles.html` | seven design tenets — the *why* (pairs with governance) |
| `security.html` | vuln disclosure, supported versions, signing keys |
| `press.html` | wordmark, palette, fact sheet |
| `badges.html` | embeddable SVG status badges |
| `accessibility.html` | WCAG 2.2 AA conformance statement |
| `cookies.html` | "the list is shorter than the policy" — zero |
| `licenses.html` | third-party inventory |
| `colophon.html` | how the site itself is built (this set-piece) |
| `sitemap.html` | human-readable site index |

### Site JS (11 modules)

| File | Role |
|---|---|
| `js/main.js` | theme toggle, embedded landing-page demo, scroll spy, FAQ accordion |
| `js/extras.js` | reveal-on-scroll, copy buttons, status frost, anchor flash, typed terminal, mobile burger, SW registration |
| `js/palette.js` | ⌘K command palette with hand-curated index |
| `js/help.js` | `?` help modal · g-then-key navigation chords |
| `js/demo.js` | playground runtime (Web Crypto Ed25519 + SHA-256, Merkle ledger, drift PSI) |
| `js/floating.js` | bottom-right floating actions (scroll, share, page-attestation pill) |
| `js/attest-site.js` | per-page SHA-256 verification against `/asset-manifest.json` |
| `js/themes.js` | multi-preset palette picker beyond basic dark/light |
| `js/bench.js` | live-binds `/benchmarks` numbers from `assets/bench.json` |
| `js/source.js` | injects "edit this page on GitHub" link into every footer |
| `js/badges.js` | click-to-copy snippet handler on `/badges.html` |
| `sw.js` | service worker — SWR for HTML, cache-first for assets, fonts |

### Site infra

* `manifest.json` — PWA install metadata + shortcuts (Playground, Docs, Status, Crypto, Changelog).
* `feed.xml` / `feed.json` / `atom.xml` — RSS 2.0 + JSON Feed 1.1 + Atom 1.0 (RFC 4287).
* `opensearch.xml` — browser search-engine descriptor.
* `robots.txt` + `sitemap.xml` + `sitemap.html` — crawler + human site indexes.
* `humans.txt` — thanks list + crypto fingerprints.
* `up.txt` — 3-byte healthcheck.
* `status.json` — programmatic semantic liveness (auto-regenerated by `regen_status.py`).
* `_headers` + `_redirects` — Netlify/Cloudflare/Vercel HTTP-level config.
* `.editorconfig` — cross-editor style.
* `asset-manifest.json` — SHA-256 of every served file (regenerated by `regen_manifest.py`).
* `search-index.json` — page index for sitegraph + palette (regenerated by `regen_search.py`).
* `.well-known/security.txt` (RFC 9116) + `.well-known/dnt-policy.txt` (EFF).
* `assets/og-card.{svg,png}` — 1200×630 share card.
* `assets/favicon.svg` + `favicon-mono.svg` + `apple-touch-icon.png`.
* `assets/pgp.asc` — public signing key block.
* `assets/badges/{version,license,conformance,ledger,build,runtime}.svg` — six embeddable status badges.

### Build helpers (run by `make`)

* `check_html.py` — HTML structure, broken-link, duplicate-id, heading-hierarchy validator.
* `check_integrity.py` — cross-reference audit across PRECACHE / asset-manifest / sitemap / palette index.
* `regen_manifest.py` — recomputes SHA-256 of every served file → `asset-manifest.json`.
* `regen_search.py` — extracts title + description per page → `search-index.json`.
* `regen_status.py` — scrapes current release from changelog → `status.json`.
* `build_docs.py` — renders `docs/*.md` → `{architecture,spec,threat-model}.html`.

### Build pipeline

```sh
make help            # menu
make validate        # check_html.py — broken links, dup ids, heading hierarchy
make integrity       # check_integrity.py — cross-reference audit
make manifest        # regen asset-manifest.json (SHA-256 of every served file)
make search-index    # regen search-index.json (one record per page)
make status          # regen status.json (semantic liveness + counts)
make docs            # render docs/*.md → architecture/spec/threat-model.html
make all             # search-index + status + manifest + validate + integrity
make serve           # local http server on :8765
```

### Linter / iteration etiquette

The project goes through Ralph-loop iterations. A linter regenerates
canonical metadata (canonical URLs, og tags, manifest links). When the
linter modifies a file:

1. **Don't revert** linter changes — they are intentional.
2. Re-read the file before editing if you've been notified of a change.
3. Prefer additive edits (append CSS, add JS files) over rewrites.
4. After edits, run `make all` so manifest + search-index stay in sync.

## Build commands

```sh
# build the C++ runtime
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build -j

# run tests
build/tests/asclepius_tests       # 1388 cases / 35086 asserts
build/tests/benchmarks/asclepius_med/asclepius_bench_asclepius_med  # L2-Medical · 4/128
ctest --test-dir build --output-on-failure

# run the bench harness (regenerates site/assets/bench.json)
./build/examples/04_bench/example_bench --n 30000 \
    > site/assets/bench.json

# render docs
cd site && python3 build_docs.py

# serve the site locally
cd site && python3 -m http.server 8765
# → http://localhost:8765
```

## When in doubt

* **Substrate, not app.** If a feature would make Asclepius into "an AI
  scribe" or "an MLOps platform," push back; we are the layer underneath
  those.
* **Auditability over convenience.** Anything that hides bytes is a
  smell. Cryptographic provenance is the product.
* **One headline per page.** Italic accent = the rhetorical hook. Use it
  once per heading, not throughout.
* **No emojis in committed text** unless explicitly asked.
