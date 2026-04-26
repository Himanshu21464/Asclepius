// SPDX-License-Identifier: Apache-2.0
// Asclepius — service worker for offline-first reads.
//
// Strategy:
//   • cache-first  for assets   (CSS, JS, fonts, SVG, PNG, JSON)
//   • SWR          for HTML     (return cache, revalidate in background)
//   • network-only for non-GET / cross-origin (Google Fonts is exempt; we
//     stale-while-revalidate them too)
//
// Once you've visited a page on this site, it's available offline. Bumping
// the SW_VERSION below invalidates the cache cleanly.

const SW_VERSION = 'asclepius-v0.1.0-2026-04-28';
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

self.addEventListener('install', (e) => {
    e.waitUntil((async () => {
        const c = await caches.open(STATIC_CACHE);
        await c.addAll(PRECACHE);
        self.skipWaiting();
    })());
});

self.addEventListener('activate', (e) => {
    e.waitUntil((async () => {
        const keys = await caches.keys();
        await Promise.all(keys
            .filter((k) => !k.startsWith(SW_VERSION))
            .map((k) => caches.delete(k)));
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

    // ── same-origin asset — cache first ──────────────────────────────
    if (isSameOriginAsset(url)) {
        e.respondWith((async () => {
            const cache = await caches.open(STATIC_CACHE);
            const cached = await cache.match(req);
            if (cached) return cached;
            try {
                const res = await fetch(req);
                if (res && res.ok) cache.put(req, res.clone());
                return res;
            } catch (_) {
                return cached || Response.error();
            }
        })());
        return;
    }

    // ── google fonts — stale-while-revalidate ───────────────────────
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
