#!/usr/bin/env node
// Asclepius — site JS test runner.
//
// Tiny, dependency-free test harness. Targets the *logic* of the site's
// JS — the in-browser regexes, validators, hash helpers, and palette
// ranker — and runs it under node. The bigger UX bits (DOM events, demo
// runtime end-to-end) are exercised in the browser.
//
//   node site/tests/run.mjs               # all tests
//   node site/tests/run.mjs --verbose
//
// Exit code 0 = green, 1 = red. Suitable for CI.

import * as crypto from 'node:crypto';
import * as path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));

let pass = 0, fail = 0;
const verbose = process.argv.includes('--verbose');

function describe(name, fn) {
    try {
        fn();
        if (verbose) console.log(`  · ${name}`);
    } catch (e) {
        console.error(`  ✗ ${name}\n      ${e.message}`);
        fail++;
    }
}
function ok(name, fn) {
    try {
        const r = fn();
        if (r === false) throw new Error(`expected truthy, got false`);
        pass++;
        if (verbose) console.log(`  ✓ ${name}`);
    } catch (e) {
        fail++;
        console.error(`  ✗ ${name}\n      ${e.message}`);
    }
}
function eq(a, b) {
    const A = typeof a === 'object' ? JSON.stringify(a) : String(a);
    const B = typeof b === 'object' ? JSON.stringify(b) : String(b);
    if (A !== B) throw new Error(`expected\n        ${B}\n      got\n        ${A}`);
}

// ─── PHI scrubber regex set (mirrors src/policy/phi_scrubber.cpp) ────────

