// Filter glossary entries as the user types
(() => {
    'use strict';
    const $q = document.getElementById('gl-q');
    const list = document.getElementById('gl-list');
    const letters = document.getElementById('gl-letters');
    const allEntries = Array.from(list.querySelectorAll('.gl-entry'));
    const allLetterSections = Array.from(list.querySelectorAll('.gl-letter'));

    // Build the alphabet jump-bar from sections that exist
    const have = new Set(allLetterSections.map((s) => s.dataset.l));
    let html = '';
    for (let c = 0; c < 26; c++) {
        const L = String.fromCharCode(65 + c);
        if (have.has(L)) html += `<a href="#letter-${L}">${L}</a>`;
        else             html += `<a class="is-empty" tabindex="-1" aria-hidden="true">${L}</a>`;
    }
    letters.innerHTML = html;
    allLetterSections.forEach((s) => (s.id = `letter-${s.dataset.l}`));

    // / focuses the search
    document.addEventListener('keydown', (e) => {
        if (e.key === '/' && document.activeElement !== $q &&
            !document.body.classList.contains('cmd-open')) {
            const tgt = e.target;
            if (tgt.tagName === 'INPUT' || tgt.tagName === 'TEXTAREA') return;
            e.preventDefault();
            $q.focus();
            $q.select();
        }
    });

    function escapeHtml(s) {
        return String(s).replace(/[&<>"']/g, (c) => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
    }

    // cache original innerHTML for each entry so we can restore highlights
    allEntries.forEach((e) => { e.dataset.orig = e.innerHTML; });

    function update() {
        const q = $q.value.trim().toLowerCase();
        let visible = 0;
        for (const e of allEntries) {
            const haystack = e.textContent.toLowerCase();
            const match = !q || haystack.includes(q);
            e.classList.toggle('is-hidden', !match);
            if (match && q) {
                // wrap matches in <mark>
                const re = new RegExp('(' + q.replace(/[.*+?^${}()|[\]\\]/g, '\\$&') + ')', 'gi');
                e.innerHTML = e.dataset.orig.replace(/(>)([^<]+)(?=<)/g, (m, lt, txt) => {
                    return lt + txt.replace(re, '<mark>$1</mark>');
                });
                visible++;
            } else if (match) {
                e.innerHTML = e.dataset.orig;
                visible++;
            }
        }
        // hide empty letter sections
        for (const s of allLetterSections) {
            const any = !!s.querySelector('.gl-entry:not(.is-hidden)');
            s.style.display = any ? '' : 'none';
        }
    }

    $q.addEventListener('input', update);
    update();
})();
