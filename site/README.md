# Asclepius — site

The product website. **44 hand-rolled HTML pages**, **11 vanilla-JS modules**,
**6 audit gates**, **0 frameworks**. Single source of truth at every layer:
edit anything, run `make all`, get a single yes/no answer.

## Quick start

```sh
cd site
make help            # menu of all targets
make serve           # local http server on :8765
make all             # validate + regen + audit, end to end
```

The Google Fonts stylesheet is the only over-the-wire dependency at runtime.
Block it and the site falls through to the system serif/sans/mono stack and
reads cleanly.

## What's in here

```
site/
  ─── pages (44) ──────────────────────────────────────────────────────────
  index.html              long-form landing (manifesto → bench → open)
  demo.html               in-browser playground · real Web Crypto + Merkle ledger
  docs.html               docs hub
  architecture.html       generated from ../docs/ARCHITECTURE.md
  spec.html               generated from ../docs/SPEC.md
  threat-model.html       generated from ../docs/THREAT_MODEL.md
  benchmarks.html         live-bound to assets/bench.json
  changelog.html          h-entry microformat releases
  roadmap.html            quarters + L1/L2/L3 ladder
  conformance.html        the public test battery
  status.html             build / verifier / chain dashboard
  validate.html           in-page validator UI
  crypto.html             hash / sign / verify in-tab
  sandbox.html            extended playground
  explorer.html           ledger row explorer
  verify-bundle.html      regulator-side bundle check
  compare.html            two-bundle differ
  api.html                API reference (C++ / Python / Rust / gRPC)
  cheatsheet.html         quick-reference card
  deploy.html             production patterns
  playbooks.html          operator runbooks
  integrations.html       EHR / model / dashboard matrix
  decisions.html          architecture decision records
  use-cases.html          scenario gallery
  glossary.html           every term defined once
  learn.html              guided tour
  research.html           open research questions
  observability.html      telemetry / dashboards
  regulator.html          regulator-facing surface
  economics.html          economic case
  migrations.html         upgrade flow
  labs.html               pilot deployments
  contributing.html       PR / RFC / DCO
  governance.html         three-tier change model
  principles.html         seven design tenets
  security.html           vuln disclosure (RFC 9116)
  press.html              wordmark, palette, fact sheet
  badges.html             6 embeddable SVG status badges
  accessibility.html      WCAG 2.2 AA statement
  cookies.html            "shorter than the policy" — zero
  licenses.html           third-party license inventory
  colophon.html           how the site itself is built
  sitemap.html            human site index
  sitegraph.html          link-graph visualizer
  404.html                themed not-found

  ─── stylesheets ─────────────────────────────────────────────────────────
  styles.css              design system, dark + light themes
  demo.css                playground-only styles

  ─── js modules (11) ─────────────────────────────────────────────────────
  js/main.js              theme toggle, scroll spy, FAQ accordion
  js/extras.js            reveal-on-scroll, copy buttons, frost, mobile burger, SW reg
  js/palette.js           ⌘K command palette, hand-curated index
  js/help.js              ? help modal · g-then-key navigation chords
  js/demo.js              playground runtime (Web Crypto + Merkle ledger)
  js/floating.js          bottom-right cluster (scroll, share, attestation)
  js/attest-site.js       per-page SHA-256 verify against /asset-manifest.json
  js/themes.js            multi-preset palette picker
  js/bench.js             live-binds /benchmarks numbers from bench.json
  js/source.js            "edit on GitHub" injector
  js/badges.js            click-to-copy snippets on /badges
  sw.js                   service worker — SWR HTML, cache-first assets

  ─── infrastructure ─────────────────────────────────────────────────────
  manifest.json           PWA install metadata + 5 app shortcuts
  feed.xml                RSS 2.0
  feed.json               JSON Feed 1.1
  atom.xml                Atom 1.0 (RFC 4287)
  opensearch.xml          browser search-engine descriptor
  robots.txt              index-friendly
  sitemap.xml             every URL we publish
  humans.txt              thanks list + crypto fingerprints
  up.txt                  3-byte healthcheck (returns "OK\n")
  status.json             programmatic semantic liveness
  asset-manifest.json     SHA-256 of every served file
  search-index.json       title + description per page
  _headers                Netlify / Cloudflare HTTP headers
  _redirects              19 friendly aliases
  .editorconfig           cross-editor consistency
  .gitignore              transient files we never commit
  .well-known/security.txt    RFC 9116
  .well-known/dnt-policy.txt  EFF Do-Not-Track Compliance Policy

  ─── assets ──────────────────────────────────────────────────────────────
  assets/og-card.{svg,png}        1200×630 share card
  assets/favicon.svg              brand mark
  assets/favicon-mono.svg         Safari mask icon
  assets/apple-touch-icon.png     iOS home-screen
  assets/pgp.asc                  signing key block
  assets/bench.json               C++ runtime benchmark output
  assets/badges/{6 svgs}          embeddable status badges

  ─── build helpers ──────────────────────────────────────────────────────
  Makefile                coordinates everything
  check_html.py           HTML structure / link / heading / id validator
  check_integrity.py      cross-reference audit (sw / manifest / sitemap / palette)
  check_orphans.py        BFS reachability + JS-module-loaded audit
  check_content.py        page-depth audit (word counts)
  regen_manifest.py       re-SHA every served file → asset-manifest.json
  regen_search.py         extract title + desc → search-index.json
  regen_status.py         scrape current release → status.json
  build_docs.py           render docs/*.md → architecture/spec/threat-model.html
```

