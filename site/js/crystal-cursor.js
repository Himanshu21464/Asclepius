// Asclepius — site-wide crystal-lattice cursor effect.
//
// A rigid hexagonal grid of dots sits behind the content as a faint
// honeycomb. The pointer activates a local neighbourhood: edges
// between nearby hex cells fade in, dots brighten and shift colour
// from --primary purple to --accent green as the cursor approaches.
// The grid never moves; the cursor is the only animation source.
//
// No deps. dpr=1 canvas. mix-blend:screen keeps it docile over body
// text. Pauses on tab blur. Honors prefers-reduced-motion.

(() => {
  'use strict';

  if (window.__asclepiusCrystalCursor) return;
  window.__asclepiusCrystalCursor = true;

  const reduce = window.matchMedia &&
    window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  if (reduce) return;

  const canvas = document.createElement('canvas');
  canvas.id = 'crystal-cursor';
  canvas.setAttribute('aria-hidden', 'true');
  canvas.style.cssText = [
    'position:fixed', 'inset:0',
    'width:100%', 'height:100%',
    'pointer-events:none',
    'z-index:1',
    'mix-blend-mode:screen',
    'opacity:0.75',
  ].join(';');
  const mount = () => document.body && document.body.appendChild(canvas);
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', mount, { once: true });
  } else { mount(); }

  const ctx = canvas.getContext('2d', { alpha: true });

  // Palette
  const PRIMARY = '181, 123, 255';
  const ACCENT  = '52, 211, 153';

  // Pointer
  const ptr = { x: -10000, y: -10000, active: false };
  window.addEventListener('pointermove', (e) => {
    ptr.x = e.clientX; ptr.y = e.clientY; ptr.active = true;
  }, { passive: true });

  // Lattice — hexagonal grid of dots
  const SP = 72;                          // hex cell width
  const ROW_H = SP * 0.866;                // sin(60°) — hex row height
  let W = 0, H = 0;
  let dots = [];

  function build() {
    W = window.innerWidth; H = window.innerHeight;
    canvas.width = W; canvas.height = H;
    dots = [];
    let r = 0;
    for (let y = -ROW_H; y < H + ROW_H; y += ROW_H) {
      const ox = (r & 1) ? SP * 0.5 : 0;
      for (let x = -SP; x < W + SP; x += SP) {
        dots.push({ x: x + ox, y, tw: Math.random() * Math.PI * 2 });
      }
      r++;
    }
  }
  build();
  window.addEventListener('resize', build);

  // Constants
  const ACTIVE  = 240;
  const ACTIVE2 = ACTIVE * ACTIVE;
  const EDGE    = SP * 1.05;               // slightly > SP catches both axial neighbours
  const EDGE2   = EDGE * EDGE;
  const CUTOFF2 = (ACTIVE + EDGE) * (ACTIVE + EDGE);

  let raf = 0, paused = false;
  function frame() {
    if (paused) return;
    ctx.clearRect(0, 0, W, H);
    const cx = ptr.x, cy = ptr.y;

    // Pass 1 — activated edges (only when cursor present, only within reach)
    if (ptr.active) {
      ctx.lineWidth = 0.7;
      for (let i = 0; i < dots.length; i++) {
        const a = dots[i];
        const adx = a.x - cx, ady = a.y - cy;
        if (adx * adx + ady * ady > CUTOFF2) continue;
        for (let j = i + 1; j < dots.length; j++) {
          const b = dots[j];
          const dx = a.x - b.x, dy = a.y - b.y;
          const d2 = dx * dx + dy * dy;
          if (d2 > 1 && d2 < EDGE2) {
            const mx = (a.x + b.x) * 0.5, my = (a.y + b.y) * 0.5;
            const dcx = mx - cx, dcy = my - cy;
            const dc = Math.sqrt(dcx * dcx + dcy * dcy);
            const near = Math.max(0, 1 - dc / ACTIVE);
            if (near < 0.10) continue;
            ctx.strokeStyle = `rgba(${PRIMARY}, ${near * 0.55})`;
            ctx.beginPath();
            ctx.moveTo(a.x, a.y); ctx.lineTo(b.x, b.y);
            ctx.stroke();
          }
        }
      }
    }

    // Pass 2 — dots (faint baseline, brighten + tint green near cursor)
    for (const d of dots) {
      d.tw += 0.010;
      const tw = 0.55 + Math.sin(d.tw) * 0.25;
      let near = 0;
      if (ptr.active) {
        const dx = d.x - cx, dy = d.y - cy;
        const d2 = dx * dx + dy * dy;
        if (d2 < ACTIVE2) {
          near = 1 - Math.sqrt(d2) / ACTIVE;
        }
      }
      const a = 0.10 * tw + near * 0.65;
      const r = 1.0 + near * 1.8;
      ctx.fillStyle = near > 0.45
        ? `rgba(${ACCENT}, ${a})`
        : `rgba(${PRIMARY}, ${a})`;
      ctx.beginPath();
      ctx.arc(d.x, d.y, r, 0, Math.PI * 2);
      ctx.fill();
    }

    raf = requestAnimationFrame(frame);
  }

  document.addEventListener('visibilitychange', () => {
    paused = document.hidden;
    if (!paused) raf = requestAnimationFrame(frame);
  });

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', () => { raf = requestAnimationFrame(frame); });
  } else {
    raf = requestAnimationFrame(frame);
  }
})();
