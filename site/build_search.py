#!/usr/bin/env python3
"""Build a full-text search index from every HTML page.

Scrapes <title>, meta description, h1/h2/h3 headings, and the first 2-3
paragraphs of body text from each .html file in this directory. Emits
search-index.json — a flat array the palette can fetch and filter.

Run from site/:
    python3 build_search.py
"""
from __future__ import annotations
import json, os, re, sys, time
from html.parser import HTMLParser
from html import unescape


def clean(text: str) -> str:
    """Normalise whitespace and strip mark/tags."""
    return re.sub(r"\s+", " ", text or "").strip()


class Extractor(HTMLParser):
    def __init__(self):
        super().__init__()
        self.title = ""
        self.description = ""
        self.headings: list[tuple[str, str]] = []
        self.first_paragraphs: list[str] = []
        self._stack: list[str] = []
        self._buf: list[str] = []
        self._capture: str | None = None
        self._depth_in_main = 0

    # tracking helpers
    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        self._stack.append(tag)
        # capture title text
        if tag == "title":
            self._capture = "title"
            self._buf = []
        # capture h1/h2/h3 text
        if tag in ("h1", "h2", "h3"):
            self._capture = tag
            self._buf = []
        # capture <p> text — but only the first ~3 paragraphs of body (not nav, foot)
        if tag == "p" and len(self.first_paragraphs) < 3 \
                and self._depth_in_main and not self._inside("footer") \
                and not self._inside("nav"):
            self._capture = "p"
            self._buf = []
        # meta description / og:description
        if tag == "meta":
            n = (attrs.get("name") or "").lower()
            p = (attrs.get("property") or "").lower()
            content = attrs.get("content") or ""
            if n == "description" and not self.description:
                self.description = clean(content)
            elif p == "og:description" and not self.description:
                self.description = clean(content)
        # main body: track depth so we know when to capture <p>
        if tag == "main":
            self._depth_in_main += 1

    def handle_endtag(self, tag):
        if tag == "main":
            self._depth_in_main = max(0, self._depth_in_main - 1)
        # close captures
        if self._capture and self._stack and self._stack[-1] == tag:
            text = clean(unescape("".join(self._buf)))
            if self._capture == "title":
                self.title = text
            elif self._capture in ("h1", "h2", "h3"):
                if text:
                    self.headings.append((self._capture, text))
            elif self._capture == "p":
                if text and len(text) > 24:    # skip trivial fragments
                    self.first_paragraphs.append(text)
            self._capture = None
            self._buf = []
        if self._stack and self._stack[-1] == tag:
            self._stack.pop()

    def handle_data(self, data):
        if self._capture:
            self._buf.append(data)

    def _inside(self, tag: str) -> bool:
        return tag in self._stack


def index_one(path: str) -> dict | None:
    rel = os.path.basename(path)
    with open(path, "r", encoding="utf-8") as f:
        html = f.read()
    ex = Extractor()
    ex.feed(html)
    if not ex.title:
        return None
    body_text = " ".join(ex.first_paragraphs)
    return {
        "url":          rel,
        "title":        ex.title,
        "description":  ex.description,
        "headings":     [{"level": l, "text": t} for l, t in ex.headings],
        "preview":      body_text[:280],
        "wordcount":    len(body_text.split()),
    }


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out: list[dict] = []
    skip = {"sitemap.html"}    # the site-map page is structural duplicate of sitemap.xml
    for fn in sorted(os.listdir(here)):
        if not fn.endswith(".html"):
            continue
        if fn in skip:
            continue
        try:
            entry = index_one(os.path.join(here, fn))
            if entry:
                out.append(entry)
        except Exception as e:
            print(f"  ! {fn}: {e}", file=sys.stderr)

    payload = {
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "count":        len(out),
        "pages":        out,
    }
    with open(os.path.join(here, "search-index.json"), "w", encoding="utf-8") as f:
        json.dump(payload, f, indent=2, ensure_ascii=False)
    print(f"wrote search-index.json — {len(out)} pages, "
          f"{sum(p['wordcount'] for p in out):,} words indexed")


if __name__ == "__main__":
    main()
