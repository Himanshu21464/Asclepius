#!/usr/bin/env python3
"""Orphan-file audit.

BFS from index.html through every internal href in every reachable page.
Anything on disk that this BFS can't reach is an orphan: either delete it,
or add it to ALLOWLIST below if it's intentionally hidden (e.g., reachable
only via the service worker, palette index, or feed reader).

Exits 0 only when the orphan set is empty.
"""
from __future__ import annotations
import os
import re
import sys
from urllib.parse import urldefrag, urlparse

HERE = os.path.dirname(os.path.abspath(__file__))

# Files reachable through paths the BFS doesn't follow:
#  - service worker precache + Web App manifest
#  - feed readers (RSS / Atom / JSON Feed)
#  - sitemap consumers
#  - palette / search index consumers
#  - HTTP-only configs (Netlify _headers / _redirects)
#  - well-known URIs (RFC 5785) — not generally page-linked
#  - infrastructure reachable only by direct request (status.json, up.txt)
ALLOWLIST = {
    'sw.js',
    'manifest.json',
    'asset-manifest.json',
    'search-index.json',
    'sitemap.xml',
    'robots.txt',
    'humans.txt',
    'up.txt',
    'status.json',
    'feed.xml',
    'feed.json',
    'atom.xml',
    'opensearch.xml',
    '_headers',
    '_redirects',
    '.editorconfig',
    '.well-known/security.txt',
    '.well-known/dnt-policy.txt',
    'check_html.py',
    'check_integrity.py',
    'check_orphans.py',
    'check_content.py',
    'regen_manifest.py',
    'regen_search.py',
    'regen_status.py',
    'build_docs.py',
    'build_api.py',
    'build_search.py',
    'tests/run.mjs',
    'Makefile',
    'README.md',
    '.gitignore',
    '404.html',          # served only on 404 status, never body-linked
    # Per-asset entries reachable via OG cards, mask icons, badge embeds.
    'assets/og-card.svg',
    'assets/og-card.png',
    'assets/favicon.svg',
    'assets/favicon-mono.svg',
    'assets/apple-touch-icon.png',
    'assets/pgp.asc',
    'assets/bench.json',
    'assets/badges/version.svg',
    'assets/badges/license.svg',
    'assets/badges/conformance.svg',
    'assets/badges/ledger.svg',
    'assets/badges/build.svg',
    'assets/badges/runtime.svg',
}

LINK_PATTERNS = [
    re.compile(r'href="([^"]+)"',          re.I),
    re.compile(r"href='([^']+)'",          re.I),
    re.compile(r'src="([^"]+)"',           re.I),
    re.compile(r"src='([^']+)'",           re.I),
    re.compile(r'data-copy="([^"]+)"',     re.I),
    re.compile(r"<a[^>]+href=([^\s>]+)",   re.I),
]


def is_external(url: str) -> bool:
    return bool(urlparse(url).scheme) and urlparse(url).scheme not in ('', 'file')


def should_walk(rel: str) -> bool:
    return rel.endswith('.html')


def collect_links(path: str) -> set[str]:
    out: set[str] = set()
    try:
        s = open(path, encoding='utf-8').read()
    except Exception:
        return out
    for pat in LINK_PATTERNS:
        for m in pat.finditer(s):
            url = m.group(1)
            if is_external(url) or url.startswith('mailto:') or url.startswith('javascript:') or url.startswith('data:'):
                continue
            url = urldefrag(url)[0]
            if not url:
                continue
            url = url.split('?', 1)[0].lstrip('./')
            out.add(url)
    return out


def collect_disk(root: str) -> set[str]:
    """Every file on disk, posix-relative."""
    found: set[str] = set()
    for r, dirs, files in os.walk(root):
        # walk into .well-known but skip hidden/git/python caches
        dirs[:] = [d for d in dirs if d == '.well-known' or not d.startswith('.')]
        dirs[:] = [d for d in dirs if d != '__pycache__']
        for name in files:
            full = os.path.join(r, name)
            rel = os.path.relpath(full, root).replace(os.sep, '/')
            found.add(rel)
    return found


def main() -> int:
    seed = 'index.html'
    seen: set[str] = {seed}
    queue: list[str] = [seed]
    while queue:
        cur = queue.pop()
        full = os.path.join(HERE, cur)
        if not os.path.exists(full):
            continue
        if should_walk(cur):
            for link in collect_links(full):
                # absolute-from-root
                if link.startswith('/'):
                    link = link.lstrip('/')
                if link not in seen and os.path.exists(os.path.join(HERE, link)):
                    seen.add(link)
                    queue.append(link)

    on_disk = collect_disk(HERE)
    orphans = sorted(on_disk - seen - ALLOWLIST)

    # Code-orphan check: every js/*.js must be loaded by at least one HTML page.
    # The reachability set already includes <script src> targets, so a JS module
    # not in `seen` (and not allowlisted) IS already in `orphans` — but we
    # surface it separately because the fix is different (wire into a page,
    # not link from a footer).
    js_modules = sorted(p for p in on_disk if p.startswith('js/') and p.endswith('.js'))
    js_dead = [m for m in js_modules if m not in seen and m not in ALLOWLIST]

    print(f'asclepius orphan audit\n')
    print(f'  visited:     {len(seen)} files')
    print(f'  on disk:     {len(on_disk)} files')
    print(f'  allowlisted: {len(ALLOWLIST)} files')
    print(f'  js modules:  {len(js_modules)} (loaded: {len(js_modules) - len(js_dead)})')
    print(f'  orphans:     {len(orphans)}\n')

    issues = 0
    if js_dead:
        issues += len(js_dead)
        print('\033[31m✗ js modules not loaded by any page:\033[0m')
        for m in js_dead:
            print(f'   {m}')
        print()

    file_orphans = [o for o in orphans if o not in js_dead]
    if file_orphans:
        issues += len(file_orphans)
        print('\033[31m✗ orphans found:\033[0m')
        for o in file_orphans:
            print(f'   {o}')

    if issues:
        return 1

    print('\033[32m✓ no orphans · every js module loaded · BFS complete\033[0m')
    return 0


if __name__ == '__main__':
    sys.exit(main())
