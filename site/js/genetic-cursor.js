// Asclepius — site-wide genetic-lattice cursor effect.
//
// A field of drifting nodes is attracted toward the pointer; nodes
// within proximity bind into transient covalent edges that brighten
// only near the cursor so the lattice does not obscure body text.
//
// Hue range walks the project palette: green (--accent) → cyan →
// purple (--primary). Bonds use --primary. mix-blend:screen keeps the
// graph docile over dark site content.
//
// No deps. dpr=1 canvas. Pauses on tab blur. Honors prefers-reduced-
// motion.

(() => {
  'use strict';

  if (window.__asclepiusGeneticCursor) return;
  window.__asclepiusGeneticCursor = true;

  const reduce = window.matchMedia &&
    window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  if (reduce) return;

  const canvas = document.createElement('canvas');
  canvas.id = 'genetic-cursor';
  canvas.setAttribute('aria-hidden', 'true');
  canvas.style.cssText = [
    'position:fixed', 'inset:0',
    'width:100%', 'height:100%',
    'pointer-events:none',
    'z-index:1',
    'mix-blend-mode:screen',
    'opacity:0.70',
  ].join(';');
  const mount = () => document.body && document.body.appendChild(canvas);
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', mount, { once: true });
  } else { mount(); }

  const ctx = canvas.getContext('2d', { alpha: true });

  let W = 0, H = 0;
  let nodes = [];

  // Pointer state
  const ptr = { x: -10000, y: -10000 };
  let active = false;
  window.addEventListener('pointermove', (e) => {
    ptr.x = e.clientX; ptr.y = e.clientY; active = true;
  }, { passive: true });

  function build() {
    W = window.innerWidth; H = window.innerHeight;
    canvas.width = W; canvas.height = H;
    // Density: ~1 node per 28k px², cap 50
    const count = Math.max(20, Math.min(50, Math.floor((W * H) / 28000)));
    nodes = new Array(count);
    for (let i = 0; i < count; i++) {
      // Hue range walks accent → primary (140° green → 280° purple)
      const hue = 140 + Math.random() * 140;
      nodes[i] = {
        x: Math.random() * W,
        y: Math.random() * H,
        vx: (Math.random() - 0.5) * 0.25,
        vy: (Math.random() - 0.5) * 0.25,
        size: 1.4 + Math.random() * 1.2,
        hue,
        pulse: Math.random() * Math.PI * 2,
      };
    }
  }
  build();
  window.addEventListener('resize', build);

  let raf = 0, paused = false;

  function frame(t) {
    if (paused) return;
    ctx.clearRect(0, 0, W, H);

    const cx = ptr.x, cy = ptr.y;
    const attractR = 240, attractR2 = attractR * attractR;
    const bondR = 130, bondR2 = bondR * bondR;

    // Update nodes — gentle drift, attracted to cursor when in range
    for (const n of nodes) {
      if (active) {
        const dx = cx - n.x, dy = cy - n.y;
        const d2 = dx * dx + dy * dy;
        if (d2 < attractR2) {
          const dist = Math.sqrt(d2);
          const f = (1 - dist / attractR) * 0.30;
          n.vx += (dx / (dist + 1)) * f;
          n.vy += (dy / (dist + 1)) * f;
        }
      }
      n.vx *= 0.95; n.vy *= 0.95;
      n.x += n.vx; n.y += n.vy;
      if (n.x < 0)         n.vx += 0.3;
      else if (n.x > W)    n.vx -= 0.3;
      if (n.y < 0)         n.vy += 0.3;
      else if (n.y > H)    n.vy -= 0.3;
      n.pulse += 0.04;
    }

    // Bonds — only render those whose midpoint is close to the cursor
    if (active) {
      ctx.lineWidth = 0.7;
      const haloR = attractR * 1.2;
      for (let i = 0; i < nodes.length; i++) {
        const a = nodes[i];
        // Cheap pre-check: skip far-from-cursor nodes entirely
        const adx = a.x - cx, ady = a.y - cy;
        if (adx * adx + ady * ady > (haloR + bondR) * (haloR + bondR)) continue;
        for (let j = i + 1; j < nodes.length; j++) {
          const b = nodes[j];
          const dx = a.x - b.x, dy = a.y - b.y;
          const d2 = dx * dx + dy * dy;
          if (d2 < bondR2) {
            const mx = (a.x + b.x) * 0.5, my = (a.y + b.y) * 0.5;
            const dcx = mx - cx, dcy = my - cy;
            const dc2 = dcx * dcx + dcy * dcy;
            const near = Math.max(0, 1 - Math.sqrt(dc2) / haloR);
            if (near < 0.08) continue;
            const d = Math.sqrt(d2);
            const op = (1 - d / bondR) * near;
            ctx.strokeStyle = `rgba(181, 123, 255, ${op * 0.85})`;
            ctx.beginPath();
            ctx.moveTo(a.x, a.y); ctx.lineTo(b.x, b.y);
            ctx.stroke();
          }
        }
      }
    }

    // Nodes
    for (const n of nodes) {
      const pulse = 0.7 + Math.sin(n.pulse) * 0.3;
      let near = 0;
      if (active) {
        const dx = n.x - cx, dy = n.y - cy;
        near = Math.max(0, 1 - Math.sqrt(dx * dx + dy * dy) / attractR);
      }
      const size = n.size * (1 + near * 1.4) * pulse;
      ctx.fillStyle = `hsla(${n.hue}, 90%, 75%, ${0.42 + near * 0.50})`;
      ctx.beginPath();
      ctx.arc(n.x, n.y, size, 0, Math.PI * 2);
      ctx.fill();
    }

    // Subtle cursor halo
    if (active) {
      ctx.fillStyle = 'rgba(52, 211, 153, 0.05)';
      ctx.beginPath();
      ctx.arc(cx, cy, attractR * 0.6, 0, Math.PI * 2);
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
