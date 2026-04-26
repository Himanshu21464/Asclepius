// SPDX-License-Identifier: Apache-2.0
// Asclepius — "edit this page on GitHub" link injector.
//
// Adds a small monospace "edit on GitHub →" link to every page's footer.
// Doc pages (architecture, spec, threat-model) point at the markdown
// source in /docs/; everything else points at the HTML file directly.
// No HTML changes needed — the injection is automatic.

(() => {
    'use strict';

    const REPO_BASE = 'https://github.com/Himanshu21464/Asclepius/edit/main';

    // pages that are auto-rendered from /docs/*.md should edit the markdown
    const MD_SOURCED = {
        'architecture.html': 'docs/ARCHITECTURE.md',
        'spec.html':         'docs/SPEC.md',
        'threat-model.html': 'docs/THREAT_MODEL.md',
    };

    function buildLink() {
        let path = location.pathname;
        // normalize trailing slash and bare directory to index.html
        if (path === '/' || path.endsWith('/')) path += 'index.html';
        const name = path.replace(/^.*\//, '');

        const sourcePath = MD_SOURCED[name] || `site/${name}`;
        const url = `${REPO_BASE}/${sourcePath}`;

        const a = document.createElement('a');
        a.className = 'edit-source';
        a.href = url;
        a.target = '_blank';
        a.rel = 'noopener';
        a.textContent = 'edit this page →';
        a.title = `${sourcePath}`;
        return a;
    }

    function inject() {
        // skip 404 (intentionally minimal) and pages without a footer
        if (document.body && document.body.classList.contains('page-404')) return;
        const foot = document.querySelector('footer.foot .foot__row, footer.foot');
        if (!foot) return;
        if (foot.querySelector('.edit-source')) return;

        // attach as a sibling of the brand block; sits subtly in the corner
        const brand = foot.querySelector('.foot__brand');
        const link = buildLink();
        if (brand && brand.parentNode) {
            brand.parentNode.insertBefore(link, brand.nextSibling);
        } else {
            foot.appendChild(link);
        }
    }

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', inject);
    } else {
        inject();
    }
})();