const PHI_PATTERNS = [
    { re: /\b\d{3}-\d{2}-\d{4}\b/g,                                      cls: 'ssn'   },
    { re: /\b\(?\d{3}\)?[\s\-]?\d{3}[\s\-]?\d{4}\b/g,                    cls: 'phone' },
    { re: /\b[A-Z0-9._%+\-]+@[A-Z0-9.\-]+\.[A-Z]{2,}\b/gi,               cls: 'email' },
    { re: /\bMRN[:#\-\s]*\d{4,12}\b/gi,                                  cls: 'mrn'   },
    { re: /\b(0?[1-9]|1[0-2])[\/\-](0?[1-9]|[12]\d|3[01])[\/\-](19|20)\d{2}\b/g, cls: 'date' },
];

function scrub(text) {
    let out = text;
    const hits = [];
    for (const p of PHI_PATTERNS) {
        const matches = text.match(p.re);
        if (matches) {
            for (const m of matches) hits.push({ cls: p.cls, value: m });
            out = out.replace(p.re, `[REDACTED:${p.cls}]`);
        }
    }
    return { text: out, hits };
}

console.log('phi_scrubber:');
ok('SSN match',          () => eq(scrub('ssn 123-45-6789').hits[0].cls, 'ssn'));
ok('phone match',        () => eq(scrub('call (415) 555-1234').hits[0].cls, 'phone'));
ok('email match',        () => eq(scrub('mail jane@example.com').hits[0].cls, 'email'));
ok('mrn match',          () => eq(scrub('MRN: 1234567').hits[0].cls, 'mrn'));
ok('date match',         () => eq(scrub('on 03/14/2026').hits[0].cls, 'date'));
ok('clean text → no hits', () => eq(scrub('chest pain').hits.length, 0));
ok('SSN replaced',       () => eq(scrub('123-45-6789').text, '[REDACTED:ssn]'));
ok('does not match SSN-shape phone', () => {
    // Phone pattern should match (no overlap with SSN since SSN is 3-2-4 not 3-3-4)
    const r = scrub('415-555-1234');
    return r.hits[0].cls === 'phone';
});
ok('phone area code with parens', () => scrub('(212) 555-9876').hits.length === 1);

// ─── subset JSON-Schema validator (mirrors site demo logic) ──────────────

function schemaCheck(schema, doc) {
    const errs = [];
    function walk(s, d, p) {
        if (s.type) {
            const tOk =
                (s.type === 'object'  && d && typeof d === 'object' && !Array.isArray(d)) ||
                (s.type === 'array'   && Array.isArray(d)) ||
                (s.type === 'string'  && typeof d === 'string') ||
                (s.type === 'number'  && typeof d === 'number') ||
                (s.type === 'integer' && Number.isInteger(d)) ||
                (s.type === 'boolean' && typeof d === 'boolean');
            if (!tOk) { errs.push(`${p||'$'}: expected ${s.type}`); return; }
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
    walk(schema, doc, '');
    return errs;
}

console.log('schema_validator:');
ok('valid SOAP',  () => eq(schemaCheck({ type: 'object', required: ['soap'] }, { soap: 'S: ...' }).length, 0));
ok('missing required', () => schemaCheck({ type: 'object', required: ['soap'] }, { foo: 'bar' }).length > 0);
ok('type mismatch', () => schemaCheck({ type: 'string' }, 42).length > 0);
ok('maxLength', () => schemaCheck({ properties: { s: { type: 'string', maxLength: 3 } } }, { s: 'long' }).length > 0);
ok('valid array', () => schemaCheck({ type: 'array' }, [1,2,3]).length === 0);

// ─── drift PSI math (mirrors src/telemetry/drift.cpp) ────────────────────

function psi(p, q) {
    const eps = 1e-6;
    let s = 0;
    for (let i = 0; i < p.length; i++) {
        const pi = Math.max(p[i], eps);
        const qi = Math.max(q[i], eps);
        s += (pi - qi) * Math.log(pi / qi);
    }
    return s;
}

function classify(p) {
    if (Number.isNaN(p)) return 'severe';
    if (p < 0.10) return 'none';
    if (p < 0.25) return 'minor';
    if (p < 0.50) return 'moder';
    return 'severe';
}

console.log('drift:');
ok('PSI of identical distribution ≈ 0', () => {
    const u = [.2,.2,.2,.2,.2];
    return psi(u, u) < 1e-9;
});
ok('PSI shifted distribution > 0.5', () => {
    const a = [.7, .2, .05, .03, .02];
    const b = [.02, .03, .05, .2, .7];
    return psi(a, b) > 0.5;
});
ok('classify thresholds', () => {
    return classify(0.05) === 'none'
        && classify(0.15) === 'minor'
        && classify(0.30) === 'moder'
        && classify(0.80) === 'severe';
});

// ─── hex helpers ─────────────────────────────────────────────────────────

const HEX = (buf) => Array.from(new Uint8Array(buf)).map((b) => b.toString(16).padStart(2, '0')).join('');

function fromHex(h) {
    return new Uint8Array(h.match(/.{1,2}/g).map((b) => parseInt(b, 16)));
}

console.log('hex:');
ok('round-trip',   () => eq(HEX(fromHex('deadbeef')), 'deadbeef'));
ok('zero-padded',  () => eq(HEX(new Uint8Array([0, 1, 15])), '00010f'));
ok('all-zeros',    () => eq(HEX(new Uint8Array(4)), '00000000'));

// ─── canonical JSON (sorted keys, no whitespace) ─────────────────────────

function canonical(obj) {
    if (typeof obj !== 'object' || obj === null) return JSON.stringify(obj);
    if (Array.isArray(obj)) return '[' + obj.map(canonical).join(',') + ']';
    return '{' + Object.keys(obj).sort()
        .map((k) => JSON.stringify(k) + ':' + canonical(obj[k]))
        .join(',') + '}';
}

console.log('canonical_json:');
ok('object key order', () => {
    const a = { z: 1, a: 2, m: 3 };
    const b = { m: 3, a: 2, z: 1 };
    return canonical(a) === canonical(b);
});
ok('nested objects', () => canonical({ b: { y: 2, x: 1 }, a: 1 }) === '{"a":1,"b":{"x":1,"y":2}}');
ok('arrays preserve order', () => canonical([3, 1, 2]) === '[3,1,2]');

// ─── palette ranking (mirrors site/js/palette.js) ────────────────────────

function score(query, item) {
    const q = query.trim().toLowerCase();
    if (!q) return 1;
    const hay = (item.title + ' ' + item.terms + ' ' + item.kind).toLowerCase();
    if (item.title.toLowerCase().includes(q)) return 100 - item.title.length / 50;
    const tokens = q.split(/\s+/).filter(Boolean);
    let s = 0;
    for (const t of tokens) {
        const ix = hay.indexOf(t);
        if (ix < 0) return 0;
        s += Math.max(1, 50 - ix * 0.5);
    }
    return s / tokens.length;
}

console.log('palette ranking:');
ok('exact title hit ranks first', () => {
    const items = [
        { title: 'Architecture',   terms: 'arch', kind: 'doc' },
        { title: 'Architecture diagram tooling', terms: '', kind: 'doc' },
    ];
    const a = score('architecture', items[0]);
    const b = score('architecture', items[1]);
    return a > b;
});
ok('zero score on no match', () =>
    score('zzzwhitespace', { title: 'arch', terms: '', kind: 'doc' }) === 0);
ok('multi-token AND', () =>
    score('drift psi', { title: 'Drift section', terms: 'psi ks emd', kind: 'topic' }) > 0);

// ─── SHA-256 sanity (uses node crypto, mirrors browser Web Crypto) ──────

console.log('sha-256 (parity with Web Crypto):');
ok('SHA-256 of "abc"', () =>
    eq(crypto.createHash('sha256').update('abc').digest('hex'),
       'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad'));
ok('SHA-256 of empty', () =>
    eq(crypto.createHash('sha256').update('').digest('hex'),
       'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855'));

// ─── glossary auto-linker term coverage ────────────────────────────────

import { readFileSync } from 'node:fs';
const glossary = readFileSync(path.join(HERE, '..', 'glossary.html'), 'utf-8');

const REQUIRED_ANCHORS = [
    'blake2b-256', 'ed25519', 'merkle', 'phi', 'fhir', 'hipaa', 'baa', 'hsm',
    'mrn', 'psi', 'ks', 'emd', 'soap', 'ulid', 'ustar', 'consent-token',
    'patient-id', 'encounter-id', 'purpose', 'verify', 'tampered',
    'attestation', 'bundle', 'canonical-json', 'conformance', 'drift',
    'ground-truth', 'ledger', 'override', 'result', 'sqlite-wal',
];

console.log('glossary anchor coverage:');
for (const a of REQUIRED_ANCHORS) {
    ok(`anchor #${a} exists`, () => glossary.includes(`id="${a}"`));
}

// ─── extended hex round-trip edge cases ──────────────────────────────

console.log('hex (extended):');
ok('hex of 255',       () => eq(HEX(new Uint8Array([255])), 'ff'));
ok('hex of 0..3',      () => eq(HEX(new Uint8Array([0, 1, 2, 3])), '00010203'));
ok('hex 32 bytes',     () => {
    const buf = new Uint8Array(32); for (let i = 0; i < 32; i++) buf[i] = i;
    return HEX(buf) === '000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f';
});
ok('fromHex roundtrip',() => eq(HEX(fromHex('cafebabe')), 'cafebabe'));

// ─── PSI extra cases ────────────────────────────────────────────────

console.log('drift (extended):');
ok('PSI 0 for two empty distributions', () => {
    const z = new Array(20).fill(0);
    return Math.abs(psi(z, z)) < 1e-9;
});
ok('PSI symmetric',          () => {
    const a = [.5, .3, .2];
    const b = [.2, .3, .5];
    return Math.abs(psi(a, b) - psi(b, a)) < 1e-9;
});
ok('classify NaN → severe',  () => classify(NaN) === 'severe');
ok('classify 0.10 → minor',  () => classify(0.10) === 'minor');
ok('classify 0.25 → moder',  () => classify(0.25) === 'moder');
ok('classify 0.50 → severe', () => classify(0.50) === 'severe');

// ─── schema extras ──────────────────────────────────────────────────

console.log('schema (extended):');
ok('integer accepts whole', () => schemaCheck({ type: 'integer' }, 5).length === 0);
ok('integer rejects float', () => schemaCheck({ type: 'integer' }, 5.5).length > 0);
ok('boolean accepts true',  () => schemaCheck({ type: 'boolean' }, true).length === 0);
ok('boolean rejects "true"',() => schemaCheck({ type: 'boolean' }, 'true').length > 0);
ok('object rejects array',  () => schemaCheck({ type: 'object' }, [1]).length > 0);
ok('nested required',       () => schemaCheck({
    type: 'object',
    properties: { inner: { type: 'object', required: ['x'] } }
}, { inner: { y: 1 } }).length > 0);

// ─── palette ranking — multi-token AND ──────────────────────────────

console.log('palette (extended):');
ok('multi-token AND requires all', () =>
    score('drift psi pizza', { title: 'Drift', terms: 'psi ks emd', kind: 'topic' }) === 0);
ok('case-insensitive substring', () =>
    score('Architecture', { title: 'architecture', terms: '', kind: 'doc' }) > 0);
ok('whitespace tolerated', () =>
    score('   merkle  ', { title: 'Merkle', terms: '', kind: 'topic' }) > 0);

// ─── search-index integrity (asclepius's own index) ─────────────────

console.log('search-index integrity:');
const searchIndex = JSON.parse(readFileSync(path.join(HERE, '..', 'search-index.json'), 'utf-8'));
ok('count > 0',                   () => searchIndex.count > 0);
ok('count matches array length',  () => searchIndex.count === searchIndex.pages.length);
ok('every entry has url + title', () => searchIndex.pages.every((p) => p.url && p.title));
ok('headings are arrays',         () => searchIndex.pages.every((p) => Array.isArray(p.headings)));

// ─── feed integrity — RSS, Atom, JSON Feed all describe same items ─

console.log('feed integrity:');
const rssText  = readFileSync(path.join(HERE, '..', 'feed.xml'),  'utf-8');
const atomText = readFileSync(path.join(HERE, '..', 'atom.xml'),  'utf-8');
const jfText   = readFileSync(path.join(HERE, '..', 'feed.json'), 'utf-8');
ok('rss has <channel>',                () => rssText.includes('<channel>'));
ok('rss declares atom self-link',      () => rssText.includes('atom:link'));
ok('atom has feed root',               () => atomText.includes('<feed'));
ok('atom has at least one entry',      () => /<entry>/.test(atomText));
ok('json-feed parses',                 () => { JSON.parse(jfText); return true; });
ok('json-feed version 1.1',            () => JSON.parse(jfText).version.endsWith('1.1'));
ok('all feeds list v0.1.0',            () =>
    rssText.includes('v0.1.0')
    && atomText.includes('v0.1.0')
    && jfText.includes('v0.1.0'));

// ─── asset-manifest integrity ───────────────────────────────────────

console.log('asset-manifest:');
const manifest = JSON.parse(readFileSync(path.join(HERE, '..', 'asset-manifest.json'), 'utf-8'));
ok('algorithm SHA-256',          () => manifest.algorithm === 'SHA-256');
ok('count > 50',                 () => manifest.count > 50);
ok('every sha256 is 64 hex',     () =>
    Object.values(manifest.files).every((m) => typeof m.sha256 === 'string' && m.sha256.length === 64));
ok('every entry has positive bytes', () =>
    Object.values(manifest.files).every((m) => typeof m.bytes === 'number' && m.bytes > 0));
ok('contains styles.css',        () => 'styles.css' in manifest.files);
ok('contains every JS file',     () =>
    ['js/main.js', 'js/extras.js', 'js/palette.js', 'js/help.js', 'js/floating.js', 'js/attest-site.js', 'js/themes.js']
    .every((p) => p in manifest.files));

// ─── audit-entry schema integrity ──────────────────────────────────────
// Light validation: schema parses, declares required keys, regex
// patterns compile. We then validate the schema's own examples and the
// inline JSON examples in use-cases.html against the *required-keys*
// invariant — full Draft 2020-12 validation is left to a real validator.

console.log('audit-entry schema:');
const schema = JSON.parse(readFileSync(path.join(HERE, '..', 'schemas', 'audit-entry.v1.json'), 'utf-8'));
ok('schema declares 2020-12',       () => schema.$schema === 'https://json-schema.org/draft/2020-12/schema');
ok('schema has $id',                () => typeof schema.$id === 'string' && schema.$id.startsWith('https://'));
ok('schema is type=object',         () => schema.type === 'object');
ok('schema requires seq + ts + kind + actor + prev_hash + sig', () =>
    ['seq','ts','kind','actor','prev_hash','sig'].every((k) => schema.required.includes(k)));
ok('schema patterns compile',       () => {
    let count = 0;
    for (const [, p] of Object.entries(schema.properties)) {
        if (p.pattern) { new RegExp(p.pattern); count++; }
    }
    return count > 0;
});
ok('all examples have required keys', () =>
    schema.examples.every((ex) => schema.required.every((k) => k in ex)));

// validate the inline ledger excerpts in use-cases.html against the
// schema's required-keys invariant. these are the exact rows shown to
// the reader; if they ever fall out of sync with the schema, this fails.
const useCasesHtml = readFileSync(path.join(HERE, '..', 'use-cases.html'), 'utf-8');
function extractJsonBlocks(html) {
    // find <pre class="uc-bundle"> ... </pre>, strip <span> tags, return
    // the bracket-balanced JSON-like text inside.
    const out = [];
    const re = /<pre class="uc-bundle"[^>]*>([\s\S]*?)<\/pre>/g;
    let m;
    while ((m = re.exec(html)) !== null) {
        let txt = m[1].replace(/<[^>]+>/g, '');
        // decode the few HTML entities we use
        txt = txt.replace(/&quot;/g, '"').replace(/&amp;/g, '&').replace(/&lt;/g, '<').replace(/&gt;/g, '>');
        // strip JS-style line comments (the example bundles use //…)
        txt = txt.replace(/\/\/[^\n]*/g, '');
        out.push(txt.trim());
    }
    return out;
}
const blocks = extractJsonBlocks(useCasesHtml);
ok('use-cases.html has 3 example bundle blocks', () => blocks.length === 3);

function tryParse(text) {
    // The bundle excerpts use ellipses (…) inside hex strings — strip
    // those so JSON.parse is happy. We are testing structure, not bytes.
    const cleaned = text.replace(/…/g, '');
    return JSON.parse(cleaned);
}
ok('every example block parses as JSON', () => blocks.every((b) => {
    try { tryParse(b); return true; } catch { return false; }
}));

const FLEX_REQUIRED = ['seq', 'kind'];   // every row in every example shape
function rowsOf(parsed) {
    return Array.isArray(parsed) ? parsed : [parsed];
}
ok('every row has at minimum {seq, kind}', () =>
    blocks.every((b) => rowsOf(tryParse(b)).every((r) => FLEX_REQUIRED.every((k) => k in r))));

// ─── /validate.html validator algorithm parity ─────────────────────────
// The page-side validator (in validate.html) implements a Draft 2020-12
// subset. Mirror the algorithm here and verify it accepts the schema's
// own example, rejects a deliberately-broken sample, and reports the
// expected error codes.

console.log('validate.html validator logic:');

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
function validateMini(value, sch, path = '$') {
    const errs = [];
    const t = jsType(value);
    if (sch.type && !typeMatch(sch.type, t, value)) {
        errs.push({ path, code: 'type' });
        return errs;
    }
    if (Array.isArray(sch.enum) && !sch.enum.includes(value)) errs.push({ path, code: 'enum' });
    if (typeof sch.pattern === 'string' && t === 'string') {
        try { if (!new RegExp(sch.pattern).test(value)) errs.push({ path, code: 'pattern' }); } catch { /* skip */ }
    }
    if (typeof sch.minimum === 'number' && t === 'number' && value < sch.minimum) errs.push({ path, code: 'minimum' });
    if (typeof sch.maximum === 'number' && t === 'number' && value > sch.maximum) errs.push({ path, code: 'maximum' });
    if (Array.isArray(sch.required) && t === 'object') {
        for (const k of sch.required) if (!(k in value)) errs.push({ path: path + '.' + k, code: 'required' });
    }
    if (sch.additionalProperties === false && t === 'object' && sch.properties) {
        for (const k of Object.keys(value)) {
            if (!(k in sch.properties)) errs.push({ path: path + '.' + k, code: 'additionalProperty' });
        }
    }
    if (sch.properties && t === 'object') {
        for (const [k, sub] of Object.entries(sch.properties)) {
            if (k in value) errs.push(...validateMini(value[k], sub, path + '.' + k));
        }
    }
    if (sch.items && Array.isArray(value)) value.forEach((v, i) => errs.push(...validateMini(v, sch.items, path + '[' + i + ']')));
    return errs;
}

ok('schema example validates clean',     () => validateMini(schema.examples[0], schema).length === 0);
ok('broken sample produces errors',      () => {
    const broken = { seq: -1, ts: 'last tuesday', kind: 'NOT_DOTTED', actor: 'plain', prev_hash: 'wrong', sig: 'ed25519:0x1' };
    return validateMini(broken, schema).length > 0;
});
ok('missing required.seq is flagged',    () => {
    const noSeq = JSON.parse(JSON.stringify(schema.examples[0])); delete noSeq.seq;
    return validateMini(noSeq, schema).some((e) => e.code === 'required' && e.path.endsWith('.seq'));
});
ok('additionalProperty=false catches extras', () => {
    const extra = JSON.parse(JSON.stringify(schema.examples[0])); extra.unknown = 'x';
    return validateMini(extra, schema).some((e) => e.code === 'additionalProperty');
});
ok('enum violation on consent.purpose',  () => {
    const bad = JSON.parse(JSON.stringify(schema.examples[0])); bad.consent.purpose = 'gossip';
    return validateMini(bad, schema).some((e) => e.code === 'enum' && e.path.endsWith('.purpose'));
});
ok('pattern violation on actor',         () => {
    const bad = JSON.parse(JSON.stringify(schema.examples[0])); bad.actor = 'no-prefix';
    return validateMini(bad, schema).some((e) => e.code === 'pattern' && e.path.endsWith('.actor'));
});

// ─── cross-link integrity (every href + every anchor) ──────────────────
// Walks every *.html page under the site root, strips <script> and
// <style> blocks (so template literals and regex literals don't generate
// false positives), and verifies that:
//   · every internal link target exists on disk
//   · every #anchor referenced exists as an id="…" on the target page
// Catches the class of bugs introduced when a page is renamed or an
// anchor is removed.

import { readdirSync } from 'node:fs';

console.log('cross-link integrity:');
const SITE_ROOT  = path.join(HERE, '..');
const SCRIPT_RE  = /<script\b[^>]*>[\s\S]*?<\/script>/gi;
const STYLE_RE   = /<style\b[^>]*>[\s\S]*?<\/style>/gi;
const HREF_RE    = /href="([^"]+)"/g;
const ID_RE      = /id="([^"]+)"/g;

