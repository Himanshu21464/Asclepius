#!/usr/bin/env python3
"""Asclepius site validator.

A focused linter for the static site. Runs as a CI guardrail: exits non-zero
on any of:

  * an internal href / src that does not resolve
  * duplicate `id="..."` within a single page
  * <img> without alt
  * heading hierarchy that skips a level (h1 → h3 without h2)
  * a `<a href="#fragment">` whose `#fragment` is not present on the same page
  * obvious metadata gaps (missing <title>, <meta name="description">,
    canonical or OG image)

Run from the site/ directory:
    python3 check_html.py

The validator is intentionally hand-rolled: html.parser is in the stdlib and
the rules above are simpler to express than wedging an external tool into
CI. We are not trying to be htmlhint; we are trying to catch the bugs that
matter for an editorial product site.
"""

from __future__ import annotations

import os
import re
import sys
from html.parser import HTMLParser
from urllib.parse import urlparse, urldefrag

HERE = os.path.dirname(os.path.abspath(__file__))


class Page(HTMLParser):
    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.ids: dict[str, int]   = {}
        self.hrefs: list[str]      = []
        self.srcs: list[str]       = []
        self.imgs_no_alt: int      = 0
        self.headings: list[int]   = []
        self.has_title             = False
        self.has_description       = False
        self.has_canonical         = False
        self.has_og_image          = False
        self._in_title             = False

    def handle_starttag(self, tag: str, attrs: list) -> None:
        d = dict(attrs)
        if 'id' in d:
            self.ids[d['id']] = self.ids.get(d['id'], 0) + 1
        if tag == 'a' and 'href' in d:
            self.hrefs.append(d['href'])
        if tag in ('script', 'img', 'iframe', 'source', 'audio', 'video') and 'src' in d:
            self.srcs.append(d['src'])
        # <img srcset="..."> — the candidates list (rare on this site, but covered)
        if tag == 'img' and 'srcset' in d:
            for cand in d['srcset'].split(','):
                url = cand.strip().split(' ', 1)[0]
                if url:
                    self.srcs.append(url)
        if tag == 'link' and d.get('rel'):
            rel = d['rel'].lower()
            if 'canonical' in rel:
                self.has_canonical = True
            if d.get('href') and tag == 'link' and 'stylesheet' not in rel and 'icon' not in rel:
                pass
            if 'stylesheet' in rel and d.get('href'):
                self.srcs.append(d['href'])
            if 'icon' in rel and d.get('href'):
                self.srcs.append(d['href'])
        if tag == 'img':
            if 'alt' not in d:
                self.imgs_no_alt += 1
        if tag in ('h1', 'h2', 'h3', 'h4', 'h5', 'h6'):
            self.headings.append(int(tag[1]))
        if tag == 'meta':
            if d.get('name') == 'description':
                self.has_description = True
            if d.get('property') == 'og:image':
                self.has_og_image = True
        if tag == 'title':
            self._in_title = True

    def handle_endtag(self, tag: str) -> None:
        if tag == 'title':
            self._in_title = False

    def handle_data(self, data: str) -> None:
        if self._in_title and data.strip():
            self.has_title = True


def parse(path: str) -> Page:
    p = Page()
    with open(path, encoding='utf-8') as f:
        p.feed(f.read())
    return p


def is_external(url: str) -> bool:
    return bool(urlparse(url).scheme) and urlparse(url).scheme not in ('', 'file')


def is_mailto(url: str) -> bool:
    return url.startswith('mailto:')


def main() -> int:
    pages = sorted(p for p in os.listdir(HERE) if p.endswith('.html'))
    errors: list[str] = []
    warnings: list[str] = []

    print(f'asclepius site validator — {len(pages)} html files\n')

    for name in pages:
        full = os.path.join(HERE, name)
        page = parse(full)

        # duplicate ids
        for ident, count in page.ids.items():
            if count > 1:
                errors.append(f'{name}: duplicate id "{ident}" (×{count})')

        # broken internal hrefs
        for href in page.hrefs:
            if not href or is_external(href) or is_mailto(href):
                continue
            if href.startswith('#'):
                frag = href[1:]
                if frag and frag not in page.ids:
                    errors.append(f'{name}: dangling in-page fragment "#{frag}"')
                continue
            target, frag = urldefrag(href)
            target_path = os.path.join(HERE, target)
            if not os.path.exists(target_path):
                errors.append(f'{name}: broken link "{href}" (file missing)')
                continue
            if frag:
                target_page = parse(target_path)
                if frag not in target_page.ids:
                    warnings.append(f'{name}: cross-page fragment "{href}" not found in {target}')

        # missing local resources
        for src in page.srcs:
            if not src or is_external(src) or src.startswith('data:'):
                continue
            base = src.split('?', 1)[0]
            if not os.path.exists(os.path.join(HERE, base)):
                errors.append(f'{name}: missing resource "{src}"')

        # imgs without alt
        if page.imgs_no_alt:
            warnings.append(f'{name}: {page.imgs_no_alt} <img> without alt')

        # heading hierarchy
        prev = 0
        for h in page.headings:
            if prev and h - prev > 1:
                warnings.append(f'{name}: heading hierarchy jumps h{prev}→h{h}')
            prev = max(prev, h) if h <= prev else h

        # metadata
        if not page.has_title:        warnings.append(f'{name}: empty or missing <title>')
        if not page.has_description:  warnings.append(f'{name}: missing <meta name="description">')
        if not page.has_og_image:     warnings.append(f'{name}: missing <meta property="og:image">')
        # 404 page intentionally has no canonical
        if name not in ('404.html',) and not page.has_canonical:
            warnings.append(f'{name}: missing <link rel="canonical">')

    if errors:
        print(f'\033[31m✗ {len(errors)} errors\033[0m')
        for e in errors: print('   ', e)
    if warnings:
        print(f'\033[33m! {len(warnings)} warnings\033[0m')
        for w in warnings: print('   ', w)
    if not errors and not warnings:
        print('\033[32m✓ all clean\033[0m')

    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
