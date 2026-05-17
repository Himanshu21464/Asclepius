// Ledger Explorer — drop or paste JSONL, parse + chain-verify in-browser.
(() => {
    'use strict';
    const $ = (s) => document.querySelector(s);

    const SAMPLE = [
        {seq:1, ts:"2026-04-01T08:14:02.103Z", actor:"clinician:dr.smith", event_type:"inference.committed", tenant:"kp-northwest",
         prev_hash:"0".repeat(64), payload_hash:"", body:{model:"scribe@v3", patient:"pat:9f1a", purpose:"ambient_documentation", status:"ok"}},
        {seq:2, ts:"2026-04-01T08:14:11.802Z", actor:"clinician:dr.smith", event_type:"evaluation.override", tenant:"kp-northwest",
         prev_hash:"", payload_hash:"", body:{inference_id:"inf_a14", rationale:"clinician revised assessment after additional history"}},
        {seq:3, ts:"2026-04-01T08:15:44.501Z", actor:"system:ehr-bridge", event_type:"evaluation.ground_truth", tenant:"kp-northwest",
         prev_hash:"", payload_hash:"", body:{inference_id:"inf_a14", truth:{label:"stable angina"}, source:"chart-followup"}},
    ];

    const enc = new TextEncoder();
    const HEX = (buf) => Array.from(new Uint8Array(buf)).map((b) => b.toString(16).padStart(2, '0')).join('');

    async function sha256(s) {
        const buf = await crypto.subtle.digest('SHA-256', typeof s === 'string' ? enc.encode(s) : s);
        return HEX(buf);
    }

    function canonical(obj) {
        // sorted-keys, no whitespace
        return JSON.stringify(obj, Object.keys(obj).sort());
    }

    async function buildSample() {
        // produce a coherent sample with real prev/payload hashes
        const out = [];
        let prev = '0'.repeat(64);
        for (const e of SAMPLE) {
            e.prev_hash = prev;
            e.payload_hash = await sha256(e.event_type + '\x1e' + canonical(e.body));
            // synth entry_hash — toy: hash everything together
            prev = await sha256(`${e.seq}|${e.ts}|${e.prev_hash}|${e.payload_hash}|${e.actor}|${e.event_type}|${e.tenant}|${canonical(e.body)}`);
            out.push(JSON.stringify(e));
        }
        return out.join('\n');
    }

    function tail(s, n = 8) {
        if (!s) return '—';
        if (s.length <= 16) return s;
        return s.slice(0, 6) + '…' + s.slice(-n);
    }

    async function verify(text) {
        const lines = text.split('\n').map((l) => l.trim()).filter(Boolean);
        const entries = [];
        const errors = [];
        for (const [i, l] of lines.entries()) {
            try {
                const e = JSON.parse(l);
                e.__line = i + 1;
                entries.push(e);
            } catch (err) {
                errors.push({ line: i + 1, error: 'parse: ' + err.message });
            }
        }

        // walk chain
        let prev = entries[0]?.prev_hash || '';
        let chainOk = 0, chainBad = 0;
        let payloadOk = 0, payloadBad = 0;
        for (const [i, e] of entries.entries()) {
            // chain
            if (i === 0) {
                e.__chainOk = true;            // first entry's prev_hash unconstrained here
                chainOk++;
            } else if (e.prev_hash === entries[i-1].entry_hash || e.prev_hash === entries[i-1].__entryHash) {
                e.__chainOk = true; chainOk++;
            } else {
                // we don't have the operator's entry_hash — accept if prev_hash linked
                // to *some* prior payload_hash (toy chain via payload_hash)
                e.__chainOk = (e.prev_hash === entries[i-1].payload_hash) || false;
                if (e.__chainOk) chainOk++; else chainBad++;
            }
            // payload-hash recompute
            if (e.event_type && e.body) {
                const recomputed = await sha256(e.event_type + '\x1e' + canonical(e.body));
                e.__recomputed = recomputed;
                e.__payloadOk = recomputed === e.payload_hash;
                if (e.__payloadOk) payloadOk++; else payloadBad++;
            } else {
                e.__payloadOk = null;
            }
        }
        return { entries, errors, chainOk, chainBad, payloadOk, payloadBad };
    }

    function renderSummary(r) {
        $('#ex-summary').hidden = false;
        const types = new Set(r.entries.map((e) => e.event_type).filter(Boolean));
        const span = (() => {
            if (!r.entries.length) return '—';
            const ts = r.entries.map((e) => e.ts).filter(Boolean).sort();
            if (ts.length < 2) return ts[0] || '—';
            const first = new Date(ts[0]); const last = new Date(ts[ts.length-1]);
            return Math.round((last - first) / 1000) + ' s';
        })();
        $('#ex-stat-entries b').innerHTML       = `<em>${r.entries.length}</em>`;
        $('#ex-stat-types b').innerHTML         = `<em>${types.size}</em>`;
        const chainEl = $('#ex-stat-chain');
        chainEl.classList.toggle('is-ok',  r.chainBad === 0 && r.entries.length);
        chainEl.classList.toggle('is-bad', r.chainBad > 0);
        chainEl.querySelector('b').innerHTML = r.chainBad === 0
            ? `<em>OK</em>`
            : `<em>${r.chainBad} broken</em>`;
        const payloadEl = $('#ex-stat-payload');
        payloadEl.classList.toggle('is-ok',  r.payloadBad === 0 && r.payloadOk > 0);
        payloadEl.classList.toggle('is-bad', r.payloadBad > 0);
        payloadEl.querySelector('b').innerHTML = r.payloadBad === 0 && r.payloadOk
            ? `<em>${r.payloadOk}/${r.payloadOk}</em>`
            : `<em>${r.payloadBad} mismatch</em>`;
        $('#ex-stat-window b').innerHTML        = `<em>${span}</em>`;
    }

    function renderRows(r) {
        const list = $('#ex-list');
        list.innerHTML = '';
        if (r.errors.length) {
            for (const err of r.errors) {
                const li = document.createElement('div');
                li.className = 'ex-row is-bad';
                li.innerHTML = `<span class="ex-row__seq">l${err.line}</span><span class="ex-row__type">${err.error}</span><span class="ex-row__hash">—</span><span class="ex-row__prev">—</span><span class="ex-row__time">—</span><span class="ex-row__verdict is-bad">✗</span>`;
                list.append(li);
            }
        }
        for (const e of r.entries) {
            const row = document.createElement('details');
            row.className = 'ex-row';
            const ok = e.__chainOk && (e.__payloadOk || e.__payloadOk === null);
            if (!ok) row.classList.add('is-bad');
            row.innerHTML = `
                <summary style="display:contents;">
                    <span class="ex-row__seq">${e.seq ?? '—'}</span>
                    <span class="ex-row__type">${escapeHtml(e.event_type || '—')}</span>
                    <span class="ex-row__hash">${tail(e.payload_hash)}</span>
                    <span class="ex-row__prev">${tail(e.prev_hash)}</span>
                    <span class="ex-row__time">${(e.ts || '').slice(11, 19) || '—'}</span>
                    <span class="ex-row__verdict ${ok ? 'is-ok' : 'is-bad'}">${ok ? '✓' : '✗'}</span>
                </summary>
                <pre class="ex-row__detail">${escapeHtml(JSON.stringify(e, (k, v) => k.startsWith('__') ? undefined : v, 2))}${e.__recomputed && !e.__payloadOk ? `\n\nrecomputed payload: ${e.__recomputed}\ndeclared payload:   ${e.payload_hash}` : ''}</pre>`;
            list.append(row);
        }
    }

    function escapeHtml(s) {
        return String(s).replace(/[&<>"']/g, (c) => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
    }

    async function go() {
        const text = $('#ex-input').value.trim();
        if (!text) {
            $('#ex-summary').hidden = true;
            $('#ex-list').innerHTML = '';
            return;
        }
        const r = await verify(text);
        renderSummary(r);
        renderRows(r);
    }

    $('#ex-verify').addEventListener('click', go);
    $('#ex-clear').addEventListener('click', () => {
        $('#ex-input').value = '';
        $('#ex-summary').hidden = true;
        $('#ex-list').innerHTML = '';
    });
    $('#ex-sample').addEventListener('click', async () => {
        $('#ex-input').value = await buildSample();
        await go();
    });
    $('#ex-load').addEventListener('click', () => $('#ex-file').click());
    $('#ex-file').addEventListener('change', async (e) => {
        const file = e.target.files[0];
        if (!file) return;
        $('#ex-input').value = await file.text();
        await go();
    });

    // drag-and-drop
    const pad = $('#ex-pad');
    ['dragenter', 'dragover'].forEach((ev) => {
        pad.addEventListener(ev, (e) => { e.preventDefault(); pad.classList.add('is-drop'); });
    });
    ['dragleave', 'drop'].forEach((ev) => {
        pad.addEventListener(ev, (e) => { e.preventDefault(); pad.classList.remove('is-drop'); });
    });
    pad.addEventListener('drop', async (e) => {
        const f = e.dataTransfer.files[0];
        if (!f) return;
        $('#ex-input').value = await f.text();
        await go();
    });

    // re-verify on input (debounced)
    let t = null;
    $('#ex-input').addEventListener('input', () => {
        clearTimeout(t);
        t = setTimeout(go, 250);
    });
})();
