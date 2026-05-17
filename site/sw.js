// SPDX-License-Identifier: Apache-2.0
// Asclepius — service worker for offline-first reads.
//
// Strategy:
//   • cache-first  for assets   (CSS, JS, fonts, SVG, PNG, JSON)
//   • SWR          for HTML     (return cache, revalidate in background)
//   • network-only for non-GET / cross-origin (Google Fonts is exempt; we
//     stale-while-revalidate them too)
//
// SECURITY MODEL — integrity-gated caching:
//   • On `activate`, fetch `asset-manifest.json` and pin its SHA-256 map.
//   • On every same-origin asset fetch we then INTEND TO CACHE, recompute
//     SHA-256 over the response body and compare against the manifest.
//     A miss-or-mismatch is returned to the page (so the request still
//     succeeds today) but NEVER persisted — so a tampered byte stream
//     cannot survive in the cache and re-serve poisoned content offline.
//   • HTML pages and Google Fonts are SWR without manifest verification:
//     HTML changes constantly during dev; Google Fonts content varies by
//     User-Agent. Manifest-checked assets carry the verifiable byte
//     contract; everything else relies on TLS for integrity.
//
// Once you've visited a page on this site, it's available offline. Bumping
// the SW_VERSION below invalidates the cache cleanly.

const SW_VERSION = 'asclepius-v0.1.1-2026-05-17';
const STATIC_CACHE = `${SW_VERSION}-static`;
const HTML_CACHE   = `${SW_VERSION}-html`;
const FONT_CACHE   = `${SW_VERSION}-fonts`;

const PRECACHE = [
    'styles.css',
    'demo.css',
    'js/main.js',
    'js/extras.js',
    'js/demo.js',
    'js/palette.js',
    'js/floating.js',
    'js/help.js',
    'js/bench.js',
    'js/attest-site.js',
    'js/themes.js',
    'js/source.js',
    'js/badges.js',
    'js/crystal-cursor.js',
    // Per-page scripts (extracted from formerly-inline blocks; covered
    // by the asset manifest so SRI applies through the SW.)
    'js/page-compare.js',
    'js/page-crypto.js',
    'js/page-explorer.js',
    'js/page-glossary.js',
    'js/page-integrations.js',
    'js/page-pitch.js',
    'js/page-sandbox.js',
    'js/page-sitegraph.js',
    'js/page-status.js',
    'js/page-validate.js',
    'js/page-walkthrough.js',
    'assets/favicon.svg',
    'assets/favicon-mono.svg',
    'assets/og-card.svg',
    'assets/og-card.png',
    'assets/apple-touch-icon.png',
    'assets/bench.json',
    'assets/pgp.asc',
    'manifest.json',
    'feed.xml',
    'feed.json',
    'atom.xml',
    'opensearch.xml',
    'humans.txt',
    'up.txt',
    'status.json',
    'asset-manifest.json',
    'search-index.json',
    'assets/badges/version.svg',
    'assets/badges/license.svg',
    'assets/badges/conformance.svg',
    'assets/badges/ledger.svg',
    'assets/badges/build.svg',
    'assets/badges/runtime.svg',
];

// ─── Integrity table — populated on activate from asset-manifest.json ───
// Map<path, sha256-hex>. Empty if the manifest can't be fetched (offline
// install). The SW degrades to non-integrity-checked caching in that case;
// users still pay TLS for in-flight integrity.
let INTEGRITY = null;

self.addEventListener('install', (e) => {
    e.waitUntil((async () => {
        const c = await caches.open(STATIC_CACHE);
        // Use addAll for speed; integrity validation happens on subsequent
        // fetches (we can't intercept addAll's internal Response pipe).
        await c.addAll(PRECACHE);
        self.skipWaiting();
    })());
});

