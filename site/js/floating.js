// SPDX-License-Identifier: Apache-2.0
// Asclepius — bottom-right floating action cluster.
//
// Three small buttons that appear after the user scrolls:
//   ▲   scroll to top
//   ⤴   share permalink (copies the current URL — including hash — to
//       the clipboard, then briefly flashes "copied")
//   ◇   page attestation pill (computes SHA-256 of the rendered <main>
//       content on click, shows it in a popover; one more click copies
//       the hex)
//
// All three respect prefers-reduced-motion and the mobile viewport.

(() => {
    'use strict';

    if (document.documentElement.classList.contains('no-fab')) return;

    const enc = new TextEncoder();
    const bytesToHex = (bytes) =>
        Array.from(new Uint8Array(bytes)).map((b) => b.toString(16).padStart(2, '0')).join('');

    function makeBtn(cls, label, glyph) {
        const b = document.createElement('button');
        b.type = 'button';
        b.className = `fab ${cls}`;
        b.setAttribute('aria-label', label);
        b.innerHTML = glyph;
        return b;
    }

    function build() {
        const wrap = document.createElement('div');
        wrap.className = 'fab-cluster';
        wrap.setAttribute('aria-label', 'page tools');
        wrap.hidden = true;

        const top = makeBtn('fab--top', 'scroll to top',
            '<svg viewBox="0 0 16 16" width="14" height="14" aria-hidden="true">' +
            '<path d="M8 3v9M4 7l4-4 4 4" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" fill="none"/>' +
            '</svg>');

        const share = makeBtn('fab--share', 'copy permalink',
            '<svg viewBox="0 0 16 16" width="14" height="14" aria-hidden="true">' +
            '<path d="M11 5h2a2 2 0 0 1 2 2v6a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2v-2" stroke="currentColor" stroke-width="1.5" fill="none"/>' +
            '<rect x="1" y="1" width="10" height="10" rx="2" stroke="currentColor" stroke-width="1.5" fill="none"/>' +
            '</svg>' +
            '<span class="fab__hint">copy link</span>');

        const attest = makeBtn('fab--attest', 'compute page attestation',
            '<svg viewBox="0 0 16 16" width="14" height="14" aria-hidden="true">' +
            '<polygon points="8,1.5 14.5,5.5 14.5,10.5 8,14.5 1.5,10.5 1.5,5.5" stroke="currentColor" stroke-width="1.5" fill="none"/>' +
            '<circle cx="8" cy="8" r="2" fill="currentColor"/>' +
            '</svg>' +
            '<span class="fab__hint">attest page</span>');

        wrap.append(top, share, attest);

        // attestation popover
        const pop = document.createElement('div');
        pop.className = 'fab-pop';
        pop.hidden = true;
        pop.innerHTML = `
            <header>
                <span>page attestation · sha-256</span>
                <button type="button" class="fab-pop__copy" aria-label="copy hex">copy</button>
            </header>
            <code id="fab-pop-hex">computing…</code>
            <footer>
                <span>computed in your browser</span>
                <span>over rendered &lt;main&gt;</span>
            </footer>`;
        wrap.appendChild(pop);

        document.body.appendChild(wrap);

        return { wrap, top, share, attest, pop };
    }

    const els = build();

    // ── show / hide on scroll ────────────────────────────────────────────
    const onScroll = () => {
        els.wrap.hidden = window.scrollY < 600;
    };
    onScroll();
    addEventListener('scroll', onScroll, { passive: true });
    addEventListener('resize', onScroll, { passive: true });

    // ── scroll to top ────────────────────────────────────────────────────
    els.top.addEventListener('click', () => {
        const reduce = matchMedia('(prefers-reduced-motion: reduce)').matches;
        scrollTo({ top: 0, behavior: reduce ? 'auto' : 'smooth' });
    });

    // ── share permalink ──────────────────────────────────────────────────
    els.share.addEventListener('click', async () => {
        const url = location.href;
        try {
            if (navigator.share && location.protocol === 'https:') {
                await navigator.share({ title: document.title, url });
            } else {
                await navigator.clipboard.writeText(url);
                flash(els.share, 'copied');
            }
        } catch (_) {
            try { await navigator.clipboard.writeText(url); flash(els.share, 'copied'); }
            catch (__) { flash(els.share, 'failed'); }
        }
    });

    function flash(btn, msg) {
        const hint = btn.querySelector('.fab__hint');
        if (!hint) return;
        const prev = hint.textContent;
        hint.textContent = msg;
        btn.classList.add('is-flash');
        setTimeout(() => { hint.textContent = prev; btn.classList.remove('is-flash'); }, 1500);
    }

    // ── page attestation pill ────────────────────────────────────────────
    let lastHex = null;
    els.attest.addEventListener('click', async () => {
        if (!els.pop.hidden) { els.pop.hidden = true; return; }
        els.pop.hidden = false;
        const hexEl = els.pop.querySelector('#fab-pop-hex');
        try {
            const main = document.querySelector('main')?.innerText || document.body.innerText;
            const buf = await crypto.subtle.digest('SHA-256', enc.encode(main));
            lastHex = bytesToHex(buf);
            hexEl.textContent = lastHex.match(/.{1,8}/g).join(' ');
        } catch (e) {
            hexEl.textContent = 'unable to compute (' + (e.message || e) + ')';
        }
    });
    els.pop.querySelector('.fab-pop__copy').addEventListener('click', async () => {
        if (!lastHex) return;
        try {
            await navigator.clipboard.writeText(lastHex);
            const btn = els.pop.querySelector('.fab-pop__copy');
            btn.textContent = 'copied';
            setTimeout(() => (btn.textContent = 'copy'), 1300);
        } catch (_) {}
    });

    // close popover on Esc / outside click
    addEventListener('keydown', (e) => {
        if (e.key === 'Escape') els.pop.hidden = true;
    });
    addEventListener('click', (e) => {
        if (els.pop.hidden) return;
        if (e.target.closest('.fab-cluster')) return;
        els.pop.hidden = true;
    });
})();