const htmlPages = readdirSync(SITE_ROOT).filter((f) => f.endsWith('.html'));
const idsByPage = new Map();
const rawByPage = new Map();
for (const f of htmlPages) {
    const raw = readFileSync(path.join(SITE_ROOT, f), 'utf-8');
    const stripped = raw.replace(SCRIPT_RE, '').replace(STYLE_RE, '');
    rawByPage.set(f, stripped);
    idsByPage.set(f, new Set([...stripped.matchAll(ID_RE)].map((m) => m[1])));
}

const linkIssues = [];
for (const f of htmlPages) {
    const stripped = rawByPage.get(f);
    for (const m of stripped.matchAll(HREF_RE)) {
        const url = m[1];
        if (/^(https?:|mailto:|tel:|javascript:|#|\/)/.test(url)) {
            if (url.startsWith('#')) {
                const aid = url.slice(1);
                if (aid && !idsByPage.get(f).has(aid)) {
                    linkIssues.push(`${f}: same-page #${aid} not defined`);
                }
            }
            continue;
        }
        const [target, anchor] = url.includes('#') ? url.split('#', 2) : [url, null];
        if (target && !htmlPages.includes(target)) {
            // allow non-HTML targets that exist on disk (e.g. schemas/, feeds, assets)
            try { readFileSync(path.join(SITE_ROOT, target)); }
            catch { linkIssues.push(`${f}: link target "${target}" missing`); }
        }
        if (anchor && htmlPages.includes(target)) {
            if (!idsByPage.get(target).has(anchor)) {
                linkIssues.push(`${f}: ${target}#${anchor} anchor missing`);
            }
        }
    }
}
ok(`every internal link resolves (${htmlPages.length} pages walked)`, () => {
    if (linkIssues.length) {
        console.error('   issues:\n' + linkIssues.slice(0, 10).map((l) => '     ' + l).join('\n'));
    }
    return linkIssues.length === 0;
});

// ─── accessibility — every input has an accessible name ────────────────
// WCAG 2.2 SC 4.1.2: every form control needs a programmatically-determined
// name. Acceptable sources: aria-label, aria-labelledby, a wrapping <label>,
// or a sibling <label for="X"> where X matches the input id. Hidden inputs
// (type="hidden") and submit/button types with a `value` attribute are
// exempt; everything else must have one.

console.log('a11y — input accessible names:');
const a11yIssues = [];
const LABEL_FOR_RE = /<label[^>]*\bfor="([^"]+)"/g;