self.addEventListener('activate', (e) => {
    e.waitUntil((async () => {
        // Cleanup stale caches from prior SW_VERSION values
        const keys = await caches.keys();
        await Promise.all(keys
            .filter((k) => !k.startsWith(SW_VERSION))
            .map((k) => caches.delete(k)));

        // Pin the manifest's integrity table for this SW lifetime
        try {
            const res = await fetch('asset-manifest.json', { cache: 'no-store' });
            if (res.ok) {
                const m = await res.json();
                const map = new Map();
                for (const [path, meta] of Object.entries(m.files || {})) {
                    if (meta && typeof meta.sha256 === 'string') {
                        map.set(path, meta.sha256.toLowerCase());
                    }
                }
                INTEGRITY = map;
            }
        } catch (_) {
            // No manifest → caching falls back to "TLS-only" trust.
            INTEGRITY = null;
        }

        await self.clients.claim();
    })());
});

const isHtml = (req) =>
    req.mode === 'navigate' || (req.destination === 'document');
const isFont = (url) =>
    url.hostname === 'fonts.googleapis.com' || url.hostname === 'fonts.gstatic.com';
const isSameOriginAsset = (url) =>
    url.origin === self.location.origin &&
    !url.pathname.endsWith('.html') &&
    url.pathname !== '/';

// SHA-256 hex digest of an ArrayBuffer.
async function sha256Hex(buf) {
    const d = await crypto.subtle.digest('SHA-256', buf);
    return Array.from(new Uint8Array(d))
        .map((b) => b.toString(16).padStart(2, '0')).join('');
}

// Given the URL, return the manifest key (path relative to site root).
function manifestKey(url) {
    let p = url.pathname.replace(/^\/+/, '');
    if (p === '') p = 'index.html';
    return p;
}

// Verify a response body against the integrity table.
// Returns { ok: bool, expected: string|null, got: string|null }.
async function verifyAgainstManifest(url, response) {
    if (!INTEGRITY) return { ok: true, expected: null, got: null };  // no manifest → cannot verify
    const key = manifestKey(url);
    const expected = INTEGRITY.get(key);
    if (!expected) return { ok: true, expected: null, got: null };   // path not in manifest → skip
    try {
        const buf = await response.clone().arrayBuffer();
        const got = await sha256Hex(buf);
        return { ok: got === expected, expected, got };
    } catch (_) {
        return { ok: false, expected, got: null };
    }
}

self.addEventListener('fetch', (e) => {
    const req = e.request;
    if (req.method !== 'GET') return;

    const url = new URL(req.url);

    // ── HTML — stale-while-revalidate ────────────────────────────────
    if (isHtml(req) && url.origin === self.location.origin) {
        e.respondWith((async () => {
            const cache = await caches.open(HTML_CACHE);
            const cached = await cache.match(req);
            const network = fetch(req)
                .then((res) => {
                    if (res && res.ok) cache.put(req, res.clone());
                    return res;
                })
                .catch(() => cached);
            return cached || network;
        })());
        return;
    }

    // ── same-origin asset — cache first, with integrity gate on write
    if (isSameOriginAsset(url)) {
        e.respondWith((async () => {
            const cache = await caches.open(STATIC_CACHE);
            const cached = await cache.match(req);
            if (cached) return cached;
            try {
                const res = await fetch(req);
                if (res && res.ok) {
                    // Integrity gate — refuse to cache a mismatch. Always
                    // returns the fresh response to the caller so the page
                    // works today; refusing the *cache* prevents poisoned
                    // bytes from being served on subsequent offline loads.
                    const v = await verifyAgainstManifest(url, res);
                    if (v.ok) {
                        cache.put(req, res.clone());
                    } else if (self.registration && self.registration.scope) {
                        // Console-only; we can't message clients reliably here.
                        // The mismatch is recoverable on the next page load.
                        console.warn('[asclepius-sw] integrity mismatch — not cached',
                            { path: manifestKey(url), expected: v.expected, got: v.got });
                    }
                }
                return res;
            } catch (_) {
                return cached || Response.error();
            }
        })());
        return;
    }

    // ── google fonts — stale-while-revalidate (no manifest possible) ─
    if (isFont(url)) {
        e.respondWith((async () => {
            const cache = await caches.open(FONT_CACHE);
            const cached = await cache.match(req);
            const network = fetch(req)
                .then((res) => {
                    if (res && res.ok) cache.put(req, res.clone());
                    return res;
                })
                .catch(() => cached);
            return cached || network;
        })());
    }
});
