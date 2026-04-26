#!/usr/bin/env python3
"""Auto-generate api.html from include/asclepius/*.hpp.

Walks every public header, extracts class names, public method signatures,
free functions, enums, and the doc comments above them, and emits a
single dense reference page.

Run from site/:
    python3 build_api.py
"""
from __future__ import annotations
import os, re, sys, time, html


HERE     = os.path.dirname(os.path.abspath(__file__))
HDR_DIR  = os.path.normpath(os.path.join(HERE, '..', 'include', 'asclepius'))


def collect_headers():
    out = []
    for root, _, files in os.walk(HDR_DIR):
        for fn in files:
            if fn.endswith('.hpp'):
                out.append(os.path.join(root, fn))
    return sorted(out)


# Crude but effective header parser. Matches comments + class/struct/enum/
# function declarations. Designed for the Asclepius codebase's strict style;
# would not handle macro magic or templates with deep nesting.

CLASS_RE   = re.compile(r'^(?:template\s*<[^>]*>\s*)?(?:class|struct)\s+([A-Z][A-Za-z0-9_]*)')
ENUM_RE    = re.compile(r'^enum\s+(?:class\s+)?([A-Z][A-Za-z0-9_]*)')
FUNC_RE    = re.compile(
    # type ... name(args)
    r'^([A-Za-z_][\w:&*<>,\s]*?)\s+([A-Za-z_]\w*)\s*\(([^)]*)\)\s*(?:noexcept|const|override|=\s*default|=\s*delete|=\s*0)?\s*[;{]'
)
METHOD_RE  = FUNC_RE
FREE_FUNC_PREFIXES = ('inline', 'constexpr', 'static', 'extern', 'std::', '[[', 'asclepius::', 'Result<', 'std::', 'void', 'bool', 'int', 'auto', 'ptrdiff_t')


def parse_header(path: str) -> dict:
    with open(path, 'r', encoding='utf-8') as f:
        src = f.read()

    rel = os.path.relpath(path, HDR_DIR)
    name_only = os.path.basename(path)

    blocks = []                  # list of dicts: kind, name, sig, doc, depth
    lines = src.splitlines()

    # walk by line, accumulate doc-comment runs, then attach to the next
    # significant declaration. Skip private/protected sections in classes.
    doc_buf: list[str] = []
    in_class: list[tuple[str, int]] = []  # stack of (class_name, brace_depth)
    visibility = 'public'
    brace_depth = 0
    in_block_comment = False

    for i, raw in enumerate(lines):
        line = raw.rstrip()
        s = line.strip()

        # block-comment passthrough
        if in_block_comment:
            if '*/' in s:
                in_block_comment = False
            continue
        if s.startswith('/*') and '*/' not in s:
            in_block_comment = True
            continue

        # doc comments
        if s.startswith('//'):
            text = s.lstrip('/').strip()
            if text and not text.startswith('SPDX-'):
                doc_buf.append(text)
            continue
        if s == '':
            # blank line breaks a doc-comment run only if the next significant
            # line is itself blank — otherwise let it ride
            if doc_buf and not lines[i:i+2] or all((l.strip() == '' for l in lines[i:i+2])):
                doc_buf.clear()
            continue

        # track brace depth so we know when a class ends
        brace_depth += s.count('{') - s.count('}')

        # close current class if we exit its scope
        while in_class and brace_depth < in_class[-1][1]:
            in_class.pop()
            visibility = 'public'

        # visibility transitions inside class
        if in_class:
            mvis = re.match(r'^(public|protected|private)\s*:', s)
            if mvis:
                visibility = mvis.group(1)
                continue

        # enum
        me = ENUM_RE.match(s)
        if me:
            blocks.append({
                'kind':  'enum',
                'name':  me.group(1),
                'sig':   s.rstrip('{').strip(),
                'doc':   ' '.join(doc_buf),
                'class': in_class[-1][0] if in_class else None,
            })
            doc_buf.clear()
            continue

        # class / struct
        mc = CLASS_RE.match(s)
        if mc and not s.endswith(';'):
            cname = mc.group(1)
            if not in_class:                     # only top-level public classes
                blocks.append({
                    'kind':  'class',
                    'name':  cname,
                    'sig':   s.rstrip('{').strip().rstrip(),
                    'doc':   ' '.join(doc_buf),
                    'class': None,
                })
            in_class.append((cname, brace_depth))
            visibility = 'private' if s.startswith('class') else 'public'
            doc_buf.clear()
            continue

        # forward declaration
        if mc and s.endswith(';'):
            doc_buf.clear()
            continue

        # methods + free functions: only visible ones
        if visibility != 'public' and in_class:
            doc_buf.clear()
            continue

        mf = FUNC_RE.match(s)
        if mf:
            ret  = mf.group(1).strip()
            name = mf.group(2)
            args = mf.group(3).strip()
            # skip C++ keywords / spurious matches
            if name in ('if', 'while', 'for', 'switch', 'return', 'sizeof'):
                continue
            sig  = f'{ret} {name}({args})'.replace('  ', ' ')
            blocks.append({
                'kind':  'method' if in_class else 'function',
                'name':  name,
                'sig':   sig,
                'doc':   ' '.join(doc_buf),
                'class': in_class[-1][0] if in_class else None,
            })
            doc_buf.clear()
            continue

        # anything else clears the doc buffer
        if not s.startswith('//'):
            doc_buf.clear()

    return { 'file': rel, 'name': name_only, 'blocks': blocks }


