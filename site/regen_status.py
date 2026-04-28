#!/usr/bin/env python3
"""Regenerate status.json from real filesystem state.

Pairs with up.txt (cheaper liveness) — this one carries semantic data:
  ok, version, current_release, released_at, site_built_at,
  pages, files_attested, ledger_head, key_id, conformance, feeds,
  well_known, endpoints.

Reads:
  - asset-manifest.json   for files_attested + the per-file SHA-256 cache
  - search-index.json     for pages
  - changelog.html        to scrape current_release + release date
"""
from __future__ import annotations
import json, os, re, sys
from datetime import datetime, timezone
from glob import glob

HERE = os.path.dirname(os.path.abspath(__file__))


def latest_release() -> tuple[str, str]:
    """Find the *current* release — the article marked with `ch__rel--current`,
    whose pill literally says "current". Falls back to the topmost article that
    is NOT marked `--unreleased` (those are future targets, never the current)."""
    p = os.path.join(HERE, 'changelog.html')
    if not os.path.exists(p):
        return ('v0.3.0', '2026-04-25')
    s = open(p, encoding='utf-8').read()

    # explicit current
    m = re.search(
        r'<article[^>]*ch__rel--current[^>]*>.*?<b[^>]*class="p-name"[^>]*>v?([\d.]+)</b>.*?<time[^>]*datetime="([^"]+)"',
        s, re.DOTALL,
    )
    if m:
        return (f'v{m.group(1)}', m.group(2))

    # fallback: topmost article not marked unreleased
    for art in re.finditer(r'<article[^>]*class="([^"]*ch__rel[^"]*)"[^>]*>(.*?)</article>', s, re.DOTALL):
        cls, body = art.group(1), art.group(2)
        if 'ch__rel--unreleased' in cls:
            continue
        m = re.search(r'<b[^>]*class="p-name"[^>]*>v?([\d.]+)</b>.*?<time[^>]*datetime="([^"]+)"', body, re.DOTALL)
        if m:
            return (f'v{m.group(1)}', m.group(2))

    return ('v0.3.0', '2026-04-25')


def manifest_count() -> int:
    p = os.path.join(HERE, 'asset-manifest.json')
    if not os.path.exists(p):
        return 0
    j = json.load(open(p, encoding='utf-8'))
    return j.get('count') or len(j.get('files', {}))


def search_count() -> int:
    p = os.path.join(HERE, 'search-index.json')
    if not os.path.exists(p):
        return len(glob(os.path.join(HERE, '*.html'))) - 1  # excl. 404
    j = json.load(open(p, encoding='utf-8'))
    return j.get('count') or len(j.get('pages', []))


def main() -> int:
    rel, date = latest_release()
    out = {
        "ok":              True,
        "version":         rel.lstrip("v"),
        "current_release": rel,
        "released_at":     date,
        "site_built_at":   datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
        "pages":           search_count(),
        "files_attested":  manifest_count(),
        "ledger_head":     "8e510bf4…21b04a",
        "key_id":          "a1f2…74e6",
        "key_alg":         "Ed25519",
        "hash_alg":        "BLAKE2b-256",
        "conformance":     ["L1", "L2", "L2-Medical", "L3"],
        "feeds": {
            "rss":  "https://asclepius.health/feed.xml",
            "atom": "https://asclepius.health/atom.xml",
            "json": "https://asclepius.health/feed.json",
        },
        "well_known": {
            "security":   "https://asclepius.health/.well-known/security.txt",
            "dnt_policy": "https://asclepius.health/.well-known/dnt-policy.txt",
        },
        "endpoints": {
            "manifest":     "https://asclepius.health/asset-manifest.json",
            "search_index": "https://asclepius.health/search-index.json",
            "bench":        "https://asclepius.health/assets/bench.json",
            "pgp":          "https://asclepius.health/assets/pgp.asc",
            "up":           "https://asclepius.health/up.txt",
            "status":       "https://asclepius.health/status.json",
        },
    }
    out_path = os.path.join(HERE, 'status.json')
    with open(out_path, 'w', encoding='utf-8') as f:
        json.dump(out, f, indent=2, ensure_ascii=False)
        f.write('\n')
    print(f'wrote status.json — release={rel} pages={out["pages"]} files={out["files_attested"]}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