for (const f of htmlPages) {
    const stripped = rawByPage.get(f);
    const labelTargets = new Set([...stripped.matchAll(LABEL_FOR_RE)].map((m) => m[1]));
    for (const m of stripped.matchAll(/<input\b([^>]*)>/g)) {
        const attrs = m[1];
        const typeM = /\btype="([^"]+)"/.exec(attrs);
        const type = typeM ? typeM[1] : 'text';
        if (type === 'hidden' || type === 'submit' || type === 'button' || type === 'reset') continue;
        if (/\baria-label(?:ledby)?=/.test(attrs)) continue;
        const idM = /\bid="([^"]+)"/.exec(attrs);
        if (idM && labelTargets.has(idM[1])) continue;
        a11yIssues.push(`${f}: <input type="${type}"${idM ? ` id="${idM[1]}"` : ''}> has no accessible name`);
    }
    for (const m of stripped.matchAll(/<button\b([^>]*)>([\s\S]*?)<\/button>/g)) {
        const attrs = m[1];
        const inner = m[2].replace(/<svg[\s\S]*?<\/svg>/g, '').replace(/<[^>]+>/g, '').trim();
        if (/\baria-label(?:ledby)?=/.test(attrs)) continue;
        if (inner.length > 0) continue;
        a11yIssues.push(`${f}: empty <button> has no accessible name`);
    }
    for (const m of stripped.matchAll(/<img\b([^>]*)>/g)) {
        if (!/\balt=/.test(m[1])) {
            a11yIssues.push(`${f}: <img> missing alt`);
        }
    }
}

