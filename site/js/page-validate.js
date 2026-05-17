(() => {
    'use strict';
    const $ = (s) => document.querySelector(s);

    let SCHEMA = null;

    // ── load schema ───────────────────────────────────────────────────────
    fetch('schemas/audit-entry.v1.json', { credentials: 'omit' })
        .then((r) => r.ok ? r.json() : Promise.reject(new Error('schema fetch failed: ' + r.status)))
        .then((s) => {
            SCHEMA = s;
            $('#va-schema-state').textContent = '· loaded · ' + Object.keys(s.properties || {}).length + ' properties · ' + (s.required || []).length + ' required';
        })
        .catch((e) => {
            $('#va-schema-state').textContent = '· error: ' + e.message;
        });

    // ── samples ───────────────────────────────────────────────────────────
    const SAMPLE_GOOD = {
        seq: 874,
        ts: '2026-04-26T14:21:08Z',
        kind: 'inference.completed',
        actor: 'clin:np_marisol_h',
        patient: 'pt:7f3c…b21',
        encounter: 'enc:2026-04-26-r3',
        model: { id: 'scribe-v3', weights_sha256: '6a0f…e2' },
        consent: { purpose: 'scribe', granted_at: '2026-04-26T14:02:00Z' },
        input_hash: 'blake2b:9f31…',
        output_hash: 'blake2b:c8aa…',
        policies: ['phi.v4', 'schema.transcript.v2', 'length.v1'],
        drift: { psi: 0.04, window: '30d' },
        prev_hash: 'blake2b:1d2e…',
        sig: 'ed25519:0x9c…',
    };
    const SAMPLE_BAD = {
        seq: -3,                                  // violates minimum
        ts: 'last tuesday',                       // violates date-time format (light)
        kind: 'INFERENCE_DONE',                    // violates pattern
        actor: 'someone-without-prefix',           // violates pattern
        model: { id: 'scribe-v3' },                // missing weights_sha256
        consent: { purpose: 'gossip' },            // not in enum
        confidence: 1.7,                           // > maximum
        decision: 'shrug',                         // not in enum
        unknown_field: 'should be flagged',        // additionalProperties false
        prev_hash: 'wrong:format',                 // violates pattern
        sig: 'ed25519:0x9c…',
    };

    document.querySelectorAll('button.va-btn').forEach((b) => {
        b.addEventListener('click', () => {
            const act = b.dataset.act;
            if (act === 'sample-good')  $('#va-input').value = JSON.stringify(SAMPLE_GOOD, null, 2);
            if (act === 'sample-bad')   $('#va-input').value = JSON.stringify(SAMPLE_BAD,  null, 2);
            if (act === 'clear')        { $('#va-input').value = ''; renderEmpty(); }
        });
    });

    // ── tiny Draft-2020-12 subset ─────────────────────────────────────────
    function validate(value, schema, path = '$') {
        const errs = [];
        const exp  = schema.type;
        const t    = jsType(value);

        if (exp && !typeMatch(exp, t, value)) {
            errs.push({ path, code: 'type', want: exp, got: t });
            return errs;
        }
        if (Array.isArray(schema.enum) && !schema.enum.includes(value)) {
            errs.push({ path, code: 'enum', want: schema.enum, got: value });
        }
        if (typeof schema.pattern === 'string' && t === 'string') {
            try {
                if (!new RegExp(schema.pattern).test(value)) {
                    errs.push({ path, code: 'pattern', want: schema.pattern, got: value });
                }
            } catch { /* invalid regex in schema — skip */ }
        }
        if (typeof schema.minimum === 'number' && t === 'number' && value < schema.minimum) {
            errs.push({ path, code: 'minimum', want: schema.minimum, got: value });
        }
        if (typeof schema.maximum === 'number' && t === 'number' && value > schema.maximum) {
            errs.push({ path, code: 'maximum', want: schema.maximum, got: value });
        }
        if (typeof schema.minLength === 'number' && t === 'string' && value.length < schema.minLength) {
            errs.push({ path, code: 'minLength', want: schema.minLength, got: value.length });
        }
        if (Array.isArray(schema.required) && t === 'object') {
            for (const k of schema.required) {
                if (!(k in value)) errs.push({ path: path + '.' + k, code: 'required', want: 'present', got: 'missing' });
            }
        }
        if (schema.additionalProperties === false && t === 'object' && schema.properties) {
            for (const k of Object.keys(value)) {
                if (!(k in schema.properties)) {
                    errs.push({ path: path + '.' + k, code: 'additionalProperty', want: 'declared', got: k });
                }
            }
        }
        if (schema.properties && t === 'object') {
            for (const [k, sub] of Object.entries(schema.properties)) {
                if (k in value) errs.push(...validate(value[k], sub, path + '.' + k));
            }
        }
        if (schema.items && Array.isArray(value)) {
            value.forEach((v, i) => errs.push(...validate(v, schema.items, path + '[' + i + ']')));
        }
        if (Array.isArray(schema.oneOf)) {
            const matches = schema.oneOf.filter((s) => validate(value, s, path).length === 0).length;
            if (matches !== 1) {
                errs.push({ path, code: 'oneOf', want: '1 match', got: matches + ' matches' });
            }
        }
        return errs;
    }
    function jsType(v) {
        if (v === null) return 'null';
        if (Array.isArray(v)) return 'array';
        if (Number.isInteger(v)) return 'integer';
        return typeof v;
    }
    function typeMatch(want, got, v) {
        if (want === got) return true;
        if (want === 'number' && got === 'integer') return true;
        if (want === 'integer' && got === 'number' && Number.isInteger(v)) return true;
        return false;
    }

    // ── render ────────────────────────────────────────────────────────────
    function renderEmpty() {
        const out = $('#va-out');
        out.className = 'va-empty';
        out.innerHTML = 'Paste an entry on the left and click <em>validate</em>.';
        setVerdict('idle');
    }
    function setVerdict(state, detail) {
        const v = $('#va-verdict');
        v.classList.remove('is-pass', 'is-fail', 'is-warn');
        if (state === 'pass') { v.classList.add('is-pass'); v.innerHTML = '<span class="va-verdict__dot"></span>conforms · ' + (detail || ''); }
        else if (state === 'fail') { v.classList.add('is-fail'); v.innerHTML = '<span class="va-verdict__dot"></span>' + (detail || 'fails'); }
        else if (state === 'warn') { v.classList.add('is-warn'); v.innerHTML = '<span class="va-verdict__dot"></span>' + (detail || 'parse error'); }
        else { v.innerHTML = '<span class="va-verdict__dot"></span>idle'; }
    }
    function renderResults(errs, declared) {
        const out = $('#va-out');
        if (errs.length === 0) {
            out.className = '';
            out.innerHTML = '';
            const ul = document.createElement('ul');
            ul.className = 'va-list';
            const li = document.createElement('li');
            li.innerHTML = '<span class="va-mark is-ok">✓</span><span><span class="va-msg">passes all required-keys, type, pattern, enum, range and additionalProperties checks · ' + declared + ' declared properties traversed</span></span>';
            ul.appendChild(li);
            out.appendChild(ul);
            setVerdict('pass', errs.length + ' issues');
            return;
        }
        out.className = '';
        out.innerHTML = '';
        const ul = document.createElement('ul');
        ul.className = 'va-list';
        errs.forEach((e) => {
            const li = document.createElement('li');
            const wantTxt = JSON.stringify(e.want);
            const gotTxt  = JSON.stringify(e.got);
            li.innerHTML = '<span class="va-mark is-bad">✗</span>'
                + '<span><span class="va-key">' + escape(e.path) + '</span> '
                + '<span class="va-msg">· ' + e.code + '</span>'
                + '<div class="va-detail">expected ' + escape(wantTxt) + ' · got ' + escape(gotTxt) + '</div>'
                + '</span>';
            ul.appendChild(li);
        });
        out.appendChild(ul);
        setVerdict('fail', errs.length + ' issue' + (errs.length === 1 ? '' : 's'));
    }
    function escape(s) {
        return String(s)
            .replace(/&/g, '&amp;')
            .replace(/</g, '&lt;')
            .replace(/>/g, '&gt;');
    }

    // ── go ────────────────────────────────────────────────────────────────
    $('#va-go').addEventListener('click', () => {
        if (!SCHEMA) { setVerdict('warn', 'schema not loaded yet'); return; }
        const txt = $('#va-input').value.trim();
        if (!txt) { renderEmpty(); return; }
        let parsed;
        try {
            parsed = JSON.parse(txt);
        } catch (e) {
            setVerdict('warn', 'parse error: ' + e.message);
            const out = $('#va-out');
            out.className = '';
            out.innerHTML = '<ul class="va-list"><li><span class="va-mark is-warn">!</span><span><span class="va-msg">JSON parse error · ' + escape(e.message) + '</span></span></li></ul>';
            return;
        }
        const errs = validate(parsed, SCHEMA);
        const declared = Object.keys(SCHEMA.properties || {}).length;
        renderResults(errs, declared);
    });
    renderEmpty();
})();
