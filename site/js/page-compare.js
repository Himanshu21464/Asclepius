(() => {
    'use strict';
    const $ = (s) => document.querySelector(s);
    const enc = new TextEncoder();
    const HEX = (buf) => Array.from(new Uint8Array(buf)).map((b) => b.toString(16).padStart(2, '0')).join('');

    function parseJsonl(text) {
        const out = [];
        for (const [i, line] of text.split('\n').entries()) {
            const t = line.trim();
            if (!t) continue;
            try {
                const e = JSON.parse(t);
                e.__line = i + 1;
                if (typeof e.seq === 'number') out.push(e);
            } catch (_) { /* skip malformed */ }
        }
        out.sort((a, b) => a.seq - b.seq);
        return out;
    }

    function tail(s, n = 8) {
        if (!s) return '—';
        if (s.length <= 16) return s;
        return s.slice(0, 6) + '…' + s.slice(-n);
    }

    function escapeHtml(s) {
        return String(s).replace(/[&<>"']/g, (c) => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
    }

    async function buildSampleA() {
        // 4 entries
        const out = [];
        let prev = '0'.repeat(64);
        const events = [
            { event_type: 'inference.committed',  body: { model:'scribe@v3', patient:'pat:9f1a', purpose:'ambient_documentation' }},
            { event_type: 'evaluation.override',  body: { inference_id:'inf_a14', rationale:'corrected to stable angina' }},
            { event_type: 'inference.committed',  body: { model:'diag@v2', patient:'pat:b771', purpose:'diagnostic_suggestion' }},
            { event_type: 'evaluation.ground_truth', body: { inference_id:'inf_a14', truth:{label:'stable angina'}, source:'chart' }},
        ];
        for (let i = 0; i < events.length; i++) {
            const ts = `2026-04-01T08:1${i}:00Z`;
            const ph = await sha(`${events[i].event_type}\x1e${cj(events[i].body)}`);
            const entry = { seq: i+1, ts, actor: 'clinician:dr.smith', tenant: 'kp-northwest',
                            event_type: events[i].event_type, prev_hash: prev, payload_hash: ph,
                            body: events[i].body };
            const eh = await sha(`${entry.seq}|${entry.ts}|${entry.prev_hash}|${entry.payload_hash}|${entry.actor}|${entry.event_type}|${entry.tenant}|${cj(entry.body)}`);
            entry.entry_hash = eh;
            out.push(JSON.stringify(entry));
            prev = eh;
        }
        return out.join('\n');
    }

    async function buildSampleB() {
        // sample A + retroactive edit on seq 2 + 2 new entries
        const a = await buildSampleA();
        const lines = a.split('\n');
        // mutate seq 2's body — retroactive edit
        const seq2 = JSON.parse(lines[1]);
        seq2.body.rationale = 'corrected after a phone call from cardiology';
        // re-hash payload to reflect the new body (this would normally be re-signed)
        seq2.payload_hash = await sha(`${seq2.event_type}\x1e${cj(seq2.body)}`);
        lines[1] = JSON.stringify(seq2);
        // append two new rows
        let prev = JSON.parse(lines[lines.length - 1]).entry_hash;
        for (let k = 5; k <= 6; k++) {
            const body = { model: 'scribe@v3', patient: `pat:c${k}90`, purpose: 'ambient_documentation' };
            const ts = `2026-04-02T0${k - 4}:30:00Z`;
            const ph = await sha(`inference.committed\x1e${cj(body)}`);
            const entry = { seq: k, ts, actor: 'clinician:dr.smith', tenant: 'kp-northwest',
                            event_type: 'inference.committed', prev_hash: prev, payload_hash: ph, body };
            const eh = await sha(`${entry.seq}|${entry.ts}|${entry.prev_hash}|${entry.payload_hash}|${entry.actor}|${entry.event_type}|${entry.tenant}|${cj(entry.body)}`);
            entry.entry_hash = eh;
            lines.push(JSON.stringify(entry));
            prev = eh;
        }
        return lines.join('\n');
    }

    async function sha(s) {
        const buf = await crypto.subtle.digest('SHA-256', enc.encode(s));
        return HEX(buf);
    }
    function cj(o) { return JSON.stringify(o, Object.keys(o).sort()); }

    function diff(left, right) {
        const seqs = new Set([...left.map((e) => e.seq), ...right.map((e) => e.seq)]);
        const lBy = new Map(left.map((e) => [e.seq, e]));
        const rBy = new Map(right.map((e) => [e.seq, e]));
        const rows = [];
        for (const seq of [...seqs].sort((a, b) => a - b)) {
            const l = lBy.get(seq), r = rBy.get(seq);
            if (l && r) {
                if (l.payload_hash === r.payload_hash) rows.push({ seq, kind: 'same', l, r });
                else                                    rows.push({ seq, kind: 'mod',  l, r });
            } else if (r) rows.push({ seq, kind: 'add', r });
            else if (l)   rows.push({ seq, kind: 'rem', l });
        }
        return rows;
    }

    function render(rows) {
        const list = $('#cm-rows');
        const counts = { add: 0, rem: 0, mod: 0, same: 0 };
        list.innerHTML = rows.map((row) => {
            counts[row.kind]++;
            const VERB = { add: '+ added', rem: '− removed', mod: '~ modified', same: '= unchanged' }[row.kind];
            const type = (row.r || row.l).event_type;
            const lHash = row.l ? tail(row.l.payload_hash) : '—';
            const rHash = row.r ? tail(row.r.payload_hash) : '—';
            const icon = { add: '+', rem: '−', mod: '~', same: '·' }[row.kind];
            return `<div class="cm-row is-${row.kind}">
                <span class="cm-row__seq">${row.seq}</span>
                <span class="cm-row__verdict">${VERB}</span>
                <span class="cm-row__type">${escapeHtml(type)}</span>
                <span class="cm-row__l" title="L payload">${lHash}</span>
                <span class="cm-row__r" title="R payload">${rHash}</span>
                <span class="cm-row__icon">${icon}</span>
            </div>`;
        }).join('');
        $('#cm-summary').innerHTML = `
            <span class="same"><b>${counts.same}</b> unchanged</span>
            <span class="add"><b>${counts.add}</b> added</span>
            <span class="rem"><b>${counts.rem}</b> removed</span>
            <span class="mod"><b>${counts.mod}</b> modified</span>`;
    }

    async function go() {
        const left  = parseJsonl($('#cm-left-input').value);
        const right = parseJsonl($('#cm-right-input').value);
        if (!left.length && !right.length) {
            $('#cm-summary').innerHTML = '<span class="same">drop two bundles · click <b>diff</b> to compare</span>';
            $('#cm-rows').innerHTML = '';
            return;
        }
        const rows = diff(left, right);
        render(rows);
    }

    $('#cm-go').addEventListener('click', go);

    // button handlers
    document.querySelectorAll('.cm-btn').forEach((b) => {
        b.addEventListener('click', async () => {
            const side = b.dataset.side;
            const ta = $(`#cm-${side}-input`);
            switch (b.dataset.act) {
                case 'clear':  ta.value = ''; await go(); break;
                case 'sample': ta.value = side === 'left' ? await buildSampleA() : await buildSampleB(); await go(); break;
                case 'load':   $(`#cm-file-${side === 'left' ? 'l' : 'r'}`).click(); break;
            }
        });
    });

    // file pickers
    for (const which of ['l', 'r']) {
        $(`#cm-file-${which}`).addEventListener('change', async (e) => {
            const file = e.target.files[0];
            if (!file) return;
            const text = await file.text();
            $(`#cm-${which === 'l' ? 'left' : 'right'}-input`).value = text;
            await go();
        });
    }

    // drag-and-drop on each pane
    for (const side of ['left', 'right']) {
        const pane = $(`#cm-${side}`);
        ['dragenter', 'dragover'].forEach((ev) => {
            pane.addEventListener(ev, (e) => { e.preventDefault(); pane.classList.add('is-drop'); });
        });
        ['dragleave', 'drop'].forEach((ev) => {
            pane.addEventListener(ev, (e) => { e.preventDefault(); pane.classList.remove('is-drop'); });
        });
        pane.addEventListener('drop', async (e) => {
            const file = e.dataTransfer.files[0];
            if (!file) return;
            $(`#cm-${side}-input`).value = await file.text();
            await go();
        });
    }

    // re-diff on input (debounced)
    let t = null;
    for (const side of ['left', 'right']) {
        $(`#cm-${side}-input`).addEventListener('input', () => {
            clearTimeout(t);
            t = setTimeout(go, 280);
        });
    }
})();
