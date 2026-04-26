#!/usr/bin/env python3
"""Asclepius site integrity audit.

Cross-references the four authoritative file lists and reports drift:

  1. PRECACHE in sw.js              — every entry must exist on disk
  2. asset-manifest.json files{}    — every entry must exist on disk
  3. sitemap.xml <loc> URLs         — every URL must map to a file we serve
  4. palette.js INDEX url fields    — every URL must exist (if hash-less)

Exits 0 only when nothing is missing on any of those axes.
"""
from __future__ import annotations
import json
import os
import re
import sys
from urllib.parse import urlparse

HERE = os.path.dirname(os.path.abspath(__file__))
SITE_BASE = 'https://asclepius.health'


def colour(c, msg):
    return f'\033[{c}m{msg}\033[0m'


def exists(rel: str) -> bool:
    return os.path.exists(os.path.join(HERE, rel))


# ── 1. PRECACHE drift ────────────────────────────────────────────────────────
def precache_entries() -> list[str]:
    sw = open(os.path.join(HERE, 'sw.js'), encoding='utf-8').read()
    m = re.search(r'const PRECACHE = \[(.*?)\];', sw, re.DOTALL)
    if not m:
        return []
    block = m.group(1)
    return [s.strip().strip("'\",") for s in re.findall(r"'([^']+)'", block)]


# ── 2. manifest drift ───────────────────────────────────────────────────────
def manifest_files() -> list[str]:
    j = json.load(open(os.path.join(HERE, 'asset-manifest.json'), encoding='utf-8'))
    return list(j.get('files', {}).keys())


# ── 3. sitemap URLs ─────────────────────────────────────────────────────────
def sitemap_paths() -> list[str]:
    sm = open(os.path.join(HERE, 'sitemap.xml'), encoding='utf-8').read()
    paths = []
    for m in re.finditer(r'<loc>([^<]+)</loc>', sm):
        u = m.group(1)
        if u.startswith(SITE_BASE):
            p = u[len(SITE_BASE):] or '/'
            # root → index.html
            if p == '/' or p == '':
                p = 'index.html'
            else:
                p = p.lstrip('/')
            paths.append(p)
    return paths


# ── 4. palette index ────────────────────────────────────────────────────────
def palette_urls() -> list[str]:
    pj = open(os.path.join(HERE, 'js/palette.js'), encoding='utf-8').read()
    out = []
    for m in re.finditer(r"url:\s*'([^']+)'", pj):
        u = m.group(1).split('#', 1)[0]
        if not u or u.startswith('http'):
            continue
        out.append(u)
    return out


def main() -> int:
    failures: list[str] = []
    sections = [
        ("sw.js PRECACHE",     precache_entries),
        ("asset-manifest.json", manifest_files),
        ("sitemap.xml",        sitemap_paths),
        ("palette.js urls",    palette_urls),
    ]
    print(f'asclepius integrity audit\n')

    total_checked = 0
    for label, fn in sections:
        entries = fn()
        missing = [e for e in entries if not exists(e)]
        total_checked += len(entries)
        if missing:
            failures.extend(f'{label}: missing {m}' for m in missing)
            print(colour('31', f'  ✗ {label}: {len(missing)}/{len(entries)} missing'))
            for m in missing:
                print(f'      {m}')
        else:
            print(colour('32', f'  ✓ {label}: all {len(entries)} entries present'))

    print()
    if failures:
        print(colour('31', f'✗ {len(failures)} integrity issues across {total_checked} entries'))
        return 1
    print(colour('32', f'✓ {total_checked} entries fully consistent'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
