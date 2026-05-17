// status dashboard — synthetic but realistic-looking timeseries.
(() => {
  'use strict';
  const $ = (s) => document.querySelector(s);
  const fmt = (n, d = 0) => n.toLocaleString('en-US', { maximumFractionDigits: d });
  const HEX = '0123456789abcdef';
  const fakeHash = (n = 14) => Array.from({ length: n }, () => HEX[Math.floor(Math.random()*16)]).join('');
  const tail = (h) => h.slice(0,8) + '…' + h.slice(-6);

  // base TPS scaled gently by time-of-day
  function baseTps() {
    const h = new Date().getHours();
    return 220 + Math.sin((h - 9) * Math.PI / 12) * 80;
  }

  // running totals
  const startEntriesToday = 142_318;
  const startBlocks24h    = 1_044;

  let tpsHistory = Array.from({ length: 40 }, () => baseTps() + (Math.random()-0.5)*40);
  let entriesToday = startEntriesToday;
  let blocks24h    = startBlocks24h;
  let lastSeq      = 1_482_306;

  const ledger = $('#st-ledger');
  const types = [
    'inference.committed', 'inference.committed', 'inference.committed',
    'inference.committed', 'inference.committed',
    'evaluation.override', 'evaluation.ground_truth', 'consent.granted'
  ];
  const tenants = ['kp-northwest', 'sharp-mercy', 'columbia-research'];

  function pushRow() {
    lastSeq += 1;
    const t = types[Math.floor(Math.random()*types.length)];
    const tenant = tenants[Math.floor(Math.random()*tenants.length)];
    const row = document.createElement('div');
    row.className = 'st-led-row';
    row.innerHTML = `
        <span class="st-led-row__seq">${lastSeq.toString(16).padStart(8,'0').toUpperCase()}</span>
        <span class="st-led-row__type">${t}</span>
        <span class="st-led-row__hash">${tail(fakeHash(40))}</span>
        <span class="st-led-row__time">${tenant}</span>
        <span class="st-led-row__sig">✓</span>`;
    ledger.prepend(row);
    while (ledger.children.length > 6) ledger.removeChild(ledger.lastChild);
  }
  // seed
  for (let i = 0; i < 5; i++) pushRow();

  function paintSpark() {
    const svg = $('#st-spark-tps');
    const w = 100, h = 24;
    const min = Math.min(...tpsHistory);
    const max = Math.max(...tpsHistory);
    const range = max - min || 1;
    const step = w / (tpsHistory.length - 1);
    const pts = tpsHistory.map((v, i) =>
        `${(i*step).toFixed(2)},${(h - ((v - min) / range) * (h - 4) - 2).toFixed(2)}`
    ).join(' ');
    svg.innerHTML = `
        <polyline fill="none" stroke="var(--primary)" stroke-width="1.5" points="${pts}"/>
        <polyline fill="rgba(181,123,255,.08)" stroke="none" points="${pts} ${w},${h} 0,${h}"/>`;
  }

  function update() {
    const tps = Math.round(baseTps() + (Math.random()-0.5)*40);
    tpsHistory.push(tps); if (tpsHistory.length > 40) tpsHistory.shift();

    entriesToday += tps;
    blocks24h    += Math.random() < 0.18 ? 1 : 0;

    $('#st-tps').textContent     = fmt(tps);
    $('#st-entries').textContent = fmt(entriesToday);
    $('#st-blocks').textContent  = fmt(blocks24h);
    $('#st-bundles').textContent = String(37 + Math.floor((Date.now() / 90_000) % 13));

    // chain head moves with each batch
    const head = fakeHash(64);
    $('#st-head').textContent       = head;
    $('#st-seq').textContent        = fmt(lastSeq);
    $('#st-attest-time').textContent= new Date().toISOString().slice(0,19) + 'Z';
    $('#st-last-verify').textContent= 'a moment ago · ok';

    paintSpark();

    if (Math.random() < 0.55) pushRow();
  }
  update();
  setInterval(update, 2200);

  // expose for console inspection
  window.asc = { tpsHistory, get: () => ({ entriesToday, blocks24h, lastSeq, tps: tpsHistory.at(-1) }) };
})();
