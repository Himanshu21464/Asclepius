(() => {
    'use strict';
    const $ = (s) => document.querySelector(s);

    // 8 synthetic encounters covering common policy scenarios
    const ENCOUNTERS = [
        { id: 'e1', input: 'Patient John Doe MRN:12345678 reports chest pain on exertion.', output: {chief_complaint:'chest pain', subjective:'30 min pressure', assessment:'r/o ACS', plan:'ECG, troponin'} },
        { id: 'e2', input: 'Phone (415) 555-1234 to coordinate care.', output: {chief_complaint:'phone follow-up', plan:'call back next week'} },
        { id: 'e3', input: 'No PHI here, just routine vitals: BP 132/84, HR 88, SpO2 97%.', output: {chief_complaint:'routine', plan:'discharge'} },
        { id: 'e4', input: 'SSN 123-45-6789 listed in chart by mistake.', output: {chief_complaint:'data quality', plan:'remove SSN from note'} },
        { id: 'e5', input: 'Email jane.doe@example.com pending consent.', output: {chief_complaint:'pending consent'} },
        { id: 'e6', input: 'Date of injury 03/14/2026.', output: {chief_complaint:'sprain', subjective:'tripped at work', assessment:'left ankle sprain', plan:'RICE'} },
        { id: 'e7', input: 'Clean encounter, just a wellness check.', output: {} },
        { id: 'e8', input: 'Cardiology referral for arrhythmia — no PHI.', output: {chief_complaint:'arrhythmia referral', subjective:'palpitations', assessment:'r/o AFib', plan:'24h Holter'} },
    ];

    // ── presets ───────────────────────────────────────────────────────────
    const PRESETS = {
        regex: {
            'phi (default)': '\\b\\d{3}-\\d{2}-\\d{4}\\b\n\\b\\(?\\d{3}\\)?[\\s\\-]?\\d{3}[\\s\\-]?\\d{4}\\b\n\\b[A-Z0-9._%+\\-]+@[A-Z0-9.\\-]+\\.[A-Z]{2,}\\b\n\\bMRN[:#\\-\\s]*\\d{4,12}\\b\n\\b(0?[1-9]|1[0-2])[/\\-](0?[1-9]|[12]\\d|3[01])[/\\-](19|20)\\d{2}\\b',
            'just SSN':       '\\b\\d{3}-\\d{2}-\\d{4}\\b',
            'just emails':    '\\b[A-Z0-9._%+\\-]+@[A-Z0-9.\\-]+\\.[A-Z]{2,}\\b',
            'PHI (incl. names)': 'Patient\\s+[A-Z][a-z]+\\s+[A-Z][a-z]+\n\\b\\d{3}-\\d{2}-\\d{4}\\b',
        },
        schema: {
            'SOAP-shape': JSON.stringify({
                type: 'object',
                required: ['chief_complaint', 'plan'],
                properties: {
                    chief_complaint: { type: 'string', minLength: 3, maxLength: 200 },
                    subjective:      { type: 'string', maxLength: 4000 },
                    assessment:      { type: 'string', maxLength: 1000 },
                    plan:            { type: 'string', minLength: 1, maxLength: 1000 },
                },
            }, null, 2),
            'minimum required': JSON.stringify({
                type: 'object',
                required: ['chief_complaint', 'subjective', 'assessment', 'plan'],
            }, null, 2),
            'just chief_complaint': JSON.stringify({
                type: 'object',
                required: ['chief_complaint'],
                properties: { chief_complaint: { type: 'string', minLength: 3 } },
            }, null, 2),
        },
        fn: {
            'block long inputs': '// return {decision, rationale}; decision in {"allow","modify","block"}\nif (input.length > 80) {\n  return { decision: "block", rationale: "input too long" };\n}\nreturn { decision: "allow" };',
            'forbid "phone"':    '// case-insensitive phone-word block\nif (/phone/i.test(input)) {\n  return { decision: "block", rationale: "no phone fields allowed" };\n}\nreturn { decision: "allow" };',
            'require plan':      '// inspect output; require plan ≥ 5 chars\nif (typeof output.plan !== "string" || output.plan.length < 5) {\n  return { decision: "block", rationale: "plan missing or trivial" };\n}\nreturn { decision: "allow" };',
        },
    };

    let kind = 'regex';
    const editor = $('#sb-edit');
    const status = $('#sb-status');

    function setKind(k) {
        kind = k;
        document.querySelectorAll('.sb-tab').forEach((t) => {
            t.classList.toggle('is-active', t.dataset.kind === k);
        });
        // load default preset
        const first = Object.entries(PRESETS[k])[0];
        editor.value = first[1];
        renderPresets();
        run();
    }

    function renderPresets() {
        const wrap = $('#sb-presets');
        wrap.innerHTML = Object.keys(PRESETS[kind]).map((label) =>
            `<button class="sb-preset-btn" type="button" data-label="${label}">${label}</button>`
        ).join('');
        wrap.querySelectorAll('button').forEach((b) => {
            b.addEventListener('click', () => {
                editor.value = PRESETS[kind][b.dataset.label];
                run();
            });
        });
    }

    document.querySelectorAll('.sb-tab').forEach((t) => {
        t.addEventListener('click', () => setKind(t.dataset.kind));
    });

    // ── compile + run ─────────────────────────────────────────────────────

    function compileRegex(src) {
        const lines = src.split('\n').map((s) => s.trim()).filter(Boolean);
        const res = [];
        for (const l of lines) res.push(new RegExp(l, 'g'));
        return res;
    }

    function evalRegex(res, enc) {
        let modified = enc.input;
        let any = false;
        const hits = [];
        for (const re of res) {
            const matches = enc.input.match(re);
            if (matches) {
                any = true;
                hits.push(...matches);
                modified = modified.replace(re, '[REDACTED]');
            }
        }
        if (!any) return { decision: 'allow' };
        return { decision: 'modify', payload: modified, rationale: `${hits.length} match${hits.length === 1 ? '' : 'es'}` };
    }

    function evalSchema(schema, enc) {
        const errs = [];
        function walk(s, d, p) {
            if (s.type) {
                const t = s.type, ok =
                    (t === 'object' && d && typeof d === 'object' && !Array.isArray(d)) ||
                    (t === 'array'  && Array.isArray(d)) ||
                    (t === 'string' && typeof d === 'string') ||
                    (t === 'number' && typeof d === 'number') ||
                    (t === 'integer' && Number.isInteger(d)) ||
                    (t === 'boolean' && typeof d === 'boolean');
                if (!ok) { errs.push(`${p||'$'}: expected ${t}`); return; }
            }
            if (s.required && Array.isArray(s.required)) {
                for (const k of s.required) if (!(k in (d||{}))) errs.push(`${p}/${k}: required`);
            }
            if (s.properties && d && typeof d === 'object') {
                for (const k of Object.keys(s.properties)) {
                    if (k in d) walk(s.properties[k], d[k], `${p}/${k}`);
                }
            }
            if (typeof d === 'string') {
                if (s.minLength != null && d.length < s.minLength) errs.push(`${p}: too short`);
                if (s.maxLength != null && d.length > s.maxLength) errs.push(`${p}: too long`);
            }
        }
        walk(schema, enc.output, '');
        if (!errs.length) return { decision: 'allow' };
        return { decision: 'block', rationale: errs[0] };
    }

    function compileFn(src) {
        // safer-ish: still uses Function() so do not run untrusted policies
        // pulled from a server. Sandbox is a teaching surface for the
        // visitor's own input.
        // eslint-disable-next-line no-new-func
        return new Function('input', 'output', src);
    }

    function evalFn(fn, enc) {
        try {
            const r = fn(enc.input, enc.output) || {};
            const dec = ['allow', 'modify', 'block'].includes(r.decision) ? r.decision : 'allow';
            return { decision: dec, payload: r.payload, rationale: r.rationale };
        } catch (e) {
            return { decision: 'block', rationale: 'function threw: ' + (e.message || e) };
        }
    }

    function escapeHtml(s) {
        return String(s).replace(/[&<>"']/g, (c) => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
    }

    function highlightHits(text, regexes) {
        let safe = escapeHtml(text);
        // build a single flag-merged "or" pattern for highlighting
        if (!regexes.length) return safe;
        try {
            const merged = new RegExp(regexes.map((r) => r.source).join('|'), 'g');
            // operate on the raw text, then escape both sides of the match
            const out = [];
            let last = 0;
            let m;
            while ((m = merged.exec(text)) !== null) {
                out.push(escapeHtml(text.slice(last, m.index)));
                out.push('<mark>', escapeHtml(m[0]), '</mark>');
                last = m.index + m[0].length;
                if (m[0].length === 0) merged.lastIndex++;
            }
            out.push(escapeHtml(text.slice(last)));
            return out.join('');
        } catch (_) {
            return safe;
        }
    }

    function run() {
        editor.classList.remove('is-error');
        status.classList.remove('is-ok', 'is-bad');
        let evalFnRef = null;
        let regexes = null;
        let schema  = null;

        try {
            if (kind === 'regex') {
                regexes = compileRegex(editor.value);
                status.textContent = `compiled ${regexes.length} pattern${regexes.length === 1 ? '' : 's'}`;
                status.classList.add('is-ok');
                evalFnRef = (enc) => evalRegex(regexes, enc);
            } else if (kind === 'schema') {
                schema = JSON.parse(editor.value);
                status.textContent = `schema parsed`;
                status.classList.add('is-ok');
                evalFnRef = (enc) => evalSchema(schema, enc);
            } else {
                const f = compileFn(editor.value);
                status.textContent = `function compiled`;
                status.classList.add('is-ok');
                evalFnRef = (enc) => evalFn(f, enc);
            }
        } catch (e) {
            status.textContent = `compile error: ${e.message || e}`;
            status.classList.add('is-bad');
            editor.classList.add('is-error');
            renderRows([]);
            return;
        }

        const rows = ENCOUNTERS.map((enc) => ({ enc, verdict: evalFnRef(enc) }));
        renderRows(rows, regexes);
    }

    function renderRows(rows, regexes) {
        const list = $('#sb-encounters');
        const counts = { allow: 0, modify: 0, block: 0 };
        list.innerHTML = rows.map((r) => {
            counts[r.verdict.decision]++;
            const hl = regexes ? highlightHits(r.enc.input, regexes) : escapeHtml(r.enc.input);
            const verb = { allow: 'allow', modify: 'modify', block: 'block' }[r.verdict.decision];
            const icon = { allow: '✓', modify: '~', block: '✗' }[r.verdict.decision];
            const detail = r.verdict.rationale
                ? `<span class="sb-detail">${escapeHtml(r.verdict.rationale)}${r.verdict.payload ? ' · ' + escapeHtml(r.verdict.payload).slice(0, 240) : ''}</span>`
                : '';
            return `<div class="sb-encounter is-${r.verdict.decision}">
                <span class="sb-encounter__text">${hl}</span>
                <span class="sb-encounter__verdict">${verb}</span>
                <span class="sb-encounter__icon">${icon}</span>
                ${detail}
            </div>`;
        }).join('');

        $('#sb-summary').innerHTML = `
            <span class="ok"><b>${counts.allow}</b> allow</span>
            <span class="mod"><b>${counts.modify}</b> modify</span>
            <span class="bad"><b>${counts.block}</b> block</span>
            <span style="color: var(--ink-faint);">· ${rows.length} encounters</span>`;
    }

    // debounced
    let t = null;
    editor.addEventListener('input', () => {
        clearTimeout(t);
        t = setTimeout(run, 240);
    });

    setKind('regex');
})();