def render(headers):
    parts = []
    parts.append('<section class="api-toc">')
    parts.append('  <ul>')
    for h in headers:
        slug = h['name'].replace('.', '-').replace('/', '-')
        classes = [b for b in h['blocks'] if b['kind'] == 'class']
        parts.append(f'    <li><a href="#hdr-{slug}"><code>{html.escape(h["file"])}</code> <em>{len(h["blocks"])} symbols</em></a></li>')
    parts.append('  </ul>')
    parts.append('</section>')

    for h in headers:
        slug = h['name'].replace('.', '-').replace('/', '-')
        parts.append(f'<section class="api-hdr" id="hdr-{slug}">')
        parts.append(f'  <h2><code>{html.escape(h["file"])}</code></h2>')

        # group by class
        free = [b for b in h['blocks'] if b['kind'] == 'function']
        enums = [b for b in h['blocks'] if b['kind'] == 'enum' and not b['class']]
        classes: dict[str, list] = {}
        for b in h['blocks']:
            if b['kind'] in ('method',) and b['class']:
                classes.setdefault(b['class'], []).append(b)
            elif b['kind'] == 'class':
                classes.setdefault(b['name'], classes.get(b['name'], []))

        if enums:
            parts.append('  <h3>enums</h3>')
            parts.append('  <dl class="api-list">')
            for e in enums:
                parts.append(f'    <dt><code>{html.escape(e["name"])}</code></dt>')
                parts.append(f'    <dd><pre>{html.escape(e["sig"])}</pre>{("<p>" + html.escape(e["doc"]) + "</p>") if e["doc"] else ""}</dd>')
            parts.append('  </dl>')

        for cname in classes:
            members = classes[cname]
            class_doc = next((b['doc'] for b in h['blocks'] if b['kind'] == 'class' and b['name'] == cname), '')
            parts.append(f'  <article class="api-class">')
            parts.append(f'    <h3><code>{html.escape(cname)}</code></h3>')
            if class_doc:
                parts.append(f'    <p>{html.escape(class_doc)}</p>')
            if members:
                parts.append('    <dl class="api-list">')
                for m in members:
                    parts.append(f'      <dt><code>{html.escape(m["name"])}</code></dt>')
                    parts.append(f'      <dd><pre>{html.escape(m["sig"])}</pre>{("<p>" + html.escape(m["doc"]) + "</p>") if m["doc"] else ""}</dd>')
                parts.append('    </dl>')
            parts.append('  </article>')

        if free:
            parts.append('  <h3>free functions</h3>')
            parts.append('  <dl class="api-list">')
            for f in free:
                parts.append(f'    <dt><code>{html.escape(f["name"])}</code></dt>')
                parts.append(f'    <dd><pre>{html.escape(f["sig"])}</pre>{("<p>" + html.escape(f["doc"]) + "</p>") if f["doc"] else ""}</dd>')
            parts.append('  </dl>')

        parts.append('</section>')

    return '\n'.join(parts)


SHELL = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="color-scheme"        content="dark light">
<meta name="format-detection"   content="telephone=no, date=no, address=no, email=no">
<meta name="referrer"           content="strict-origin-when-cross-origin">
<meta http-equiv="Content-Security-Policy" content="default-src 'self'; img-src 'self' data:; style-src 'self' 'unsafe-inline' https://fonts.googleapis.com; font-src 'self' https://fonts.gstatic.com; script-src 'self' 'unsafe-inline'; connect-src 'self'; base-uri 'self'; form-action 'self'">
<meta http-equiv="Permissions-Policy" content="interest-cohort=(), camera=(), microphone=(), geolocation=(), payment=(), usb=()">
<title>API reference — Asclepius</title>
<meta name="description" content="Auto-generated reference of every public C++ symbol in include/asclepius/. Classes, methods, free functions, enums — direct from the source.">
<meta name="theme-color" content="#0A0710">

<link rel="icon" type="image/svg+xml" href="assets/favicon.svg">
<link rel="manifest" href="manifest.json">
<link rel="apple-touch-icon" href="assets/apple-touch-icon.png">
<link rel="mask-icon" href="assets/favicon-mono.svg" color="#B57BFF">
<link rel="canonical" href="https://asclepius.health/api.html">

