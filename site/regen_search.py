#!/usr/bin/env python3
"""Regenerate search-index.json from every page's <title> + <meta name="description">.

Run after adding or removing pages:

    python3 regen_search.py

The output is consumed by sitegraph.html and (eventually) the in-page
search overlay.
"""
from __future__ import annotations
import glob
import json
import os
import re
import sys
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.abspath(__file__))

EXCLUDE_PAGES = {'404.html'}

TITLE_RE = re.compile(r'<title>([^<]+)</title>', re.I)
DESC_RE  = re.compile(r'<meta\s+name="description"\s+content="([^"]+)"', re.I)
HEAD_RE  = re.compile(r'<h[23][^>]*>(.*?)</h[23]>', re.I | re.S)
TAG_RE   = re.compile(r'<[^>]+>')


def _strip_tags(html: str) -> str:
    return re.sub(r'\s+', ' ', TAG_RE.sub('', html)).strip()


def page_summary(path: str) -> dict | None:
    with open(path, encoding='utf-8') as f:
        s = f.read()
    t = TITLE_RE.search(s)
    d = DESC_RE.search(s)
    if not t or not d:
        return None
    headings = []
    for m in HEAD_RE.finditer(s):
        text = _strip_tags(m.group(1))
        if text and text not in headings:
            headings.append(text)
    return {
        'url':         os.path.basename(path),
        'title':       t.group(1).strip(),
        'description': d.group(1).strip(),
        'headings':    headings,
    }


def main() -> int:
    pages = []
    for f in sorted(glob.glob(os.path.join(HERE, '*.html'))):
        if os.path.basename(f) in EXCLUDE_PAGES:
            continue
        rec = page_summary(f)
        if rec:
            pages.append(rec)
    out = {
        'generated_at': datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
        'count':        len(pages),
        'pages':        pages,
    }
    out_path = os.path.join(HERE, 'search-index.json')
    with open(out_path, 'w', encoding='utf-8') as f:
        json.dump(out, f, indent=2, ensure_ascii=False)
        f.write('\n')
    print(f'wrote search-index.json — {len(pages)} pages')
    return 0


if __name__ == '__main__':
    sys.exit(main())
