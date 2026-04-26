#!/usr/bin/env python3
"""Regenerate asset-manifest.json — SHA-256 of every static file the site serves.

Read by `js/attest-site.js` so visitors can verify the bytes they received
against the published manifest. Re-run after any non-trivial edit:

    python3 regen_manifest.py

The manifest is itself published; it does not include itself.
"""
from __future__ import annotations
import hashlib
import json
import os
import sys
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.abspath(__file__))

# What goes into the manifest. Globs evaluated relative to HERE.
INCLUDE = (
    '*.html',
    '*.xml',
    '*.json',
    '*.txt',
    '*.css',
    '*.svg',
    'js/*.js',
    'assets/*.svg',
    'assets/*.png',
    'assets/*.json',
    'assets/*.asc',
    '.well-known/*.txt',
)

# Never hash these (the manifest itself, transient files).
EXCLUDE = {
    'asset-manifest.json',          # do not include self
    'search-index.json',            # auto-generated, hashed separately if needed
    'regen_manifest.py',
    'regen_search.py',
    'check_html.py',
    'build_docs.py',
    'Makefile',
    '_headers',
    '_redirects',
    '.editorconfig',
}


def matched(rel: str) -> bool:
    import fnmatch
    if rel in EXCLUDE:
        return False
    return any(fnmatch.fnmatch(rel, pat) for pat in INCLUDE)


def sha256(path: str) -> tuple[str, int]:
    h = hashlib.sha256()
    n = 0
    with open(path, 'rb') as f:
        while chunk := f.read(64 * 1024):
            h.update(chunk)
            n += len(chunk)
    return h.hexdigest(), n


def collect():
    out = {}
    for root, dirs, files in os.walk(HERE):
        # skip hidden dirs except .well-known
        dirs[:] = [d for d in dirs if not d.startswith('.') or d == '.well-known']
        for name in files:
            full = os.path.join(root, name)
            rel = os.path.relpath(full, HERE).replace(os.sep, '/')
            if not matched(rel):
                continue
            digest, n = sha256(full)
            out[rel] = {'sha256': digest, 'bytes': n}
    return out


def main() -> int:
    files = collect()
    manifest = {
        'generated_at': datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
        'algorithm':    'SHA-256',
        'count':        len(files),
        'files':        dict(sorted(files.items())),
    }
    out_path = os.path.join(HERE, 'asset-manifest.json')
    with open(out_path, 'w', encoding='utf-8') as f:
        json.dump(manifest, f, indent=2, ensure_ascii=False)
        f.write('\n')
    print(f'wrote asset-manifest.json — {len(files)} files')
    return 0


if __name__ == '__main__':
    sys.exit(main())
