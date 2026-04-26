// SPDX-License-Identifier: Apache-2.0
// Asclepius — site enhancement layer (complementary to main.js).
//
// Three small, paranoia-tolerant helpers that progressively enhance the
// static HTML. Each is feature-detected and a no-op on older browsers or
// when prefers-reduced-motion is set. main.js handles the theme toggle,
// scroll spy, accordion, and its own embedded demo; this file handles the
// editorial polish: reveal-on-scroll, hand-built copy buttons on code
// blocks, status-strip frost, and the chain-hash dot breathing.

(() => {
    'use strict';
    const reduce = matchMedia('(prefers-reduced-motion: reduce)').matches;

    // ── 1. reveal on scroll ──────────────────────────────────────────────
    const observed = document.querySelectorAll(
        'section h2.display, .pillar, .incident, .signals li, ' +
        '.verify__copy, .verify__quote, .open__inner, .now h2 + ol > li, ' +
        '.d-pane, .d-ledger-section__head, .d-drift__copy, .d-bars'
    );
    if (!reduce && 'IntersectionObserver' in window && observed.length) {
        observed.forEach((el) => el.classList.add('to-reveal'));
        const io = new IntersectionObserver(
            (entries) => {
                for (const e of entries) {
                    if (e.isIntersecting) {
                        e.target.classList.add('is-revealed');
                        io.unobserve(e.target);
                    }
                }
            },
            { rootMargin: '0px 0px -8% 0px', threshold: 0.05 }
        );
        const vh = innerHeight || 0;
        observed.forEach((el) => {
            // direct anchor jumps land us mid-page — pre-reveal anything
            // already at or above the viewport so users don't hit blank space.
            const r = el.getBoundingClientRect();
            if (r.top < vh * 1.05) {
                el.classList.add('is-revealed');
            } else {
                io.observe(el);
            }
        });
    } else if (reduce) {
        // reduced-motion users get content immediately, no animation classes
        observed.forEach((el) => el.classList.remove('to-reveal'));
    }

    // ── 2. copy-to-clipboard on existing code blocks ─────────────────────
    document.querySelectorAll(
        'pre.panel, pre.terminal__body, pre.pillar__code, .docs-content pre, .d-output__body, .d-entry__body pre'
    ).forEach((pre) => {
        if (pre.dataset.noCopy === '1' || pre.closest('.copy-wrap')) return;
        const wrap = document.createElement('div');
        wrap.className = 'copy-wrap';
        const btn = document.createElement('button');
        btn.type = 'button';
        btn.className = 'copy-btn';
        btn.setAttribute('aria-label', 'copy code');
        btn.innerHTML =
            '<span class="copy-btn__label">copy</span>' +
            '<svg class="copy-btn__icon" viewBox="0 0 16 16" width="13" height="13" aria-hidden="true">' +
            '<path d="M5 1.5h6.5a1 1 0 0 1 1 1V11h-1V2.5H5v-1Zm-2 3h7.5a1 1 0 0 1 1 1v9a1 1 0 0 1-1 1H3a1 1 0 0 1-1-1v-9a1 1 0 0 1 1-1Zm0 1v9h7.5v-9H3Z" fill="currentColor"/>' +
            '</svg>';
        if (pre.parentNode) {
            pre.parentNode.insertBefore(wrap, pre);
            wrap.appendChild(pre);
            wrap.appendChild(btn);
        }
        btn.addEventListener('click', async () => {
            try {
                await navigator.clipboard.writeText(pre.innerText.replace(/\n+$/, ''));
                btn.classList.add('is-copied');
                btn.querySelector('.copy-btn__label').textContent = 'copied';
                setTimeout(() => {
                    btn.classList.remove('is-copied');
                    btn.querySelector('.copy-btn__label').textContent = 'copy';
                }, 1400);
            } catch (e) {
                btn.querySelector('.copy-btn__label').textContent = 'failed';
            }
        });
    });

    // ── 3. status strip frosts on scroll ─────────────────────────────────
    const status = document.querySelector('.status');
    if (status) {
        const onScroll = () => {
            status.classList.toggle('is-scrolled', window.scrollY > 8);
        };
        onScroll();
        addEventListener('scroll', onScroll, { passive: true });
    }

    // ── 4. living chain in the hero artifact ─────────────────────────────
    if (!reduce) {
        document.querySelectorAll('.chain__hash').forEach((h, i) => {
            h.style.animation = `chain-breathe 3.8s ease-in-out ${i * 0.4}s infinite`;
        });
    }

    // ── 4b. anchor flash on hash navigation ──────────────────────────────
    //
    // When the URL hash changes (Cmd-K, anchor link, browser back/forward),
    // briefly glow the target so the user sees where they landed. No-op on
    // reduced-motion users.
    function flashAnchor() {
        if (reduce) return;
        const hash = location.hash;
        if (!hash || hash.length < 2) return;
        const el = document.querySelector(hash);
        if (!el) return;
        el.classList.remove('anchor-flash');
        // re-trigger the animation
        void el.offsetWidth;                          // eslint-disable-line no-unused-expressions
        el.classList.add('anchor-flash');
        setTimeout(() => el.classList.remove('anchor-flash'), 1700);
    }
    addEventListener('hashchange', flashAnchor);
    // also flash on initial page load if a hash is present
    if (location.hash) {
        // tiny delay so layout has settled and the target is in the right place
        setTimeout(flashAnchor, 80);
    }

    // ── 4c. typed-out terminal in the #verify section ────────────────────
    //
    // The terminal that demonstrates the offline-verify ritual gets typed
    // out one line at a time when it scrolls into view. Each line preserves
    // its inline markup (the green <span class="t-ok"> stays green) while
    // animating in.
    if (!reduce && 'IntersectionObserver' in window) {
        document.querySelectorAll('.terminal__body').forEach((pre) => {
            // skip terminals inside `<pre data-no-type="1">` overrides
            if (pre.dataset.noType === '1') return;

            // capture children once; replace with empty pre that we'll fill
            // line-by-line. We split on actual newlines, preserving inner
            // HTML on each line.
            const original = pre.innerHTML;
            // split into lines while keeping the trailing newlines for
            // visual fidelity
            const lines = original.split('\n');
            // wrap each line in a span so we can stagger reveals
            pre.innerHTML = lines
                .map((ln) => `<span class="typed-line">${ln}</span>`)
                .join('\n');
            pre.dataset.typing = 'true';

            const spans = pre.querySelectorAll('.typed-line');
            const io = new IntersectionObserver((entries, obs) => {
                for (const e of entries) {
                    if (!e.isIntersecting) continue;
                    obs.unobserve(pre);
                    spans.forEach((s, i) => {
                        setTimeout(() => s.classList.add('is-shown'), i * 90);
                    });
                    // append a blinking cursor at the end after the last line
                    setTimeout(() => {
                        const cur = document.createElement('span');
                        cur.className = 'type-cursor';
                        cur.setAttribute('aria-hidden', 'true');
                        pre.appendChild(cur);
                    }, spans.length * 90 + 200);
                }
            }, { threshold: 0.35, rootMargin: '0px 0px -10% 0px' });
            io.observe(pre);
        });
    }

    // ── 4d. hover preview cards for internal links ───────────────────────
    //
    // Hover any same-origin .html link for ~280 ms and a small popover
    // appears with that page's <title> and meta description, fetched lazily
    // and cached for the rest of the session. Esc / leave dismisses.
    const previewCache = new Map();
    let previewEl = null;
    let previewTimer = null;

    function ensurePreviewEl() {
        if (previewEl) return previewEl;
        previewEl = document.createElement('div');
        previewEl.className = 'link-preview';
        previewEl.setAttribute('role', 'tooltip');
        previewEl.hidden = true;
        document.body.appendChild(previewEl);
        return previewEl;
    }

    async function fetchPreview(url) {
        if (previewCache.has(url)) return previewCache.get(url);
        try {
            const res = await fetch(url, { credentials: 'same-origin' });
            if (!res.ok) throw new Error(res.status);
            const text = await res.text();
            const title = (text.match(/<title>([^<]+)<\/title>/i) || [, ''])[1].trim();
            const desc  = (text.match(/<meta\s+name="description"\s+content="([^"]+)"/i) || [, ''])[1].trim();

            // Smarter: if the URL has a hash (anchor), try to extract the
            // matching glossary entry's specific definition rather than the
            // whole-page description.
            let anchor = null;
            const hashIdx = url.indexOf('#');
            if (hashIdx > -1) {
                const hash = url.slice(hashIdx + 1);
                if (hash) {
                    // glossary.html has each term as <div class="gl-entry" id="…">
                    const re = new RegExp(
                        `<div\\s+class="gl-entry"\\s+id="${hash.replace(/[-/\\^$*+?.()|[\]{}]/g, '\\$&')}"[^>]*>([\\s\\S]*?)</div>`,
                        'i'
                    );
                    const m = text.match(re);
                    if (m) {
                        // strip nested tags, take the <dd> body if present, fall back to the whole entry
                        const ddMatch = m[1].match(/<dd[^>]*>([\s\S]*?)<\/dd>/i);
                        const raw = ddMatch ? ddMatch[1] : m[1];
                        anchor = raw
                            .replace(/<[^>]+>/g, ' ')
                            .replace(/\s+/g, ' ')
                            .trim();
                        if (anchor.length > 240) anchor = anchor.slice(0, 240) + '…';
                    }
                }
            }

            const out = { title, desc: anchor || desc, isAnchor: !!anchor };
            previewCache.set(url, out);
            return out;
        } catch (e) {
            const out = { title: '', desc: '' };
            previewCache.set(url, out);
            return out;
        }
    }

    function showPreview(a, info) {
        const el = ensurePreviewEl();
        if (!info.title && !info.desc) { el.hidden = true; return; }
        el.innerHTML =
            (info.title ? `<b>${escapeHtml(info.title)}</b>` : '') +
            (info.desc  ? `<span>${escapeHtml(info.desc)}</span>` : '') +
            `<em>${escapeHtml(a.getAttribute('href'))}</em>`;
        el.hidden = false;
        const r = a.getBoundingClientRect();
        const left = Math.min(window.innerWidth - 360, Math.max(8, r.left));
        const top  = Math.min(window.innerHeight - 140,
                              window.scrollY + r.bottom + 8);
        el.style.left = left + 'px';
        el.style.top  = top  + 'px';
    }

    function hidePreview() {
        if (previewEl) previewEl.hidden = true;
        clearTimeout(previewTimer);
    }

    function escapeHtml(s) {
        return String(s).replace(/[&<>"']/g, (c) => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
    }

    if (!reduce && window.fetch) {
        document.addEventListener('mouseenter', (e) => {
            const a = e.target instanceof Element ? e.target.closest('a') : null;
            if (!a) return;
            const href = a.getAttribute('href');
            if (!href || href.startsWith('#') || href.startsWith('mailto:') ||
                href.startsWith('javascript:')) return;
            try {
                const u = new URL(href, location.href);
                if (u.origin !== location.origin) return;
                if (!u.pathname.endsWith('.html') && u.pathname !== '/' &&
                    !u.pathname.endsWith('/')) return;
                if (u.pathname === location.pathname && !u.hash) return;
            } catch (_) { return; }
            clearTimeout(previewTimer);
            previewTimer = setTimeout(async () => {
                const url  = new URL(href, location.href).href;
                const info = await fetchPreview(url);
                showPreview(a, info);
            }, 320);
        }, true);
        document.addEventListener('mouseleave', (e) => {
            const a = e.target instanceof Element ? e.target.closest('a') : null;
            if (a) hidePreview();
        }, true);
        document.addEventListener('keydown', (e) => {
            if (e.key === 'Escape') hidePreview();
        });
        addEventListener('scroll', hidePreview, { passive: true });
    }

    // ── 4e. service worker registration ─────────────────────────────────
    if ('serviceWorker' in navigator && location.protocol !== 'file:') {
        addEventListener('load', () => {
            navigator.serviceWorker.register('sw.js', { scope: './' })
                .catch(() => {/* SW is opt-in; failure is non-fatal */});
        });
    }

    // ── 4f1. glossary auto-linker (docs pages only) ─────────────────────
    //
    // Wraps the FIRST occurrence of each glossary term in a link to the
    // glossary anchor. Conservative: only inside <p> children of
    // .docs-content, and never inside <a>, <code>, or headings.
    if (document.body.classList.contains('page-docs') &&
        !location.pathname.endsWith('docs.html')) {
        // term → glossary-anchor
        const TERMS = [
            ['BLAKE2b-256',          'blake2b-256'],
            ['BLAKE2b',              'blake2b-256'],
            ['Ed25519',              'ed25519'],
            ['Merkle',               'merkle'],
            ['canonical JSON',       'canonical-json'],
            ['evidence bundle',      'bundle'],
            ['ground truth',         'ground-truth'],
            ['conformance suite',    'conformance'],
            ['consent token',        'consent-token'],
            ['ConsentToken',         'consent-token'],
            ['EncounterId',          'encounter-id'],
            ['PatientId',            'patient-id'],
            ['Purpose',              'purpose'],
            ['Result<T',        'result'],
            ['SOAP note',            'soap'],
            ['SOAP',                 'soap'],
            ['SQLite WAL',           'sqlite-wal'],
            ['ULID',                 'ulid'],
            ['USTAR',                'ustar'],
            ['attestation',          'attestation'],
            ['HIPAA',                'hipaa'],
            ['BAA',                  'baa'],
            ['HSM',                  'hsm'],
            ['CDSS',                 'cdss'],
            ['FHIR',                 'fhir'],
            ['MRN',                  'mrn'],
            ['EMD',                  'emd'],
            ['PSI',                  'psi'],
            ['KS statistic',         'ks'],
            ['PHI',                  'phi'],
        ];
        const seen = new Set();
        const target = document.querySelector('.docs-content');
        if (target) {
            // walk paragraph nodes only
            const ps = target.querySelectorAll('p, li');
            for (const p of ps) {
                // skip paragraphs that are children of pre / code (shouldn't happen but be safe)
                if (p.closest('pre, code, h1, h2, h3, h4, h5, h6')) continue;
                walkTextNodes(p, (node) => {
                    let text = node.textContent;
                    for (const [term, anchor] of TERMS) {
                        if (seen.has(anchor)) continue;
                        // word-boundary, case-sensitive for acronyms,
                        // ci for words longer than 4 chars
                        const isAcronym = term === term.toUpperCase() && term.length <= 5;
                        const flags = isAcronym ? '' : 'i';
                        const escaped = term.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
                        const re = new RegExp(`\\b(${escaped})\\b`, flags);
                        const m = re.exec(text);
                        if (!m) continue;
                        seen.add(anchor);
                        const before = text.slice(0, m.index);
                        const matched = m[1];
                        const after  = text.slice(m.index + matched.length);
                        const a = document.createElement('a');
                        a.className = 'glossary-link';
                        a.href = 'glossary.html#' + anchor;
                        a.title = 'Defined in the glossary';
                        a.textContent = matched;
                        const beforeNode = document.createTextNode(before);
                        const afterNode  = document.createTextNode(after);
                        node.replaceWith(beforeNode, a, afterNode);
                        // continue scanning from `after`
                        text = after;
                        node = afterNode;
                    }
                });
            }
        }
    }

    function walkTextNodes(root, fn) {
        const walker = document.createTreeWalker(
            root, NodeFilter.SHOW_TEXT,
            { acceptNode(n) {
                if (n.parentElement.closest('a, code, kbd, .glossary-link')) {
                    return NodeFilter.FILTER_REJECT;
                }
                if (n.nodeValue.trim().length === 0) return NodeFilter.FILTER_REJECT;
                return NodeFilter.FILTER_ACCEPT;
            } });
        const nodes = [];
        let n; while ((n = walker.nextNode())) nodes.push(n);
        nodes.forEach(fn);
    }

    // ── 4f2. reading-time on docs pages ─────────────────────────────────
    if (document.body.classList.contains('page-docs')) {
        const main = document.querySelector('.docs-content') ||
                     document.querySelector('main');
        const header = document.querySelector('.docs-header');
        if (main && header && !header.querySelector('.read-time')) {
            const words = (main.innerText || '').trim().split(/\s+/).length;
            const minutes = Math.max(1, Math.round(words / 230));
            const pill = document.createElement('span');
            pill.className = 'read-time';
            pill.innerHTML = `<i>·</i> ${minutes}&nbsp;min read <i>·</i> ${words.toLocaleString()}&nbsp;words`;
            header.querySelector('.kbd')?.after(pill);
        }
    }

    // ── 4f3. 404 fuzzy "did you mean" suggestions ───────────────────────
    if (document.body.classList.contains('page-404')) {
        const KNOWN = [
            'index.html', 'docs.html', 'demo.html', 'architecture.html', 'spec.html',
            'threat-model.html', 'benchmarks.html', 'changelog.html', 'roadmap.html',
            'security.html', 'conformance.html', 'status.html', 'crypto.html',
            'labs.html', 'glossary.html', 'contributing.html', 'press.html',
        ];
        const tried = location.pathname.replace(/^\//, '').toLowerCase() || '';
        if (tried) {
            // crude similarity: shared chars / length
            function similarity(a, b) {
                const aSet = new Set(a);
                const bSet = new Set(b);
                let common = 0;
                for (const c of aSet) if (bSet.has(c)) common++;
                return common / Math.max(a.length, b.length, 1);
            }
            const ranked = KNOWN
                .map((p) => ({ p, s: similarity(tried, p.toLowerCase()) }))
                .sort((a, b) => b.s - a.s)
                .slice(0, 3)
                .filter((x) => x.s > 0.18);
            if (ranked.length) {
                const v404 = document.querySelector('.v404');
                if (v404 && !v404.querySelector('.v404__dym')) {
                    const dym = document.createElement('div');
                    dym.className = 'v404__dym';
                    dym.innerHTML = `<small>did you mean</small> ` +
                        ranked.map((r) => `<a href="${r.p}">${r.p.replace('.html', '')}</a>`).join(' · ');
                    v404.querySelector('.v404__nav')?.before(dym);
                }
            }
        }
    }

    // ── 4f. animated architecture-diagram trigger ───────────────────────
    const archDiagram = document.querySelector('.arch-diagram');
    if (archDiagram && 'IntersectionObserver' in window) {
        if (reduce) {
            archDiagram.classList.add('is-in');
        } else {
            const archIo = new IntersectionObserver((entries, obs) => {
                for (const e of entries) {
                    if (!e.isIntersecting) continue;
                    e.target.classList.add('is-in');
                    obs.unobserve(e.target);
                }
            }, { threshold: 0.25, rootMargin: '0px 0px -10% 0px' });
            archIo.observe(archDiagram);
        }
    }

    // ── 5. mobile hamburger nav ──────────────────────────────────────────
    //
    // Injected progressively: at narrow viewports the burger appears, the
    // existing inline nav collapses into a dropdown panel. No HTML changes
    // needed — the markup is the same on every page.
    const statusRow = document.querySelector('.status__row');
    const statusNav = document.querySelector('.status__nav');
    if (statusRow && statusNav && !statusRow.querySelector('.nav-burger')) {
        const burger = document.createElement('button');
        burger.type  = 'button';
        burger.className = 'nav-burger';
        burger.setAttribute('aria-label', 'menu');
        burger.setAttribute('aria-expanded', 'false');
        burger.setAttribute('aria-controls',  'asc-mobile-nav');
        burger.innerHTML = '<span></span><span></span><span></span>';
        statusNav.id = 'asc-mobile-nav';

        // place it right after the brand so the layout stays balanced
        const brand = statusRow.querySelector('.status__brand');
        if (brand && brand.nextSibling) {
            statusRow.insertBefore(burger, brand.nextSibling);
        } else {
            statusRow.appendChild(burger);
        }

        const setOpen = (open) => {
            burger.setAttribute('aria-expanded', String(!!open));
            document.body.classList.toggle('nav-open', !!open);
        };

        burger.addEventListener('click', () => {
            const isOpen = burger.getAttribute('aria-expanded') === 'true';
            setOpen(!isOpen);
        });
        statusNav.querySelectorAll('a').forEach((a) => {
            a.addEventListener('click', () => setOpen(false));
        });
        document.addEventListener('keydown', (e) => {
            if (e.key === 'Escape' && document.body.classList.contains('nav-open')) {
                setOpen(false);
                burger.focus();
            }
        });
        // tapping outside the panel closes it
        document.addEventListener('click', (e) => {
            if (!document.body.classList.contains('nav-open')) return;
            if (e.target.closest('.status, .nav-burger')) return;
            setOpen(false);
        });
        // close on resize past breakpoint
        addEventListener('resize', () => {
            if (innerWidth > 760) setOpen(false);
        }, { passive: true });
    }
})();
