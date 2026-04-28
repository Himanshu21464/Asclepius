#!/usr/bin/env python3
"""Render Asclepius markdown docs into stand-alone HTML pages.

Reads ../docs/{ARCHITECTURE,SPEC,THREAT_MODEL}.md and emits
{architecture,spec,threat-model}.html using the same shell as the rest of
the site.

Run from the site/ directory:
    python3 build_docs.py
"""
from __future__ import annotations
import os, sys
import markdown


HERE     = os.path.dirname(os.path.abspath(__file__))
DOCS_SRC = os.path.normpath(os.path.join(HERE, '..', 'docs'))


PAGES = [
    {
        'src':       'ARCHITECTURE.md',
        'out':       'architecture.html',
        'title':     'Architecture — Asclepius',
        'eyebrow':   'docs · 01',
        'h1':        'Architecture',
        'lede':      'Component map, data flow, and the rationale behind every layer of the runtime.',
        'next':      ('spec.html',         'Spec →'),
        'prev':      ('docs.html',         '← Docs'),
        'active':    'architecture',
    },
    {
        'src':       'SPEC.md',
        'out':       'spec.html',
        'title':     'Spec — Asclepius',
        'eyebrow':   'docs · 02',
        'h1':        'Specification',
        'lede':      'On-disk and on-wire formats. The bytes a verifier sees, in lexicographic order.',
        'next':      ('threat-model.html', 'Threat model →'),
        'prev':      ('architecture.html', '← Architecture'),
        'active':    'spec',
    },
    {
        'src':       'THREAT_MODEL.md',
        'out':       'threat-model.html',
        'title':     'Threat Model — Asclepius',
        'eyebrow':   'docs · 03',
        'h1':        'Threat model',
        'lede':      'Adversaries A1–A6, residual risk, and exactly what verify() proves.',
        'next':      ('demo.html',         'Playground →'),
        'prev':      ('spec.html',         '← Spec'),
        'active':    'threat-model',
    },
]


# ---------------------------------------------------------------------------

SHELL = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title}</title>
<meta name="description" content="{description}">
<meta name="theme-color" content="#0A0710">

<link rel="icon" type="image/svg+xml" href="assets/favicon.svg">
<link rel="mask-icon" href="assets/favicon-mono.svg" color="#B57BFF">

<meta property="og:title"        content="{title}">
<meta property="og:description"  content="{description}">
<meta property="og:image"        content="assets/og-card.svg">
<meta property="og:type"         content="article">
<meta name="twitter:card"        content="summary_large_image">

<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Instrument+Serif:ital@0;1&family=JetBrains+Mono:wght@400;500;600&family=Newsreader:ital,wght@0,300;0,400;0,500;0,600;1,300;1,400&display=swap" rel="stylesheet">

<link rel="stylesheet" href="styles.css">
</head>
<body class="page-docs">

<a class="skip-link" href="#main">skip to content</a>

<header class="status">
  <div class="status__row">
    <a class="status__brand" href="index.html">
      <span class="brand-mark" aria-hidden="true"><span class="brand-mark__inner"></span></span>
      <span class="brand-word">asclepius</span>
      <span class="brand-version">v0.6.0</span>
    </a>
    <nav class="status__nav" aria-label="primary">
      <a href="index.html#manifesto">manifesto</a>
      <a href="index.html#substrate">substrate</a>
      <a href="index.html#anatomy">anatomy</a>
      <a href="index.html#start">quickstart</a>
      <a href="docs.html" class="is-active" aria-current="page">docs</a>
      <a href="demo.html" class="status__nav-cta">playground&nbsp;↗</a>
    </nav>
    <div class="status__meta">
      <span>apache&nbsp;2.0</span>
      <span class="dot"></span>
      <button type="button" data-theme-toggle class="theme-toggle" aria-label="toggle theme" aria-pressed="false">
        <svg class="icon-moon" viewBox="0 0 16 16" aria-hidden="true"><path d="M11.5 11.5A6 6 0 0 1 4.5 4.5a6 6 0 1 0 7 7Z"/></svg>
        <svg class="icon-sun"  viewBox="0 0 16 16" aria-hidden="true"><circle cx="8" cy="8" r="3.2"/><g stroke="currentColor" stroke-width="1.4"><line x1="8" y1=".7" x2="8" y2="2.4"/><line x1="8" y1="13.6" x2="8" y2="15.3"/><line x1=".7" y1="8" x2="2.4" y2="8"/><line x1="13.6" y1="8" x2="15.3" y2="8"/><line x1="2.7" y1="2.7" x2="3.9" y2="3.9"/><line x1="12.1" y1="12.1" x2="13.3" y2="13.3"/><line x1="2.7" y1="13.3" x2="3.9" y2="12.1"/><line x1="12.1" y1="3.9" x2="13.3" y2="2.7"/></g></svg>
      </button>
      <a href="https://github.com/Himanshu21464/Asclepius" class="status__cta">github&nbsp;↗</a>
    </div>
  </div>
  <div class="scroll-progress" data-scroll-progress aria-hidden="true"></div>
</header>

