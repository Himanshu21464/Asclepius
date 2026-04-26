// SPDX-License-Identifier: Apache-2.0
// Asclepius — in-browser demo runtime.
//
// A faithful (lossy) reimplementation of the C++ runtime's externally-
// observable behaviour, just enough to let a visitor wrap a clinical-AI
// inference end-to-end without leaving the page:
//
//   • PolicyChain          (PHI scrubber, schema validator, length limit)
//   • signed Merkle ledger (Web Crypto: SHA-256 + ECDSA P-256 / Ed25519)
//   • drift monitor        (PSI on a confidence histogram)
//   • consent token        (in-memory)
//   • evidence bundle      (downloadable JSON manifest with signature)
//
// Hash function differs from the C++ runtime (SHA-256 here vs BLAKE2b-256
// there) because Web Crypto does not ship BLAKE2b. Everything else is
// shape-compatible: an entry's `prev_hash` chains, a manifest signs.

(() => {
    'use strict';

    // ── helpers ──────────────────────────────────────────────────────────
    const $ = (sel, root = document) => root.querySelector(sel);
    const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));
    const enc = new TextEncoder();
    const HEX = (buf) => Array.from(new Uint8Array(buf)).map((b) => b.toString(16).padStart(2, '0')).join('');
    const ZERO = '0'.repeat(64);
    const tail = (s, n = 8) => s.slice(0, 6) + '…' + s.slice(-n);
    const nowIso = () => new Date().toISOString();

    async function sha256(s) {
        const out = await crypto.subtle.digest('SHA-256', typeof s === 'string' ? enc.encode(s) : s);
        return HEX(out);
    }

    // ── policy chain ─────────────────────────────────────────────────────
    const PHI = [
        { re: /\b\d{3}-\d{2}-\d{4}\b/g,                                      cls: 'ssn'   },
        { re: /\b\(?\d{3}\)?[\s\-]?\d{3}[\s\-]?\d{4}\b/g,                    cls: 'phone' },
        { re: /\b[A-Z0-9._%+\-]+@[A-Z0-9.\-]+\.[A-Z]{2,}\b/gi,               cls: 'email' },
        { re: /\bMRN[:#\-\s]*\d{4,12}\b/gi,                                  cls: 'mrn'   },
        { re: /\b(0?[1-9]|1[0-2])[\/\-](0?[1-9]|[12]\d|3[01])[\/\-](19|20)\d{2}\b/g, cls: 'date' },
    ];

    function phiScrub(text) {
        let out = text;
        const hits = [];
        for (const p of PHI) {
            const matches = text.match(p.re);
            if (matches) {
                for (const m of matches) hits.push({ cls: p.cls, value: m });
                out = out.replace(p.re, `[REDACTED:${p.cls}]`);
            }
        }
        return { decision: hits.length ? 'modify' : 'allow', text: out, hits };
    }

    // a tiny subset JSON-Schema check: required keys, types, lengths
    function schemaCheck(schema, doc) {
        const errs = [];
        const walk = (s, d, path) => {
            if (s.type) {
                const ok =
                    (s.type === 'object'  && d && typeof d === 'object' && !Array.isArray(d)) ||
                    (s.type === 'array'   && Array.isArray(d)) ||
                    (s.type === 'string'  && typeof d === 'string') ||
                    (s.type === 'number'  && typeof d === 'number') ||
                    (s.type === 'integer' && Number.isInteger(d)) ||
                    (s.type === 'boolean' && typeof d === 'boolean');
                if (!ok) { errs.push(`${path||'$'}: expected ${s.type}`); return; }
            }
            if (s.required && Array.isArray(s.required)) {
                for (const k of s.required) if (!(k in (d||{}))) errs.push(`${path}/${k}: required`);
            }
            if (s.properties && d && typeof d === 'object') {
                for (const k of Object.keys(s.properties)) {
                    if (k in d) walk(s.properties[k], d[k], `${path}/${k}`);
                }
            }
            if (typeof d === 'string') {
                if (s.minLength != null && d.length < s.minLength) errs.push(`${path}: too short`);
                if (s.maxLength != null && d.length > s.maxLength) errs.push(`${path}: too long`);
            }
        };
        walk(schema, doc, '');
        return errs;
    }

    function lengthLimit(text, max) {
        return text.length <= max
            ? { decision: 'allow' }
            : { decision: 'block', rationale: `length ${text.length} exceeds ${max}` };
    }

    // ── runtime state ────────────────────────────────────────────────────
    const state = {
        ledger:  [],
        keyPair: null,
        keyId:   '',
        publicKeyHex: '',
        sigAlg:  'Ed25519',         // upgraded to ECDSA on fallback
        drift:   { ref: null, cur: null, baseline: null },
        consent: null,              // a single token for the demo
        tampered: false,
    };

    // ── crypto: prefer Ed25519, fall back to ECDSA P-256 ─────────────────
    async function initKey() {
        try {
            state.keyPair = await crypto.subtle.generateKey(
                { name: 'Ed25519' }, true, ['sign', 'verify']
            );
            state.sigAlg = 'Ed25519';
        } catch (_) {
            state.keyPair = await crypto.subtle.generateKey(
                { name: 'ECDSA', namedCurve: 'P-256' }, true, ['sign', 'verify']
            );
            state.sigAlg = 'ECDSA-P256';
        }
        const pkRaw = await crypto.subtle.exportKey('raw',
            state.sigAlg === 'Ed25519' ? state.keyPair.publicKey : state.keyPair.publicKey
        ).catch(async () => crypto.subtle.exportKey('spki', state.keyPair.publicKey));
        state.publicKeyHex = HEX(pkRaw);
        state.keyId = state.publicKeyHex.slice(0, 16);
        $('#kp-alg').textContent      = state.sigAlg;
        $('#kp-keyid').textContent    = state.keyId;
        $('#kp-status').textContent   = 'ready';
        $('#kp-status').classList.add('is-ok');
    }

    async function sign(bytes) {
        const algo = state.sigAlg === 'Ed25519'
            ? { name: 'Ed25519' }
            : { name: 'ECDSA', hash: { name: 'SHA-256' } };
        const sig = await crypto.subtle.sign(algo, state.keyPair.privateKey, bytes);
        return HEX(sig);
    }

    async function verify(bytes, sigHex) {
        const algo = state.sigAlg === 'Ed25519'
            ? { name: 'Ed25519' }
            : { name: 'ECDSA', hash: { name: 'SHA-256' } };
        const sigBytes = new Uint8Array(sigHex.match(/.{2}/g).map((b) => parseInt(b, 16)));
        return crypto.subtle.verify(algo, state.keyPair.publicKey, sigBytes, bytes);
    }

    // ── ledger append ────────────────────────────────────────────────────
    async function appendEntry(eventType, actor, body, tenant = '') {
        const seq          = state.ledger.length + 1;
        const ts           = nowIso();
        const prevHash     = state.ledger.length ? state.ledger[seq - 2].entryHash : ZERO;
        const bodyJson     = JSON.stringify(body, Object.keys(body).sort());
        const payloadHash  = await sha256(eventType + '\x1e' + bodyJson);
        const signInput    = enc.encode(`${seq}|${ts}|${prevHash}|${payloadHash}|${actor}|${eventType}|${tenant}|${bodyJson}`);
        const signature    = await sign(signInput);
        const entryHash    = await sha256(
            `${seq}|${ts}|${prevHash}|${payloadHash}|${actor}|${eventType}|${tenant}|${bodyJson}|${signature}|${state.keyId}`
        );
        const entry = { seq, ts, prevHash, payloadHash, actor, eventType, tenant, body, bodyJson, signature, entryHash };
        state.ledger.push(entry);
        return entry;
    }

    // ── verify the whole chain ───────────────────────────────────────────
    async function verifyChain() {
        let prev = ZERO;
        for (const e of state.ledger) {
            if (e.prevHash !== prev) return { ok: false, at: e.seq, reason: 'chain break' };
            const bodyJson = JSON.stringify(e.body, Object.keys(e.body).sort());
            const recomputedPayload = await sha256(e.eventType + '\x1e' + bodyJson);
            if (recomputedPayload !== e.payloadHash) return { ok: false, at: e.seq, reason: 'payload hash' };
            const signInput = enc.encode(`${e.seq}|${e.ts}|${e.prevHash}|${e.payloadHash}|${e.actor}|${e.eventType}|${e.tenant}|${bodyJson}`);
            const okSig = await verify(signInput, e.signature);
            if (!okSig) return { ok: false, at: e.seq, reason: 'signature' };
            const recomputedEntry = await sha256(
                `${e.seq}|${e.ts}|${e.prevHash}|${e.payloadHash}|${e.actor}|${e.eventType}|${e.tenant}|${bodyJson}|${e.signature}|${state.keyId}`
            );
            prev = recomputedEntry;
        }
        return { ok: true, n: state.ledger.length, head: prev };
    }

    // ── consent ──────────────────────────────────────────────────────────
    function grantConsent(patient, purpose, ttlSec = 3600) {
        const issuedAt  = Date.now();
        const expiresAt = issuedAt + ttlSec * 1000;
        const tokenId   = 'ct_' + issuedAt.toString(16) + '_' + Math.floor(Math.random()*0xffffffff).toString(16);
        state.consent   = { tokenId, patient, purposes: [purpose], issuedAt, expiresAt, revoked: false };
        return state.consent;
    }

    // ── the fake "model": produces a SOAP-shaped output ──────────────────
    function fakeScribe(transcript) {
        return {
            chief_complaint: 'chest pain on exertion',
            subjective:      transcript,
            objective:       'BP 132/84, HR 88, RR 16, SpO2 97%',
            assessment:      'atypical chest pain; r/o ACS',
            plan:            'ECG, troponin x2, GTN PRN, cards consult',
        };
    }

    // ── DOM rendering ────────────────────────────────────────────────────
    const SOAP_SCHEMA = {
        type: 'object',
        required: ['chief_complaint','subjective','assessment','plan'],
        properties: {
            chief_complaint: { type: 'string', minLength: 3,  maxLength: 200 },
            subjective:      { type: 'string', minLength: 1,  maxLength: 8000 },
            objective:       { type: 'string' },
            assessment:      { type: 'string', minLength: 1,  maxLength: 1000 },
            plan:            { type: 'string', minLength: 1,  maxLength: 1000 },
        },
    };

    function flashStep(el) {
        el.classList.remove('is-active', 'is-block', 'is-modify');
        void el.offsetWidth;          // force reflow so the animation re-runs
        el.classList.add('is-active');
    }

    function renderEntry(entry, parent) {
        const card = document.createElement('article');
        card.className = 'd-entry';
        card.dataset.seq = entry.seq;
        card.innerHTML = `
            <header class="d-entry__head">
                <span class="d-entry__seq">seq <b>${String(entry.seq).padStart(4,'0')}</b></span>
                <span class="d-entry__type">${entry.eventType}</span>
                <span class="d-entry__ts">${entry.ts.slice(11,19)}Z</span>
            </header>
            <dl class="d-entry__kv">
                <div><dt>actor</dt><dd>${escapeHtml(entry.actor)}</dd></div>
                <div><dt>tenant</dt><dd>${escapeHtml(entry.tenant) || '—'}</dd></div>
                <div><dt>prev_hash</dt><dd class="mono">${tail(entry.prevHash)}</dd></div>
                <div><dt>payload</dt><dd class="mono">${tail(entry.payloadHash)}</dd></div>
                <div><dt>signature</dt><dd class="mono">${tail(entry.signature, 10)}</dd></div>
                <div><dt>entry_hash</dt><dd class="mono">${tail(entry.entryHash)}</dd></div>
            </dl>
            <details class="d-entry__body">
                <summary>body · ${entry.bodyJson.length}&nbsp;bytes</summary>
                <pre>${escapeHtml(JSON.stringify(entry.body, null, 2))}</pre>
                <button class="d-tamper" data-seq="${entry.seq}">tamper with this entry →</button>
            </details>`;
        parent.prepend(card);
        // staggered reveal
        requestAnimationFrame(() => card.classList.add('is-in'));
    }

    function escapeHtml(s) {
        return String(s).replace(/[&<>"']/g, (c) => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
    }

    function renderPhi(text) {
        // produce a span-decorated version with PHI highlighted
        const out  = document.createElement('div');
        let cursor = 0;
        const all  = [];
        for (const p of PHI) {
            const re = new RegExp(p.re.source, p.re.flags);
            let m;
            while ((m = re.exec(text)) !== null) all.push({ s: m.index, e: m.index + m[0].length, cls: p.cls, raw: m[0] });
        }
        all.sort((a,b) => a.s - b.s);
        const merged = [];
        for (const m of all) {
            if (merged.length && m.s < merged[merged.length-1].e) continue;     // skip overlap
            merged.push(m);
        }
        for (const m of merged) {
            if (m.s > cursor) out.append(document.createTextNode(text.slice(cursor, m.s)));
            const sp = document.createElement('span');
            sp.className   = `phi-hit phi-hit--${m.cls}`;
            sp.textContent = m.raw;
            sp.dataset.cls = m.cls;
            out.appendChild(sp);
            cursor = m.e;
        }
        if (cursor < text.length) out.append(document.createTextNode(text.slice(cursor)));
        return { html: out.innerHTML, hits: merged };
    }

    // ── public bound to UI ───────────────────────────────────────────────
    async function onWrap() {
        const transcript = $('#d-input').value;
        const steps = $$('.d-pipe .d-step');
        steps.forEach((s) => s.classList.remove('is-active','is-block','is-modify','is-allow'));
        $('#d-output').textContent = '';
        $('#d-status').textContent = 'wrapping…';
        $('#d-status').className   = 'd-status is-pending';

        // step 01 — consent
        flashStep(steps[0]); await sleep(160);
        if (!state.consent || state.consent.expiresAt < Date.now() || state.consent.revoked) {
            steps[0].classList.add('is-block');
            $('#d-status').textContent = 'blocked: consent missing';
            $('#d-status').className   = 'd-status is-block';
            return;
        }
        steps[0].classList.add('is-allow');

        // step 02 — input policies
        flashStep(steps[1]); await sleep(180);
        const phi   = phiScrub(transcript);
        const lim   = lengthLimit(transcript, 8000);
        if (lim.decision === 'block') {
            steps[1].classList.add('is-block');
            $('#d-status').textContent = 'blocked: ' + lim.rationale;
            $('#d-status').className   = 'd-status is-block';
            return;
        }
        steps[1].classList.add(phi.decision === 'modify' ? 'is-modify' : 'is-allow');
        const inputForModel = phi.text;
        $('#d-redactions').innerHTML =
            phi.hits.length
                ? phi.hits.map((h) => `<li><b>${h.cls}</b><span class="mono">${escapeHtml(h.value)}</span></li>`).join('')
                : '<li class="empty">no PHI detected</li>';

        // step 03 — model call
        flashStep(steps[2]); await sleep(220);
        const out = fakeScribe(inputForModel);
        steps[2].classList.add('is-allow');
        $('#d-output').textContent = JSON.stringify(out, null, 2);

        // step 04 — output policies
        flashStep(steps[3]); await sleep(180);
        const errs = schemaCheck(SOAP_SCHEMA, out);
        if (errs.length) {
            steps[3].classList.add('is-block');
            $('#d-status').textContent = 'blocked output: ' + errs[0];
            $('#d-status').className   = 'd-status is-block';
            return;
        }
        steps[3].classList.add('is-allow');

        // step 05 — commit
        flashStep(steps[4]); await sleep(160);
        const inHash  = await sha256(inputForModel);
        const outHash = await sha256(JSON.stringify(out));
        const entry = await appendEntry('inference.committed', 'clinician:dr.smith', {
            inference_id:    'inf_' + Date.now().toString(16),
            model:           'scribe@v3',
            actor:           'clinician:dr.smith',
            patient:         state.consent.patient,
            encounter:       'enc:' + Math.random().toString(36).slice(2,10).toUpperCase(),
            purpose:         'ambient_documentation',
            consent_token_id:state.consent.tokenId,
            input_hash:      inHash,
            output_hash:     outHash,
            status:          'ok',
            redactions:      phi.hits.map((h) => h.cls),
        }, 'kp-northwest');
        steps[4].classList.add('is-allow');
        renderEntry(entry, $('#d-ledger'));
        $('#d-status').textContent = 'ok · committed seq ' + entry.seq;
        $('#d-status').className   = 'd-status is-ok';
        $('#d-len').textContent    = state.ledger.length;
        $('#d-head').textContent   = tail(entry.entryHash);

        // synthesize a confidence value that gradually drifts away from
        // baseline so PSI grows realistically as the user wraps repeatedly.
        const phase = state.ledger.length;
        const mean  = Math.max(0.30, 0.70 - 0.025 * phase);
        const noise = (Math.random() - 0.5) * 0.10;     // ±5%
        observeDrift(Math.max(0.0, Math.min(1.0, mean + noise)));

        // refresh the verify pill — every commit changes the head.
        await onVerify();
    }

    function observeDrift(value) {
        if (!state.drift.ref) return;
        const idx = Math.min(19, Math.max(0, Math.floor(value * 20)));
        state.drift.cur[idx]++;
        renderDrift();
    }

    function renderDrift() {
        const ref = state.drift.ref, cur = state.drift.cur;
        const refSum = ref.reduce((a,b) => a+b, 0);
        const curSum = cur.reduce((a,b) => a+b, 0);
        const eps = 1e-6;

        // PSI is undefined until we have at least a handful of current
        // observations — emit "—" rather than blow up on an empty cur.
        let psi = 0;
        if (curSum >= 5) {
            for (let i = 0; i < 20; i++) {
                const p = Math.max(ref[i]/refSum, eps);
                const q = Math.max(cur[i]/curSum, eps);
                psi += (p - q) * Math.log(p / q);
            }
        }
        $('#d-psi').textContent = curSum >= 5 ? psi.toFixed(3) : '—';
        $('#d-psi-bar').style.setProperty('--psi', Math.min(1, psi/0.5));
        const sev = curSum < 5      ? 'none'
                   : psi < 0.10     ? 'none'
                   : psi < 0.25     ? 'minor'
                   : psi < 0.50     ? 'moder'
                                    : 'severe';
        const el = $('#d-severity');
        el.textContent = curSum < 5 ? 'awaiting data' : sev;
        el.className   = 'd-severity is-' + sev;
        // render bars
        const bars = $('#d-bars');
        if (bars) {
            bars.innerHTML = cur.map((c, i) => {
                const refBar = ref[i] / Math.max(...ref);
                const curBar = c     / Math.max(...cur, 1);
                return `<div class="d-bar"><i class="d-bar__ref" style="--h:${refBar*100}%"></i><i class="d-bar__cur" style="--h:${curBar*100}%"></i></div>`;
            }).join('');
        }
    }

    function initDriftBaseline() {
        // baseline: gentle bell around bin 14 (~0.7)
        const hist = new Array(20).fill(0);
        for (let i = 0; i < 20; i++) {
            const d = i - 14;
            hist[i] = Math.round(120 * Math.exp(-(d*d)/8));
        }
        state.drift.baseline = hist.slice();
        state.drift.ref      = hist;
        state.drift.cur      = new Array(20).fill(0);
    }

    async function onTamper(seq) {
        const entry = state.ledger.find((e) => e.seq === Number(seq));
        if (!entry) return;

        // capture before/after so we can diff in the UI
        const before = entry.body.status || entry.body.purpose || 'tampered';
        const after  = before.endsWith('X')
                         ? before.slice(0, -1) + '!'
                         : before + 'X';

        // mutate exactly one byte: prefer status, then purpose, else add a key
        if      ('status'  in entry.body) entry.body.status  = after;
        else if ('purpose' in entry.body) entry.body.purpose = after;
        else                              entry.body.tampered = true;

        entry.bodyJson  = JSON.stringify(entry.body, Object.keys(entry.body).sort());
        state.tampered  = true;
        renderTamperDiff(seq, before, after);
        await onVerify();
    }

    function renderTamperDiff(seq, before, after) {
        const card = document.querySelector(`.d-entry[data-seq="${seq}"]`);
        if (!card) return;
        // open the body so the diff is visible
        const det = card.querySelector('.d-entry__body');
        if (det) det.open = true;

        let diff = card.querySelector('.d-tamper-diff');
        if (!diff) {
            diff = document.createElement('div');
            diff.className = 'd-tamper-diff';
            const tamperBtn = card.querySelector('.d-tamper');
            if (tamperBtn) tamperBtn.parentNode.insertBefore(diff, tamperBtn);
            else card.appendChild(diff);
        }
        diff.innerHTML = `
            <div class="d-tamper-diff__head">
                <span class="d-tamper-diff__icon">⚠</span>
                <span><b>Tampered</b> · this entry's body was modified after signing. The chain detects it because the recomputed payload hash no longer matches the one in the header — and that one was signed.</span>
            </div>
            <div class="d-tamper-diff__rows">
                <div class="d-tamper-diff__row d-tamper-diff__row--old">
                    <span class="d-tamper-diff__label">before</span>
                    <code>${escapeHtml(before)}</code>
                </div>
                <div class="d-tamper-diff__row d-tamper-diff__row--new">
                    <span class="d-tamper-diff__label">after</span>
                    <code>${escapeHtml(after)}</code>
                </div>
            </div>`;
    }

    async function onVerify() {
        const v = await verifyChain();
        const el = $('#d-verify');
        if (v.ok) {
            el.textContent = `OK · ${v.n} entries · head ${tail(v.head)}`;
            el.className   = 'd-verify is-ok';
        } else {
            el.textContent = `INVALID at seq ${v.at}: ${v.reason}`;
            el.className   = 'd-verify is-bad';
        }
    }

    function onExport() {
        const head = state.ledger.length ? state.ledger[state.ledger.length-1].entryHash : ZERO;
        const manifest = {
            asclepius_version: '0.1.0-demo',
            window: { start: state.ledger[0]?.ts || nowIso(), end: nowIso() },
            ledger_head: head,
            key_id:      state.keyId,
            sig_alg:     state.sigAlg,
            entries:     state.ledger.map(({ seq, ts, eventType, payloadHash, prevHash, entryHash, signature, body }) => ({
                seq, ts, event_type: eventType, payload_hash: payloadHash, prev_hash: prevHash, entry_hash: entryHash, signature, body,
            })),
            note: 'Browser demo — hash function is SHA-256, not BLAKE2b-256. Shape-compatible; not interoperable with the C++ runtime.',
        };
        const blob = new Blob([JSON.stringify(manifest, null, 2)], { type: 'application/json' });
        const a    = document.createElement('a');
        a.href     = URL.createObjectURL(blob);
        a.download = 'asclepius-evidence.json';
        a.click();
        URL.revokeObjectURL(a.href);
    }

    // ── live PHI preview as the user types ──────────────────────────────
    function refreshPhiPreview() {
        const txt = $('#d-input').value;
        const { html, hits } = renderPhi(txt);
        $('#d-input-preview').innerHTML = html || '<span class="ink-mute">(empty)</span>';
        $('#d-phi-count').textContent   = hits.length;
    }

    function sleep(ms) { return new Promise((r) => setTimeout(r, ms)); }

    // ── boot ────────────────────────────────────────────────────────────
    document.addEventListener('DOMContentLoaded', async () => {
        if (!$('#d-app')) return;             // not on the demo page
        $('#d-input').value = `Patient John Doe MRN:12345678 reports 30 minutes of substernal chest pressure radiating to the left arm. Started during morning jog 03/14/2026. Phone (415) 555-1234. Email john.doe@example.com. SSN 123-45-6789. Denies dyspnea, nausea, diaphoresis. Past medical history of hypertension on lisinopril.`;

        await initKey();
        initDriftBaseline();
        renderDrift();

        const tok = grantConsent('pat:p_demo_9f1a', 'ambient_documentation', 3600);
        $('#d-consent').textContent = tok.tokenId;

        $('#d-input').addEventListener('input', refreshPhiPreview);
        refreshPhiPreview();

        $('#d-wrap').addEventListener('click', onWrap);
        $('#d-verify-btn').addEventListener('click', onVerify);
        $('#d-export').addEventListener('click', onExport);
        $('#d-clear').addEventListener('click', () => {
            state.ledger    = [];
            state.drift.cur = new Array(20).fill(0);
            $('#d-ledger').innerHTML = '';
            $('#d-len').textContent  = '0';
            $('#d-head').textContent = '0…0';
            renderDrift();
            onVerify();
        });

        $('#d-ledger').addEventListener('click', (e) => {
            const t = e.target.closest('.d-tamper');
            if (t) onTamper(t.dataset.seq);
        });

        // initial verify
        onVerify();
    });
})();
