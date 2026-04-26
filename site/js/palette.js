// SPDX-License-Identifier: Apache-2.0
// Asclepius — command palette.
//
// Cmd-K / Ctrl-K (or "/") opens a centred fuzzy-search dialog over the
// whole site. Static index, no network, no telemetry. Esc closes. ↑↓
// navigates, ↵ opens.

(() => {
    'use strict';

    // ── search index ─────────────────────────────────────────────────────
    // hand-curated; deliberate alternative to a runtime crawl. Each entry
    // has terms[] that match against beyond the title.
    const INDEX = [
        // pages
        { title: 'Badges (embeddable SVGs)',         url: 'badges.html',                       kind: 'page',    terms: 'badges shields embed svg readme version license conformance build' },
        { title: 'Principles — design tenets',        url: 'principles.html',                   kind: 'page',    terms: 'principles tenets values trust audit substrate vendor-neutral patient' },
        { title: 'Colophon — about the site',         url: 'colophon.html',                     kind: 'page',    terms: 'colophon site fonts type palette build pipeline craft about' },
        { title: 'API reference',                    url: 'api.html',                          kind: 'page',    terms: 'api reference c++ python rust grpc protocol functions' },
        { title: 'Use cases — three concrete deployments', url: 'use-cases.html',               kind: 'page',    terms: 'use cases scenarios scribe diagnostic agentic ambient soap radiology triage agent action' },
        { title: 'Integrations — vendor recipes',    url: 'integrations.html',                 kind: 'page',    terms: 'integrations recipes vendor scribe imaging agent fhir batch evaluation copy paste twelve lines' },
        { title: 'Audit-entry schema (JSON Schema)', url: 'schemas/audit-entry.v1.json',       kind: 'spec',    terms: 'schema json audit entry ledger row validation 2020-12 v1 spec' },
        { title: 'Playbooks — operator runbooks',    url: 'playbooks.html',                    kind: 'page',    terms: 'playbooks runbooks operator oncall incident sev key rotation truncation drift consent subpoena partition' },
        { title: 'Decisions — ADR log',              url: 'decisions.html',                    kind: 'page',    terms: 'decisions adr architecture decision records why ed25519 blake2b sqlite c++ canonical json tar pimpl result' },
        { title: 'For regulators — compliance map',  url: 'regulator.html',                    kind: 'page',    terms: 'regulator regulators auditor compliance hipaa eu ai act fda samd ab 3030 joint commission map' },
        { title: 'Observability — metrics + traces + logs', url: 'observability.html',         kind: 'page',    terms: 'observability metrics traces logs prometheus opentelemetry otel json lines alert rules promql' },
        { title: 'Research — bibliography &amp; prior art', url: 'research.html',               kind: 'page',    terms: 'research bibliography prior art papers merkle ed25519 blake2 ml-dsa libsodium ct trillian in-toto slsa psi ks emd fhir dicom' },
        { title: 'Validate an entry (in-browser JSON Schema)', url: 'validate.html',             kind: 'page',    terms: 'validate validator json schema entry browser draft 2020-12 audit-entry conform check' },
        { title: 'Economics — substrate cost model', url: 'economics.html',                     kind: 'page',    terms: 'economics cost storage cpu disk retention bytes per entry hospital small medium large' },
        { title: 'Migrations — version-to-version', url: 'migrations.html',                     kind: 'page',    terms: 'migrations migrate upgrade version postgres ml-dsa dual sign m-001 m-002 m-003 m-004' },
        { title: 'Migration M-001 — v0.0 → v0.1',     url: 'migrations.html#m-001',             kind: 'section', terms: 'migration m-001 0.0 0.1 initial ga genesis' },
        { title: 'Migration M-002 — Postgres backend',url: 'migrations.html#m-002',             kind: 'section', terms: 'migration m-002 postgres backend pluggable multi-tenant' },
        { title: 'Migration M-004 — ML-DSA dual-sign',url: 'migrations.html#m-004',             kind: 'section', terms: 'migration m-004 ml-dsa dual sign post quantum v1.0' },
        { title: 'Compliance — HIPAA',               url: 'regulator.html#hipaa',              kind: 'section', terms: 'hipaa security privacy 164 audit controls integrity authentication compliance' },
        { title: 'Compliance — EU AI Act',           url: 'regulator.html#eu-ai-act',          kind: 'section', terms: 'eu ai act high risk article 9 12 14 17 record keeping post market' },
        { title: 'Compliance — FDA SaMD',            url: 'regulator.html#fda-samd',           kind: 'section', terms: 'fda samd 510k pccp gmlp pre market software medical device' },
        { title: 'Compliance — California AB 3030',  url: 'regulator.html#ab-3030',            kind: 'section', terms: 'ab 3030 california generative ai disclosure clinical communications' },
        { title: 'Compliance — Joint Commission',    url: 'regulator.html#joint-comm',         kind: 'section', terms: 'joint commission rc.01.03.01 im.02.01.03 medical record integrity authentication' },
        { title: 'Observability — metrics catalogue',url: 'observability.html#metrics',        kind: 'section', terms: 'metrics catalogue prometheus runtime ledger crypto drift consent bundle' },
        { title: 'Observability — traces',           url: 'observability.html#traces',         kind: 'section', terms: 'traces opentelemetry otel spans inference policy model boundary' },
        { title: 'Observability — alert rules',      url: 'observability.html#alerts',         kind: 'section', terms: 'alert rules promql pager paging threshold severity' },
        { title: 'Playbook — ledger truncation',     url: 'playbooks.html#truncation',         kind: 'section', terms: 'truncation chain broken seq gap prev hash mismatch verifier' },
        { title: 'Playbook — key rotation',          url: 'playbooks.html#key-rotation',       kind: 'section', terms: 'key rotation scheduled ed25519 hsm rotate' },
        { title: 'Playbook — key compromise',        url: 'playbooks.html#key-compromise',     kind: 'section', terms: 'key compromise leak exfiltration revoke emergency' },
        { title: 'Playbook — drift breach',          url: 'playbooks.html#drift-breach',       kind: 'section', terms: 'drift psi threshold exceed feature distribution' },
        { title: 'Playbook — consent revocation',    url: 'playbooks.html#consent-revocation', kind: 'section', terms: 'consent revoke patient withdraw cascade hipaa' },
        { title: 'Playbook — bundle subpoena',       url: 'playbooks.html#subpoena',           kind: 'section', terms: 'subpoena evidence bundle court regulator export legal' },
        { title: 'Playbook — sidecar partition',     url: 'playbooks.html#partition',          kind: 'section', terms: 'partition network split sidecar wal reconcile' },
        { title: 'ADR-001 — Ed25519 over RSA',       url: 'decisions.html#adr-001',            kind: 'adr',     terms: 'adr 001 ed25519 rsa ecdsa signature' },
        { title: 'ADR-002 — BLAKE2b over SHA-256',   url: 'decisions.html#adr-002',            kind: 'adr',     terms: 'adr 002 blake2b sha256 hash' },
        { title: 'ADR-003 — SQLite WAL backend',     url: 'decisions.html#adr-003',            kind: 'adr',     terms: 'adr 003 sqlite wal backend storage' },
        { title: 'ADR-004 — C++20 over Rust',        url: 'decisions.html#adr-004',            kind: 'adr',     terms: 'adr 004 c++ cpp rust language choice' },
        { title: 'ADR-005 — Canonical JSON',         url: 'decisions.html#adr-005',            kind: 'adr',     terms: 'adr 005 canonical json cbor encoding' },
        { title: 'ADR-006 — USTAR tar bundles',      url: 'decisions.html#adr-006',            kind: 'adr',     terms: 'adr 006 ustar tar zip bundle format' },
        { title: 'ADR-007 — Pimpl ABI',              url: 'decisions.html#adr-007',            kind: 'adr',     terms: 'adr 007 pimpl abi stability c++' },
        { title: 'ADR-008 — Result&lt;T,Error&gt;',  url: 'decisions.html#adr-008',            kind: 'adr',     terms: 'adr 008 result error exceptions error handling' },
        { title: 'ADR-009 — Strong typedefs',        url: 'decisions.html#adr-009',            kind: 'adr',     terms: 'adr 009 strong typedef id type safety' },
        { title: 'ADR-010 — ML-DSA migration',       url: 'decisions.html#adr-010',            kind: 'adr',     terms: 'adr 010 ml-dsa dilithium post quantum pq fips 204' },
        { title: 'Use case — ambient scribe',         url: 'use-cases.html#scribe',             kind: 'section', terms: 'scribe ambient soap note transcript microphone clinic family medicine' },
        { title: 'Use case — diagnostic second-read', url: 'use-cases.html#diagnostic',         kind: 'section', terms: 'diagnostic radiology second read peer review imaging dicom pe pulmonary' },
        { title: 'Use case — agentic action',         url: 'use-cases.html#agentic',            kind: 'section', terms: 'agentic agent action triage ed staged commit sign-off blast radius' },
        { title: 'Governance',                       url: 'governance.html',                   kind: 'page',    terms: 'governance decisions rfc maintainers vote release roles' },
        { title: 'Cookies (we use zero)',           url: 'cookies.html',                      kind: 'page',    terms: 'cookies trackers analytics privacy localStorage zero policy' },
        { title: 'Licenses (third-party)',           url: 'licenses.html',                     kind: 'page',    terms: 'license licenses third party libsodium sqlite oss apache mit bsd ofl' },
        { title: 'Sitemap (human index)',           url: 'sitemap.html',                      kind: 'page',    terms: 'sitemap index map all pages' },
        { title: 'Accessibility statement',         url: 'accessibility.html',                kind: 'page',    terms: 'a11y accessibility wcag conformance screen reader' },
        { title: 'Home — Manifesto',                 url: 'index.html',                        kind: 'page',    terms: 'home landing manifesto trust substrate clinical AI' },
        { title: 'Documentation hub',                url: 'docs.html',                         kind: 'page',    terms: 'docs documentation overview' },
        { title: 'Playground (interactive demo)',    url: 'demo.html',                         kind: 'page',    terms: 'playground demo interactive sandbox try' },
        { title: 'Benchmarks',                       url: 'benchmarks.html',                   kind: 'page',    terms: 'benchmarks performance latency throughput numbers speed p99' },
        { title: 'Changelog',                        url: 'changelog.html',                    kind: 'page',    terms: 'changelog releases versions semver history' },
        { title: 'Roadmap',                          url: 'roadmap.html',                      kind: 'page',    terms: 'roadmap upcoming planned future Q3 Q4 conformance L1 L2 L3' },
        { title: 'Security policy',                  url: 'security.html',                     kind: 'page',    terms: 'security vulnerability disclosure pgp key cve report' },
        { title: 'Conformance suite',                url: 'conformance.html',                  kind: 'page',    terms: 'conformance suite L1 L2 L3 audit drift evidence tests pass fail' },
                { title: 'Site graph (page network)',     url: 'sitegraph.html',                    kind: 'page',    terms: 'site graph network visualization map nodes pages links' },
        { title: 'Learn — three surfaces',          url: 'learn.html',                        kind: 'page',    terms: 'learn tutorial hub onboarding playground verify explorer guided walkthrough' },
        { title: 'Compare two bundles',            url: 'compare.html',                      kind: 'page',    terms: 'compare diff bundles ledger excerpts retroactive edits added removed modified' },
        { title: 'Policy sandbox',                 url: 'sandbox.html',                      kind: 'page',    terms: 'sandbox policy editor regex schema custom test verdict allow modify block' },
{ title: 'Deploy guide (production)',     url: 'deploy.html',                       kind: 'page',    terms: 'deploy production docker compose postgres sidecar kubernetes hsm key custody retention' },
        { title: 'Ledger explorer',                url: 'explorer.html',                     kind: 'page',    terms: 'explorer auditor inspect ledger jsonl chain verify upload paste' },
        { title: 'Verify a bundle (walkthrough)',  url: 'verify-bundle.html',          kind: 'page',    terms: 'verify bundle walkthrough offline tar openssl jq tutorial' },
        { title: 'Cheatsheet (one-page reference)',  url: 'cheatsheet.html',                   kind: 'page',    terms: 'cheatsheet reference summary print one-page integration patterns shortcuts performance' },
        { title: 'Glossary',                         url: 'glossary.html',                     kind: 'page',    terms: 'glossary terms definitions phi fhir hipaa merkle psi ks emd ulid' },
        { title: 'Status',                           url: 'status.html',                       kind: 'page',    terms: 'status live dashboard production tps throughput' },
        { title: 'Crypto toolbox',                   url: 'crypto.html',                       kind: 'page',    terms: 'crypto labs hash sign verify ed25519 sha-256 chain forge' },
        { title: 'Labs (pilot sites)',               url: 'labs.html',                         kind: 'page',    terms: 'labs pilots design partners research' },
        { title: 'Architecture',                     url: 'architecture.html',                 kind: 'doc',     terms: 'architecture component map data flow rationale' },
        { title: 'Specification',                    url: 'spec.html',                         kind: 'doc',     terms: 'spec specification on-disk on-wire format' },
        { title: 'Threat model',                     url: 'threat-model.html',                 kind: 'doc',     terms: 'threat model adversary attack security tamper' },

        // landing-page sections
        { title: 'Manifesto',                        url: 'index.html#manifesto',              kind: 'section', terms: 'manifesto principle open' },
        { title: 'The break — 2026 incidents',       url: 'index.html#crisis',                 kind: 'section', terms: 'crisis kaiser sharp unitedhealth liability lawsuit' },
        { title: 'Five primitives',                  url: 'index.html#substrate',              kind: 'section', terms: 'substrate ledger policy drift consent evidence pillars' },
        { title: 'Anatomy of an inference',          url: 'index.html#anatomy',                kind: 'section', terms: 'anatomy diagram dataflow pipeline lifecycle' },
        { title: 'Quickstart',                       url: 'index.html#start',                  kind: 'section', terms: 'quickstart c++ python cli grpc tabs install' },
        { title: 'Compare vs MLflow / Datadog / Epic', url: 'index.html#compare',              kind: 'section', terms: 'compare versus alternatives mlflow datadog epic' },
        { title: 'FAQ',                              url: 'index.html#faq',                    kind: 'section', terms: 'faq questions hipaa overhead epic deployment' },
        { title: 'Why now (regulatory timeline)',    url: 'index.html#why-now',                kind: 'section', terms: 'why now regulatory hhs fda california timeline' },
        { title: 'Open source — vendor neutral',     url: 'index.html#open',                   kind: 'section', terms: 'open source apache vendor neutral' },

        // playground bits
        { title: 'Wrap an inference',                url: 'demo.html#d-app',                   kind: 'action',  terms: 'wrap inference run try interactive' },
        { title: 'Tamper with a ledger entry',       url: 'demo.html#d-ledger',                kind: 'action',  terms: 'tamper invalid corrupt verify red' },
        { title: 'Export evidence bundle',           url: 'demo.html#d-export',                kind: 'action',  terms: 'export evidence bundle download manifest' },

        // docs deep-links
        { title: 'Component map',                    url: 'architecture.html#component-map',           kind: 'topic', terms: 'architecture diagram components' },
        { title: 'Data flow for a single inference', url: 'architecture.html#data-flow-for-a-single-inference', kind: 'topic', terms: 'flow inference run' },
        { title: 'Why a Merkle-chained signed ledger', url: 'architecture.html#why-a-merkle-chained-signed-ledger', kind: 'topic', terms: 'merkle ledger why chain' },
        { title: 'Why Ed25519 + BLAKE2b',             url: 'architecture.html#why-ed25519--blake2b',   kind: 'topic', terms: 'ed25519 blake2b crypto choice' },
        { title: 'Why C++20',                         url: 'architecture.html#why-c20',                 kind: 'topic', terms: 'c++ 20 latency abi' },
        { title: 'Storage model',                     url: 'architecture.html#storage-model',           kind: 'topic', terms: 'sqlite storage schema' },
        { title: 'Evidence bundles',                  url: 'architecture.html#evidence-bundles',        kind: 'topic', terms: 'evidence bundle tar manifest' },
        { title: 'Threading model',                   url: 'architecture.html#threading-model',         kind: 'topic', terms: 'threading concurrency thread-safe' },

        { title: 'Hash function — BLAKE2b-256',       url: 'spec.html#hash-function',                   kind: 'topic', terms: 'hash blake2b 256 libsodium' },
        { title: 'Signature scheme — Ed25519',        url: 'spec.html#signature-scheme',                kind: 'topic', terms: 'sign ed25519 signature' },
        { title: 'Canonical JSON',                    url: 'spec.html#canonical-json',                  kind: 'topic', terms: 'canonical json sorted keys' },
        { title: 'Ledger entry format',               url: 'spec.html#ledger-entry',                    kind: 'topic', terms: 'ledger entry shape' },
        { title: 'SQLite schema',                     url: 'spec.html#sqlite-schema',                   kind: 'topic', terms: 'sqlite schema table wal' },
        { title: 'Standard event types',              url: 'spec.html#standard-event-types',            kind: 'topic', terms: 'event type inference committed override ground truth' },
        { title: 'Evidence bundle (USTAR tar)',       url: 'spec.html#evidence-bundle-ustar-tar',       kind: 'topic', terms: 'tar bundle ustar manifest' },
        { title: 'Versioning policy',                 url: 'spec.html#versioning-policy',               kind: 'topic', terms: 'version semver compatibility' },

        { title: 'A1 — Internal log tampering',       url: 'threat-model.html#a1-internal-actor-tampering-with-audit-logs',     kind: 'topic', terms: 'tamper insider audit log' },
        { title: 'A2 — Malicious model output',       url: 'threat-model.html#a2-model-produces-malicious-or-phi-laden-output', kind: 'topic', terms: 'phi malicious model output' },
        { title: 'A3 — Replay attacks',               url: 'threat-model.html#a3-replay-of-a-stale-model-decision',             kind: 'topic', terms: 'replay attack stale' },
        { title: 'A4 — Bundle forgery',               url: 'threat-model.html#a4-bundle-forgery',                                kind: 'topic', terms: 'forge bundle fake' },
        { title: 'A5 — Side-channel PHI leakage',     url: 'threat-model.html#a5-side-channel-phi-leakage-via-ledger',          kind: 'topic', terms: 'phi leak ledger side channel' },
        { title: 'A6 — Drift undetected',             url: 'threat-model.html#a6-drift-undetected',                              kind: 'topic', terms: 'drift slow undetected' },
        { title: 'What verify() proves',              url: 'threat-model.html#what-verify-proves',                                kind: 'topic', terms: 'verify proves guarantees' },

        // external
        { title: 'GitHub repository',                 url: 'https://github.com/asclepius-ai/asclepius', kind: 'extern', terms: 'github code source repo' },
    ];

    // ── fuzzy ranking ────────────────────────────────────────────────────
    // basic substring match + token-coverage weighting; good enough for a
    // <100-entry index.
    function score(query, item) {
        const q = query.trim().toLowerCase();
        if (!q) return 1;
        const hay = (item.title + ' ' + item.terms + ' ' + item.kind).toLowerCase();
        // exact substring of title is the strongest signal
        if (item.title.toLowerCase().includes(q)) return 100 - item.title.length / 50;
        // every query token must appear *somewhere*
        const tokens = q.split(/\s+/).filter(Boolean);
        let s = 0;
        for (const t of tokens) {
            const ix = hay.indexOf(t);
            if (ix < 0) return 0;
            s += Math.max(1, 50 - ix * 0.5);
        }
        return s / tokens.length;
    }

    function rank(query) {
        return INDEX
            .map((item) => ({ item, s: score(query, item) }))
            .filter(({ s }) => s > 0)
            .sort((a, b) => b.s - a.s)
            .slice(0, 20)
            .map(({ item }) => item);
    }

    // ── render ───────────────────────────────────────────────────────────
    function render() {
        const dlg = document.createElement('div');
        dlg.id = 'asc-palette';
        dlg.setAttribute('role', 'dialog');
        dlg.setAttribute('aria-modal', 'true');
        dlg.setAttribute('aria-label', 'Command palette');
        dlg.hidden = true;
        dlg.innerHTML = `
            <div class="cmd-backdrop" data-close></div>
            <div class="cmd-panel">
                <div class="cmd-input-wrap">
                    <svg class="cmd-glyph" viewBox="0 0 16 16" aria-hidden="true">
                        <circle cx="7" cy="7" r="4.5" fill="none" stroke="currentColor" stroke-width="1.4"/>
                        <line x1="10.5" y1="10.5" x2="14" y2="14" stroke="currentColor" stroke-width="1.4" stroke-linecap="round"/>
                    </svg>
                    <input id="cmd-q" type="text" placeholder="search anything — pages, sections, topics" autocomplete="off" spellcheck="false">
                    <kbd>Esc</kbd>
                </div>
                <ul id="cmd-results" class="cmd-results" role="listbox" aria-label="results"></ul>
                <footer class="cmd-foot">
                    <span><kbd>↑</kbd><kbd>↓</kbd> navigate</span>
                    <span><kbd>↵</kbd> open</span>
                    <span><kbd>Esc</kbd> close</span>
                </footer>
            </div>`;
        document.body.appendChild(dlg);
        return dlg;
    }

    let dlg, $q, $results, selectedIdx = 0, currentResults = [];

    // ── persistence ───────────────────────────────────────────────────────
    // localStorage shape: { recent: [url, ...], lastQuery: string }
    const STORE_KEY = 'asc-palette-v1';

    function load() {
        try { return JSON.parse(localStorage.getItem(STORE_KEY) || '{}'); }
        catch (_) { return {}; }
    }
    function save(state) {
        try { localStorage.setItem(STORE_KEY, JSON.stringify(state)); }
        catch (_) { /* private mode etc. — fail silently */ }
    }
    function rememberVisit(url) {
        const s = load();
        s.recent = [url].concat((s.recent || []).filter((u) => u !== url)).slice(0, 8);
        save(s);
    }
    function recentItems() {
        const s = load();
        const recents = s.recent || [];
        if (!recents.length) return [];
        const byUrl = new Map(INDEX.map((it) => [it.url, it]));
        return recents.map((u) => byUrl.get(u)).filter(Boolean);
    }

    function open() {
        dlg.hidden = false;
        document.body.classList.add('cmd-open');
        // restore last query unless it was a navigation we already completed
        const s = load();
        $q.value = s.lastQuery || '';
        update();
        requestAnimationFrame(() => { $q.focus(); $q.select(); });
    }
    function close() {
        dlg.hidden = true;
        document.body.classList.remove('cmd-open');
        const s = load();
        s.lastQuery = $q.value;
        save(s);
    }

    function update() {
        const q = $q.value.trim();
        // if the query is empty, prefer recent visits (pinned at top), then
        // fall through to a default ranking
        if (!q) {
            const recents = recentItems();
            const seen = new Set(recents.map((it) => it.url));
            const fill = INDEX.filter((it) => !seen.has(it.url) && it.kind === 'page').slice(0, 6);
            currentResults = recents.concat(fill);
        } else {
            // hand-curated INDEX first; full-text fills below the fold
            const curated = rank(q);
            const seenUrls = new Set(curated.map((it) => it.url));
            const fulltext = searchFulltext(q)
                .filter((it) => !seenUrls.has(it.url))
                .sort((a, b) => b.__s - a.__s)
                .slice(0, 12);
            currentResults = curated.concat(fulltext).slice(0, 24);
        }
        selectedIdx = 0;
        if (!currentResults.length) {
            $results.innerHTML = '<li class="cmd-empty">no matches — try "merkle", "phi", or "evidence"</li>';
            return;
        }
        const recentSet = new Set(recentItems().map((it) => it.url));
        $results.innerHTML = currentResults.map((it, i) => {
            const tag = q ? '' : (recentSet.has(it.url)
                ? '<span class="cmd-kind cmd-kind--recent">recent</span>'
                : '');
            return `
            <li role="option" data-i="${i}" data-url="${escapeAttr(it.url)}" class="${i === 0 ? 'is-selected' : ''}">
                ${tag || `<span class="cmd-kind">${escapeHtml(it.kind)}</span>`}
                <span class="cmd-title">${markup(it.title, q)}</span>
                <span class="cmd-arrow">↗</span>
            </li>`;
        }).join('');
    }

    function move(delta) {
        if (!currentResults.length) return;
        selectedIdx = (selectedIdx + delta + currentResults.length) % currentResults.length;
        $results.querySelectorAll('li').forEach((li, i) => {
            li.classList.toggle('is-selected', i === selectedIdx);
            if (i === selectedIdx) li.scrollIntoView({ block: 'nearest' });
        });
    }

    function go() {
        const item = currentResults[selectedIdx];
        if (!item) return;
        rememberVisit(item.url);
        // also remember the query so the next open keeps context
        const s = load();
        s.lastQuery = $q.value;
        save(s);
        if (item.url.startsWith('http')) window.open(item.url, '_blank');
        else window.location.href = item.url;
    }

    function escapeAttr(s) { return String(s).replace(/[&<>"']/g, (c) => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c])); }
    function escapeHtml(s) { return escapeAttr(s); }

    function markup(title, q) {
        if (!q.trim()) return escapeHtml(title);
        const lower = title.toLowerCase();
        const ql    = q.toLowerCase();
        const ix    = lower.indexOf(ql);
        if (ix < 0) return escapeHtml(title);
        return escapeHtml(title.slice(0, ix))
             + '<mark>' + escapeHtml(title.slice(ix, ix + q.length)) + '</mark>'
             + escapeHtml(title.slice(ix + q.length));
    }

    function bindGlobalKeys() {
        document.addEventListener('keydown', (e) => {
            // Cmd-K / Ctrl-K opens the palette
            if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === 'k') {
                e.preventDefault();
                open();
                return;
            }
            // "/" opens unless we're already typing in a field
            if (e.key === '/' && !dlg.hidden === false) return;
            if (e.key === '/' && !isTyping(e.target)) {
                e.preventDefault();
                open();
                return;
            }
            if (dlg.hidden) return;
            if (e.key === 'Escape')                  { e.preventDefault(); close(); return; }
            if (e.key === 'ArrowDown')               { e.preventDefault(); move(1); return; }
            if (e.key === 'ArrowUp')                 { e.preventDefault(); move(-1); return; }
            if (e.key === 'Enter')                   { e.preventDefault(); go();    return; }
        });
    }

    function isTyping(el) {
        if (!el) return false;
        const tag = el.tagName;
        return tag === 'INPUT' || tag === 'TEXTAREA' || el.isContentEditable;
    }

    document.addEventListener('DOMContentLoaded', () => {
        dlg      = render();
        $q       = dlg.querySelector('#cmd-q');
        $results = dlg.querySelector('#cmd-results');

        $q.addEventListener('input', update);
        // start fetching the full-text index on first focus so it's ready
        $q.addEventListener('focus', () => { ensureSearchIndex().then(update); }, { once: true });
        $results.addEventListener('mousemove', (e) => {
            const li = e.target.closest('li[data-i]');
            if (!li) return;
            const i = Number(li.dataset.i);
            if (i !== selectedIdx) {
                selectedIdx = i;
                $results.querySelectorAll('li').forEach((x, j) => x.classList.toggle('is-selected', j === i));
            }
        });
        $results.addEventListener('click', (e) => {
            const li = e.target.closest('li[data-i]');
            if (!li) return;
            selectedIdx = Number(li.dataset.i);
            go();
        });
        dlg.addEventListener('click', (e) => {
            if (e.target.dataset.close !== undefined) close();
        });
        bindGlobalKeys();

        // Insert a "press ⌘K" affordance into the nav meta if there's room
        // for it on desktop. Hidden on mobile by the responsive rules.
        const meta = document.querySelector('.status__meta');
        if (meta && !meta.querySelector('.cmd-trigger')) {
            const t = document.createElement('button');
            t.type = 'button';
            t.className = 'cmd-trigger';
            t.setAttribute('aria-label', 'open command palette');
            t.innerHTML = '<kbd>⌘</kbd><kbd>K</kbd>';
            t.addEventListener('click', open);
            const cta = meta.querySelector('.status__cta');
            if (cta) meta.insertBefore(t, cta);
            else     meta.appendChild(t);
        }
    });
})();