<meta property="og:title"        content="Asclepius — API reference">
<meta property="og:description"  content="Auto-generated from the C++ headers. Every public symbol.">
<meta property="og:image"        content="assets/og-card.png">
<meta property="og:type"         content="article">
<meta name="twitter:card"        content="summary_large_image">

<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Instrument+Serif:ital@0;1&family=JetBrains+Mono:wght@400;500;600&family=Newsreader:ital,wght@0,300;0,400;0,500;0,600;1,300;1,400&display=swap" rel="stylesheet">

<link rel="stylesheet" href="styles.css">

<style>
.api-hero {{ padding: clamp(4rem, 8vw, 6rem) 0 1.6rem; }}
.api-hero h1 {{ font-family: var(--font-serif); font-weight: 400; font-size: clamp(2.4rem, 5.5vw, 4rem); line-height: 1; letter-spacing: -.02em; color: var(--ink-warm); margin: 0 0 .8rem; }}
.api-hero h1 i {{ color: var(--primary); font-style: italic; }}
.api-hero p {{ color: var(--ink-mute); max-width: 64ch; line-height: 1.55; }}
.api-hero .kbd {{ display: inline-block; margin-bottom: 1rem; }}
.api-hero p code {{ font-family: var(--font-mono); font-size: .9em; padding: .08em .35em; background: var(--surface-2); border: 1px solid var(--rule-soft); border-radius: 3px; color: var(--ink-warm); }}

.api-toc {{
    margin: 2rem 0 2.4rem;
    padding: 1rem 1.2rem;
    background: var(--surface);
    border: 1px solid var(--rule);
    border-radius: 8px;
}}
.api-toc ul {{ list-style: none; padding: 0; margin: 0;
    display: grid; grid-template-columns: repeat(auto-fit, minmax(260px, 1fr));
    gap: .35rem .9rem;
    font-family: var(--font-mono); font-size: .85rem;
}}
.api-toc a {{ display: flex; justify-content: space-between; gap: .8rem; align-items: baseline;
    color: var(--ink); padding: .35rem .6rem; border-radius: 4px;
    text-decoration: none; transition: background .15s, color .15s;
}}
.api-toc a:hover {{ background: var(--bg-2); color: var(--primary); }}
.api-toc em {{ color: var(--ink-faint); font-style: normal; font-size: .76rem; letter-spacing: .04em; }}

.api-hdr {{ padding: 2rem 0; border-top: 1px solid var(--rule); }}
.api-hdr:first-of-type {{ border-top: 0; padding-top: 0; }}
.api-hdr h2 {{
    font-family: var(--font-serif); font-weight: 400;
    font-size: 1.8rem; color: var(--ink-warm);
    letter-spacing: -.01em; margin: 0 0 1.2rem;
}}
.api-hdr h2 code {{ font-family: var(--font-mono); font-size: .8em; color: var(--primary); background: transparent; border: 0; padding: 0; }}
.api-hdr h3 {{
    font-family: var(--font-mono); font-size: .68rem;
    letter-spacing: .14em; text-transform: uppercase;
    color: var(--ink-faint);
    margin: 1.6rem 0 .6rem;
}}

.api-class {{
    margin: 0 0 2rem;
    padding: 1.2rem 1.4rem;
    background: var(--surface);
    border: 1px solid var(--rule);
    border-radius: 8px;
}}
.api-class h3 {{
    font-family: var(--font-serif);
    font-size: 1.4rem; font-weight: 400;
    color: var(--ink-warm);
    text-transform: none;
    letter-spacing: -.01em;
    margin: 0 0 .8rem;
}}
.api-class h3 code {{ font-family: var(--font-mono); font-size: .8em; color: var(--primary); background: transparent; border: 0; padding: 0; }}
.api-class > p {{ color: var(--ink); font-family: var(--font-prose); line-height: 1.65; margin: 0 0 1rem; }}

.api-list {{ display: grid; grid-template-columns: 12em minmax(0, 1fr); gap: .65rem 1.4rem; margin: 0; }}
.api-list dt {{ color: var(--primary); font-family: var(--font-mono); font-size: .82rem; font-weight: 500; padding-top: .15rem; }}
.api-list dt code {{ background: transparent; border: 0; padding: 0; color: inherit; }}
.api-list dd {{ margin: 0; }}
.api-list pre {{
    margin: 0; padding: .55rem .8rem;
    background: var(--bg); border: 1px solid var(--rule-soft);
    border-radius: 4px; font-family: var(--font-mono); font-size: .8rem;
    color: var(--ink); overflow-x: auto;
}}
.api-list dd p {{ margin: .55rem 0 0; color: var(--ink-mute); font-size: .88rem; line-height: 1.55; }}