## Audit gates (`make all`)

The site has **6 cross-cutting audits**, each producing a single yes/no answer:

| Gate                      | Tool                  | What it catches                                |
|---------------------------|-----------------------|------------------------------------------------|
| HTML structure            | `check_html.py`       | broken hrefs, dup ids, h1→h3 jumps, missing alt |
| Cross-reference integrity | `check_integrity.py`  | sw / manifest / sitemap / palette drift         |
| File-orphan reachability  | `check_orphans.py`    | files on disk no link reaches                  |
| JS-module reachability    | `check_orphans.py`    | js modules no page loads                       |
| Content depth             | `check_content.py`    | pages with chrome but no body                  |
| SHA-256 self-attestation  | `regen_manifest.py`   | the bytes we serve = the bytes we publish      |

Every gate is hand-rolled Python; together they're ~600 LoC. They run on
every `make all` and there is no other CI; if `make all` is green, the site
is shippable.

## Aesthetic

Editorial cryptographic dark — Stripe Press meets a kernel changelog meets a
medical journal. Three typefaces:

- **Instrument Serif** — display, italic for emphasis
- **Newsreader** — long-form prose
- **JetBrains Mono** — technical data, code, labels

Palette tokens live at the top of `styles.css`. The portfolio's canonical
purple `#6B21A8` and accent green `#059669` are lifted for legibility on
near-black; a parallel parchment-light theme inverts surface and ink. Theme
choice persists in `localStorage` under `asclepius-theme`; multi-preset
picker via `js/themes.js`.

## Self-attestation

Every byte the server returns has its SHA-256 published in
[`/asset-manifest.json`](asset-manifest.json). The pill at the bottom-right
of every page (◇, injected by `js/attest-site.js`) recomputes the hash of the
rendered `<main>` and shows you whether it matches the manifest entry.

This is the same posture as the runtime, applied to the marketing site.
There is no coherent way to ask hospitals to trust an audit substrate while
shipping their executives a marketing site that runs Google Tag Manager.

## Privacy

- No cookies set on this origin. Two `localStorage` keys (`asclepius-theme`,
  `asclepius-demo-*`) — both stay on your device.
- No analytics SDKs, no A/B testing, no marketing pixels, no
  fingerprinting libraries. The site loads zero third-party JavaScript.
- Only over-the-wire dependency at runtime: the Google Fonts stylesheet.
  Documented at [`/cookies.html`](cookies.html); EFF DNT compliance at
  [`/.well-known/dnt-policy.txt`](.well-known/dnt-policy.txt).

## Deploy

Static. Drop the directory onto any host (GitHub Pages, Cloudflare Pages,
Netlify, Vercel, S3 + CloudFront, plain nginx). On Netlify / Cloudflare /
Vercel the `_headers` file applies real HTTP-level security headers (HSTS,
COOP/CORP, CSP, Permissions-Policy); on others the same defenses live in
the per-page `<meta http-equiv>` tags.

```sh
make all       # one command before deploy
```

If `make all` is green, ship it.