<main id="main">
  <div class="container">
    <header class="docs-header">
      <span class="kbd">{eyebrow}</span>
      <h1>{h1}</h1>
      <p>{lede}</p>
    </header>

    <div class="docs-grid">
      <aside class="docs-toc" aria-label="on this page">
        <b>contents</b>
        {toc}
        <b style="margin-top: 1.6rem;">other docs</b>
        <a href="architecture.html" class="{a_arch}">Architecture</a>
        <a href="spec.html"         class="{a_spec}">Spec</a>
        <a href="threat-model.html" class="{a_threat}">Threat model</a>
        <a href="demo.html">Playground&nbsp;↗</a>
      </aside>

      <article class="docs-content">
        {body}

        <hr>
        <p class="docs-pager" style="display:flex;justify-content:space-between;font-family:var(--font-mono);font-size:.82rem;color:var(--ink-mute);">
          <a href="{prev_href}">{prev_label}</a>
          <a href="{next_href}">{next_label}</a>
        </p>
      </article>
    </div>
  </div>
</main>

<footer class="foot">
  <div class="container foot__row">
    <div class="foot__brand">
      <span class="brand-mark"><span class="brand-mark__inner"></span></span>
      <span>Asclepius — docs.</span>
    </div>
    <div class="foot__cols">
      <div>
        <b>project</b>
        <a href="index.html#manifesto">manifesto</a>
        <a href="index.html#substrate">substrate</a>
        <a href="index.html#anatomy">anatomy</a>
      </div>
      <div>
        <b>docs</b>
        <a href="architecture.html">architecture</a>
        <a href="spec.html">spec</a>
        <a href="threat-model.html">threat model</a>
      </div>
      <div>
        <b>code</b>
        <a href="https://github.com/Himanshu21464/Asclepius">github&nbsp;↗</a>
        <a href="index.html#start">quickstart</a>
        <a href="demo.html">playground</a>
      </div>
    </div>
  </div>
</footer>

<script src="js/main.js" defer></script>
<script src="js/palette.js" defer></script>
<script src="js/extras.js" defer></script>
</body>
</html>
"""


def slugify(text: str) -> str:
    raw = ''.join(c if c.isalnum() else '-' for c in text.lower())
    # collapse runs of dashes — em-dashes / spaces / punctuation runs
    # would otherwise yield "a1---internal-..." which doesn't match clean URLs
    while '--' in raw:
        raw = raw.replace('--', '-')
    return raw.strip('-')


def build_toc(html: str) -> str:
    """Extract h2/h3 headings into a TOC list."""
    import re
    headings = re.findall(r'<(h2|h3)[^>]*>(.+?)</\1>', html, re.S)
    if not headings:
        return ''
    out = []
    for tag, text in headings:
        # strip inline tags for label
        label = re.sub(r'<[^>]+>', '', text).strip()
        slug  = slugify(label)
        depth_class = 'toc-l1' if tag == 'h2' else 'toc-l2'
        out.append(f'<a href="#{slug}" class="{depth_class}">{label}</a>')
    return '\n'.join(out)


def add_heading_ids(html: str) -> str:
    """Inject `id="..."` slugs into h2/h3 elements so the TOC works."""
    import re

    def repl(m):
        tag, attrs, body = m.group(1), m.group(2), m.group(3)
        label = re.sub(r'<[^>]+>', '', body).strip()
        slug  = slugify(label)
        return f'<{tag}{attrs} id="{slug}">{body}</{tag}>'

    return re.sub(r'<(h[23])([^>]*)>(.+?)</\1>', repl, html, flags=re.S)


def build():
    converter = markdown.Markdown(
        extensions=['fenced_code', 'tables', 'attr_list', 'def_list'],
        output_format='html5',
    )
    for p in PAGES:
        src = os.path.join(DOCS_SRC, p['src'])
        with open(src, 'r', encoding='utf-8') as f:
            md = f.read()
        # strip leading h1 (we render our own header)
        lines = md.splitlines()
        if lines and lines[0].startswith('# '):
            lines = lines[1:]
        html = converter.convert('\n'.join(lines))
        converter.reset()
        html = add_heading_ids(html)
        toc  = build_toc(html)

        active_map = {
            'architecture': dict(a_arch='is-active', a_spec='', a_threat=''),
            'spec':         dict(a_arch='', a_spec='is-active', a_threat=''),
            'threat-model': dict(a_arch='', a_spec='', a_threat='is-active'),
        }[p['active']]

        out_text = SHELL.format(
            title       = p['title'],
            description = p['lede'],
            eyebrow     = p['eyebrow'],
            h1          = p['h1'],
            lede        = p['lede'],
            toc         = toc,
            body        = html,
            prev_href   = p['prev'][0],
            prev_label  = p['prev'][1],
            next_href   = p['next'][0],
            next_label  = p['next'][1],
            **active_map,
        )

        out = os.path.join(HERE, p['out'])
        with open(out, 'w', encoding='utf-8') as f:
            f.write(out_text)
        print(f'wrote {p["out"]}  ({len(out_text):>6} bytes)')


if __name__ == '__main__':
    build()
