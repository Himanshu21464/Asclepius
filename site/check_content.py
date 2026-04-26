#!/usr/bin/env python3
"""Page-depth audit.

For each HTML page in the site, count the number of words inside <main>.
Flag pages with suspiciously little content. Catches:

  - skeleton pages with chrome but no body
  - pages where a refactor accidentally deleted content

Threshold defaults to 200 words. Override with `--min N`.

Exit code is 0 even if pages are thin — this audit is informational, not
gating. If you want to fail on thin pages, pipe through `grep -q` or set
ASCLEPIUS_CONTENT_MIN.
"""
from __future__ import annotations
import argparse
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# pages where low word count is intentional
INTENTIONALLY_SHORT = {
    '404.html',          # "the chain broke at this seq" + 3 buttons
    'sitegraph.html',    # almost all SVG / data-driven
    'sitemap.html',      # mostly link list, low prose
    'badges.html',       # mostly preview cards
    'crypto.html',       # mostly interactive widgets, prose is structural
    'demo.html',         # interactive playground, sparse prose
    'sandbox.html',      # interactive
    'explorer.html',     # interactive
    'verify-bundle.html',# interactive
    'validate.html',     # interactive
    'compare.html',      # interactive bundle differ
    'glossary.html',     # rendered list, treat as data
    'status.html',       # dashboard cells
}

MAIN_RE   = re.compile(r'<main[^>]*>(.*?)</main>', re.S | re.I)
SCRIPT_RE = re.compile(r'<script[^>]*>.*?</script>', re.S | re.I)
STYLE_RE  = re.compile(r'<style[^>]*>.*?</style>', re.S | re.I)
TAG_RE    = re.compile(r'<[^>]+>')
WORD_RE   = re.compile(r"[A-Za-z0-9'’]+")


def word_count(html: str) -> int:
    m = MAIN_RE.search(html)
    body = m.group(1) if m else html
    body = SCRIPT_RE.sub('', body)
    body = STYLE_RE.sub('', body)
    text = TAG_RE.sub(' ', body)
    return len(WORD_RE.findall(text))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--min', type=int,
                    default=int(os.environ.get('ASCLEPIUS_CONTENT_MIN', '200')))
    ap.add_argument('--all', action='store_true', help='show all pages, not just thin ones')
    args = ap.parse_args()

    rows = []
    for f in sorted(glob.glob(os.path.join(HERE, '*.html'))):
        name = os.path.basename(f)
        with open(f, encoding='utf-8') as fh:
            n = word_count(fh.read())
        rows.append((name, n))

    print(f'asclepius content audit · threshold = {args.min} words\n')

    thin = []
    for name, n in rows:
        intentional = name in INTENTIONALLY_SHORT
        if args.all:
            tag = '\033[33m thin\033[0m' if (n < args.min and not intentional) \
                  else ('\033[2m short(*) \033[0m' if intentional else '   ')
            print(f'  {tag} {name:<22} {n:>5} words')
        if n < args.min and not intentional:
            thin.append((name, n))

    if not args.all:
        for name, n in rows:
            mark = '\033[33m✗\033[0m' if (n < args.min and name not in INTENTIONALLY_SHORT) else '\033[32m✓\033[0m'
            print(f'  {mark} {name:<22} {n:>5}')

    print()
    if thin:
        print(f'\033[33m{len(thin)} pages below threshold:\033[0m')
        for name, n in thin:
            print(f'   {name:<22} {n:>5} words')
    else:
        print('\033[32m✓ every page above threshold (or intentionally short)\033[0m')

    return 0  # informational, never fail


if __name__ == '__main__':
    sys.exit(main())