@media (max-width: 760px) {{
    .api-list {{ grid-template-columns: 1fr; }}
    .api-list dt {{ padding-top: 0; }}
}}

.api-meta {{
    margin: 2rem 0 4rem;
    padding-top: 1rem;
    border-top: 1px solid var(--rule-soft);
    color: var(--ink-faint);
    font-family: var(--font-mono);
    font-size: .76rem;
    letter-spacing: .04em;
    line-height: 1.7;
}}
.api-meta a {{ color: var(--primary); }}
</style>
</head>
<body class="page-api">

<a class="skip-link" href="#main">skip to content</a>

<header class="status">
  <div class="status__row">
    <a class="status__brand" href="index.html">
      <span class="brand-mark" aria-hidden="true"><span class="brand-mark__inner"></span></span>
      <span class="brand-word">asclepius</span>
      <span class="brand-version">v0.1.0</span>
    </a>
    <nav class="status__nav" aria-label="primary">
      <a href="docs.html">docs</a>
      <a href="api.html" class="is-active" aria-current="page">api</a>
      <a href="cheatsheet.html">cheatsheet</a>
      <a href="glossary.html">glossary</a>
      <a href="demo.html" class="status__nav-cta">playground&nbsp;↗</a>
    </nav>
    <div class="status__meta">
      <span>apache&nbsp;2.0</span>
      <span class="dot"></span>
      <a href="https://github.com/Himanshu21464/Asclepius" class="status__cta">github&nbsp;↗</a>
    </div>
  </div>
</header>

<main id="main">

<section class="api-hero">
  <div class="container">
    <span class="kbd">api · auto-generated · {generated}</span>
    <h1>Every public symbol, <i>from the source.</i></h1>
    <p>This page is regenerated from <code>include/asclepius/*.hpp</code> by <code>build_api.py</code>. {n_headers} headers, {n_symbols} public symbols. The signatures here are the contract; if a future release breaks one without bumping a major version, it's a bug — file it.</p>
  </div>
</section>

<section class="container">
{body}
  <p class="api-meta">
    generated from {n_headers} public headers · run <code>python3 build_api.py</code> from <code>site/</code> to refresh ·
    canonical source: <a href="https://github.com/Himanshu21464/Asclepius/tree/main/include/asclepius">include/asclepius/</a>
  </p>
</section>

</main>

<footer class="foot">
  <div class="container foot__row">
    <div class="foot__brand">
      <span class="brand-mark"><span class="brand-mark__inner"></span></span>
      <span>Asclepius — API reference.</span>
    </div>
    <div class="foot__cols">
      <div>
        <b>project</b>
        <a href="index.html#manifesto">manifesto</a>
        <a href="changelog.html">changelog</a>
        <a href="roadmap.html">roadmap</a>
      </div>
      <div>
        <b>docs</b>
        <a href="architecture.html">architecture</a>
        <a href="spec.html">spec</a>
        <a href="threat-model.html">threat model</a>
        <a href="glossary.html">glossary</a>
        <a href="cheatsheet.html">cheatsheet</a>
      </div>
      <div>
        <b>code</b>
        <a href="https://github.com/Himanshu21464/Asclepius">github&nbsp;↗</a>
        <a href="deploy.html">deploy</a>
        <a href="demo.html">playground</a>
      </div>
    </div>
  </div>
</footer>

<script src="js/themes.js" defer></script>
<script src="js/main.js" defer></script>
<script src="js/help.js" defer></script>
<script src="js/palette.js" defer></script>
<script src="js/floating.js" defer></script>
<script src="js/attest-site.js" defer></script>
<script src="js/extras.js" defer></script>

<script type="speculationrules">
{{"prerender":[{{"where":{{"and":[{{"href_matches":"/*"}},{{"not":{{"href_matches":"/*\\\\?*"}}}}]}},"eagerness":"moderate"}}],"prefetch":[{{"where":{{"and":[{{"href_matches":"/*"}}]}},"eagerness":"conservative"}}]}}
</script>
</body>
</html>
"""


def main():
    headers = []
    n_symbols = 0
    for path in collect_headers():
        try:
            h = parse_header(path)
            headers.append(h)
            n_symbols += len(h['blocks'])
        except Exception as e:
            print(f"  ! {path}: {e}", file=sys.stderr)

    body = render(headers)
    out = SHELL.format(
        body       = body,
        n_headers  = len(headers),
        n_symbols  = n_symbols,
        generated  = time.strftime('%Y-%m-%d', time.gmtime()),
    )
    with open(os.path.join(HERE, 'api.html'), 'w', encoding='utf-8') as f:
        f.write(out)
    print(f"wrote api.html — {len(headers)} headers, {n_symbols} public symbols")


if __name__ == '__main__':
    main()
