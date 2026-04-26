// SPDX-License-Identifier: Apache-2.0
// Asclepius — keyboard help dialog (?) + g-then-key navigation chords.
//
// Press ? to see every binding. Press g then h/d/p/c/s/r to jump to
// home / docs / playground / changelog / status / roadmap. Esc cancels
// the chord. Doesn't fire while you're typing in an input.
//
// Shortcut philosophy is GitHub-style: predictable, mnemonic, lower-case.

(() => {
    'use strict';

    const SHORTCUTS = [
        { keys: ['⌘', 'K'],         desc: 'open command palette',          group: 'Search' },
        { keys: ['Ctrl', 'K'],      desc: 'open command palette',          group: 'Search' },
        { keys: ['/'],              desc: 'open command palette',          group: 'Search' },

        { keys: ['g', 'h'],         desc: 'go to home',                    group: 'Navigation' },
        { keys: ['g', 'd'],         desc: 'go to docs',                    group: 'Navigation' },
        { keys: ['g', 'p'],         desc: 'go to playground',              group: 'Navigation' },
        { keys: ['g', 'c'],         desc: 'go to changelog',               group: 'Navigation' },
        { keys: ['g', 's'],         desc: 'go to status',                  group: 'Navigation' },
        { keys: ['g', 'r'],         desc: 'go to roadmap',                 group: 'Navigation' },
        { keys: ['g', 'b'],         desc: 'go to benchmarks',              group: 'Navigation' },
        { keys: ['g', 'k'],         desc: 'go to crypto toolbox',          group: 'Navigation' },
        { keys: ['g', 'l'],         desc: 'go to conformance',             group: 'Navigation' },

        { keys: ['?'],              desc: 'this help dialog',              group: 'General' },
        { keys: ['Esc'],            desc: 'close dialog / cancel chord',   group: 'General' },
        { keys: ['↑', '↓'],         desc: 'navigate palette results',      group: 'General' },
        { keys: ['↵'],              desc: 'open selected result',          group: 'General' },

        { keys: ['t'],              desc: 'scroll to top',                 group: 'Page' },
        { keys: ['Tab'],            desc: 'reveal "skip to content"',      group: 'Page' },
    ];

    const ROUTES = {
        h: 'index.html',
        d: 'docs.html',
        p: 'demo.html',
        c: 'changelog.html',
        s: 'status.html',
        r: 'roadmap.html',
        b: 'benchmarks.html',
        k: 'crypto.html',
        l: 'conformance.html',
    };

    const $ = (s) => document.querySelector(s);

    function build() {
        const dlg = document.createElement('div');
        dlg.id = 'asc-help';
        dlg.setAttribute('role', 'dialog');
        dlg.setAttribute('aria-modal', 'true');
        dlg.setAttribute('aria-label', 'Keyboard shortcuts');
        dlg.hidden = true;

        const groups = {};
        for (const s of SHORTCUTS) (groups[s.group] ??= []).push(s);

        let html = `
            <div class="help-backdrop" data-close></div>
            <div class="help-panel">
                <header>
                    <span>keyboard shortcuts</span>
                    <kbd>?</kbd>
                </header>
                <div class="help-body">`;
        for (const [g, items] of Object.entries(groups)) {
            html += `<section><h3>${g}</h3><dl>`;
            for (const s of items) {
                const keys = s.keys.map((k) => `<kbd>${escapeHtml(k)}</kbd>`).join('<i>+</i>');
                html += `<dt>${keys}</dt><dd>${escapeHtml(s.desc)}</dd>`;
            }
            html += `</dl></section>`;
        }
        html += `
                </div>
                <footer><span>Press <kbd>Esc</kbd> to close</span></footer>
            </div>`;
        dlg.innerHTML = html;
        document.body.appendChild(dlg);
        return dlg;
    }

    function escapeHtml(s) {
        return String(s).replace(/[&<>"']/g, (c) => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
    }

    let dlg = null;
    function open() {
        if (!dlg) dlg = build();
        dlg.hidden = false;
        document.body.classList.add('help-open');
    }
    function close() {
        if (!dlg) return;
        dlg.hidden = true;
        document.body.classList.remove('help-open');
    }

    function isTyping(el) {
        if (!el) return false;
        const t = el.tagName;
        if (t === 'INPUT' || t === 'TEXTAREA' || t === 'SELECT') return true;
        if (el.isContentEditable) return true;
        // also bail if the command palette is open
        if (document.body.classList.contains('cmd-open')) return true;
        return false;
    }

    let chordPending = false;
    let chordTimer  = null;

    function startChord() {
        chordPending = true;
        document.body.classList.add('chord-pending');
        clearTimeout(chordTimer);
        chordTimer = setTimeout(cancelChord, 1500);
    }
    function cancelChord() {
        chordPending = false;
        document.body.classList.remove('chord-pending');
        clearTimeout(chordTimer);
    }

    document.addEventListener('keydown', (e) => {
        if (isTyping(e.target)) return;
        if (e.metaKey || e.ctrlKey || e.altKey) return;

        // Esc closes help and cancels chord
        if (e.key === 'Escape') {
            cancelChord();
            if (dlg && !dlg.hidden) close();
            return;
        }

        // ? opens help (Shift-/)
        if (e.key === '?' && !chordPending) {
            e.preventDefault();
            open();
            return;
        }

        // chord handler: g, then a route letter
        if (chordPending) {
            const dest = ROUTES[e.key.toLowerCase()];
            cancelChord();
            if (dest) {
                e.preventDefault();
                location.href = dest;
            }
            return;
        }
        if (e.key === 'g') {
            e.preventDefault();
            startChord();
            return;
        }

        // t — scroll to top
        if (e.key === 't' && !e.shiftKey) {
            e.preventDefault();
            const reduce = matchMedia('(prefers-reduced-motion: reduce)').matches;
            scrollTo({ top: 0, behavior: reduce ? 'auto' : 'smooth' });
        }
    });

    // close on backdrop click
    document.addEventListener('click', (e) => {
        if (e.target?.dataset?.close !== undefined) close();
    });
})();
