// SPDX-License-Identifier: Apache-2.0
// Asclepius — site self-attestation.
//
// Demonstrates the substrate's thesis at the site layer: every static
// asset has a published SHA-256 in /asset-manifest.json, and any visitor
// can verify the bytes they actually received against that manifest, in
// their own browser, without trusting our servers any further.
//
// Adds a small inline pill to every site footer; clicking it expands an
// inline result panel.

(() => {
    'use strict';

    // skip if we're inside a nested context (unlikely) or no <footer>
    const foot = document.querySelector('footer.foot, footer');
    if (!foot) return;
    if (foot.querySelector('.site-attest')) return;

    const enc = new TextEncoder();
    const HEX = (buf) =>
        Array.from(new Uint8Array(buf)).map((b) => b.toString(16).padStart(2, '0')).join('');

    const wrap = document.createElement('div');
    wrap.className = 'site-attest';
    wrap.innerHTML = `
        <button type="button" class="site-attest__btn" aria-expanded="false">
            <span class="site-attest__dot" aria-hidden="true"></span>
            <b>verify this site</b>
            <i>self-attestation · sha-256</i>
        </button>
        <div class="site-attest__panel" hidden></div>`;
    foot.appendChild(wrap);

    const btn   = wrap.querySelector('.site-attest__btn');
    const panel = wrap.querySelector('.site-attest__panel');
    const dot   = wrap.querySelector('.site-attest__dot');

    let started = false;

    btn.addEventListener('click', async () => {
        const open = btn.getAttribute('aria-expanded') === 'true';
        btn.setAttribute('aria-expanded', String(!open));
        panel.hidden = open;
        if (!open && !started) {
            started = true;
            await runVerification();
        }
    });

    async function fetchHash(url) {
        const res = await fetch(url, { cache: 'no-store' });
        if (!res.ok) throw new Error(res.status);
        const buf = await res.arrayBuffer();
        const dig = await crypto.subtle.digest('SHA-256', buf);
        return HEX(dig);
    }

    async function runVerification() {
        panel.innerHTML = '<p class="site-attest__loading">fetching <code>asset-manifest.json</code>…</p>';

        let manifest;
        try {
            const res = await fetch('asset-manifest.json', { cache: 'no-store' });
            manifest = await res.json();
        } catch (e) {
            panel.innerHTML = `<p class="site-attest__err">cannot fetch manifest: ${e.message || e}</p>`;
            dot.classList.add('is-err');
            return;
        }

        const total  = Object.keys(manifest.files).length;
        let   ok     = 0;
        let   failed = [];

        // Verify a representative subset (first 12) so we don't hammer the
        // server. The full set is verifiable on demand by clicking an item.
        const sample = Object.entries(manifest.files).slice(0, 12);

        panel.innerHTML = `
            <header>
                <span>verifying <b>${sample.length}</b> of ${total} assets · sha-256</span>
                <code id="site-attest-progress">0 / ${sample.length}</code>
            </header>
            <ul id="site-attest-list"></ul>
            <footer>
                <span>generated ${manifest.generated_at}</span>
                <a href="asset-manifest.json">manifest&nbsp;↗</a>
            </footer>`;

        const list = panel.querySelector('#site-attest-list');
        const prog = panel.querySelector('#site-attest-progress');

        for (const [path, meta] of sample) {
            const li = document.createElement('li');
            li.innerHTML = `<span class="site-attest__path">${path}</span>
                            <span class="site-attest__hash">computing…</span>
                            <span class="site-attest__verdict">·</span>`;
            list.append(li);
            try {
                const got = await fetchHash(path);
                const match = got === meta.sha256;
                if (match) ok++; else failed.push(path);
                li.querySelector('.site-attest__hash').textContent =
                    got.slice(0, 8) + '…' + got.slice(-6);
                const verdict = li.querySelector('.site-attest__verdict');
                verdict.textContent = match ? '✓' : '✗';
                verdict.classList.add(match ? 'is-ok' : 'is-bad');
            } catch (e) {
                failed.push(path);
                const v = li.querySelector('.site-attest__verdict');
                v.textContent = '!';
                v.classList.add('is-bad');
            }
            prog.textContent = `${ok + failed.length} / ${sample.length}`;
        }

        const all = ok === sample.length;
        dot.classList.add(all ? 'is-ok' : 'is-bad');

        const summary = document.createElement('p');
        summary.className = 'site-attest__summary ' + (all ? 'is-ok' : 'is-bad');
        summary.innerHTML = all
            ? `<b>OK</b> · ${ok} / ${sample.length} matched · the bytes you received are the bytes the project published.`
            : `<b>MISMATCH</b> · ${failed.length} files differ from the manifest. Tampering or a stale cache.`;
        panel.append(summary);
    }
})();