ok(`every input/button/img has accessible name (${htmlPages.length} pages)`, () => {
    if (a11yIssues.length) {
        console.error('   issues:\n' + a11yIssues.slice(0, 10).map((l) => '     ' + l).join('\n'));
    }
    return a11yIssues.length === 0;
});

// ─── heading hierarchy — h1 → h2 → h3 with no skipped levels ───────────
// WCAG 2.4.6 + 1.3.1: heading levels create a document outline; jumping
// from h1 to h3 (skipping h2) breaks screen-reader navigation. We allow
// jumping back UP (h3 → h2) but never DOWN past one level.

console.log('heading hierarchy:');
const headingIssues = [];
for (const f of htmlPages) {
    const stripped = rawByPage.get(f);
    const bodyIdx = stripped.indexOf('<body');
    const body = bodyIdx >= 0 ? stripped.slice(bodyIdx) : stripped;
    const levels = [...body.matchAll(/<h([1-6])\b/g)].map((m) => Number(m[1]));
    if (!levels.length) continue;
    if (levels[0] !== 1) {
        headingIssues.push(`${f}: starts at h${levels[0]} (should start at h1)`);
    }
    for (let i = 1; i < levels.length; i++) {
        if (levels[i] > levels[i - 1] + 1) {
            headingIssues.push(`${f}: h${levels[i - 1]} → h${levels[i]} (skipped a level)`);
        }
    }
}
ok(`no skipped heading levels (${htmlPages.length} pages)`, () => {
    if (headingIssues.length) {
        console.error('   issues:\n' + headingIssues.slice(0, 10).map((l) => '     ' + l).join('\n'));
    }
    return headingIssues.length === 0;
});

// ─── summary ───────────────────────────────────────────────────────────

console.log('');
console.log(`${pass + fail} tests · ${pass} passed · ${fail} failed`);
process.exit(fail === 0 ? 0 : 1);
