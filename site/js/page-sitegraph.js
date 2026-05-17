// Site graph — drawn live from search-index.json + a small fetch of each
// page's links. Force-directed via Barnes-Hut-free naive O(n²) — fine
// for ~25 nodes. No dependencies.
(() => {
    'use strict';

    const W = 1000, H = 600;
    const svg = document.getElementById('sg-svg');
    const info = document.getElementById('sg-info');
    const me = location.pathname.split('/').pop() || 'index.html';

    async function loadIndex() {
        try {
            const r = await fetch('search-index.json');
            const j = await r.json();
            return j.pages || [];
        } catch (_) { return []; }
    }

    async function fetchLinks(url) {
        try {
            const r = await fetch(url);
            const text = await r.text();
            const links = new Set();
            const re = /href="([a-z][a-z0-9-]*\.html)(?:#[^"]*)?"/gi;
            let m;
            while ((m = re.exec(text)) !== null) links.add(m[1]);
            return Array.from(links);
        } catch (_) { return []; }
    }

    function init(pages) {
        // place nodes initially in a ring, then relax
        const n = pages.length;
        for (let i = 0; i < n; i++) {
            const a = (i / n) * Math.PI * 2;
            pages[i].x  = W/2 + Math.cos(a) * 220;
            pages[i].y  = H/2 + Math.sin(a) * 180;
            pages[i].vx = 0; pages[i].vy = 0;
            pages[i].outs = [];
        }
        return pages;
    }

    function step(pages, edges) {
        const repel = 1800;          // node-node repulsion
        const link  = 0.04;          // edge spring
        const pull  = 0.02;          // gravity to center
        for (const a of pages) {
            // repulsion
            for (const b of pages) {
                if (a === b) continue;
                const dx = a.x - b.x, dy = a.y - b.y;
                const d2 = Math.max(60, dx*dx + dy*dy);
                a.vx += (dx / d2) * repel;
                a.vy += (dy / d2) * repel;
            }
            // gravity
            a.vx += (W/2 - a.x) * pull;
            a.vy += (H/2 - a.y) * pull;
        }
        for (const e of edges) {
            const a = pages[e.s], b = pages[e.t];
            if (!a || !b) continue;
            const dx = b.x - a.x, dy = b.y - a.y;
            a.vx += dx * link; a.vy += dy * link;
            b.vx -= dx * link; b.vy -= dy * link;
        }
        // integrate, with strong damping
        for (const a of pages) {
            a.vx *= 0.55; a.vy *= 0.55;
            a.x += a.vx * 0.08; a.y += a.vy * 0.08;
            // clamp inside frame
            a.x = Math.max(40, Math.min(W - 40, a.x));
            a.y = Math.max(40, Math.min(H - 40, a.y));
        }
    }

    function render(pages, edges) {
        const idx = new Map(pages.map((p, i) => [p.url, i]));
        const meIdx = idx.get(me);

        // edges first (drawn under nodes)
        const eHtml = edges.map((e) => {
            const a = pages[e.s], b = pages[e.t];
            return `<line class="sg-edge" data-s="${e.s}" data-t="${e.t}" x1="${a.x.toFixed(1)}" y1="${a.y.toFixed(1)}" x2="${b.x.toFixed(1)}" y2="${b.y.toFixed(1)}"/>`;
        }).join('');

        const nHtml = pages.map((p, i) => {
            const isCurrent = i === meIdx ? ' is-current' : '';
            const label = (p.url || '').replace(/\.html$/, '').replace(/^index$/, 'home');
            return `<g class="sg-node${isCurrent}" data-i="${i}" data-url="${p.url}">
                <circle cx="${p.x.toFixed(1)}" cy="${p.y.toFixed(1)}" r="6"/>
                <text x="${p.x.toFixed(1)}" y="${(p.y + 18).toFixed(1)}" text-anchor="middle">${label}</text>
            </g>`;
        }).join('');

        svg.innerHTML = eHtml + nHtml;

        info.innerHTML = `<b>${pages.length}</b> pages · <b>${edges.length}</b> links · hover any node`;

        const nodeEls = svg.querySelectorAll('.sg-node');
        const edgeEls = svg.querySelectorAll('.sg-edge');

        nodeEls.forEach((g) => {
            const i = Number(g.dataset.i);
            g.addEventListener('mouseenter', () => {
                const neighbours = new Set();
                edgeEls.forEach((e) => {
                    const s = Number(e.dataset.s), t = Number(e.dataset.t);
                    const touch = s === i || t === i;
                    e.classList.toggle('is-active', touch);
                    e.classList.toggle('is-dim',    !touch);
                    if (s === i) neighbours.add(t);
                    if (t === i) neighbours.add(s);
                });
                nodeEls.forEach((other) => {
                    const j = Number(other.dataset.i);
                    if (j === i)                 { other.classList.add('is-hot'); other.classList.remove('is-dim'); }
                    else if (neighbours.has(j))  { other.classList.add('is-hot'); other.classList.remove('is-dim'); }
                    else                          { other.classList.remove('is-hot'); other.classList.add('is-dim'); }
                });
                const p = pages[i];
                info.innerHTML = `<b>${p.url}</b> — ${neighbours.size} neighbour${neighbours.size === 1 ? '' : 's'}`;
            });
            g.addEventListener('mouseleave', () => {
                edgeEls.forEach((e) => { e.classList.remove('is-active', 'is-dim'); });
                nodeEls.forEach((other) => other.classList.remove('is-hot', 'is-dim'));
                info.innerHTML = `<b>${pages.length}</b> pages · <b>${edges.length}</b> links · hover any node`;
            });
            g.addEventListener('click', () => {
                const url = g.dataset.url;
                if (url && url !== me) location.href = url;
            });
        });
    }

    async function main() {
        let pages = await loadIndex();
        if (!pages.length) {
            svg.innerHTML = '';
            const stage = document.querySelector('.sg-stage');
            const div = document.createElement('div');
            div.className = 'sg-empty';
            div.textContent = 'search-index.json not available — re-run build_search.py';
            stage.appendChild(div);
            info.textContent = 'no data';
            return;
        }

        pages = init(pages);

        // crawl edges from each page
        info.textContent = 'fetching links…';
        const edges = [];
        const idx = new Map(pages.map((p, i) => [p.url, i]));
        // limit concurrency
        let done = 0;
        await Promise.all(pages.map(async (p, i) => {
            const links = await fetchLinks(p.url);
            for (const l of links) {
                const j = idx.get(l);
                if (j != null && j !== i) edges.push({ s: i, t: j });
            }
            done++;
            info.textContent = `fetching links… ${done} / ${pages.length}`;
        }));

        // dedupe edges (treat as undirected for layout)
        const seen = new Set();
        const ud = [];
        for (const e of edges) {
            const k = e.s < e.t ? `${e.s}-${e.t}` : `${e.t}-${e.s}`;
            if (seen.has(k)) continue;
            seen.add(k); ud.push(e);
        }

        info.textContent = 'relaxing layout…';
        // run a few iterations
        for (let i = 0; i < 240; i++) step(pages, ud);

        render(pages, ud);
    }

    main();
})();
