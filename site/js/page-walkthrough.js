/*  walkthrough scene controller — vanilla, ~180 lines.

    Responsibilities:
      · IntersectionObserver flips .is-active on each scene as it enters,
        which the CSS keys all per-scene animations off of.
      · Sticky control bar provides play / pause / prev / next / restart.
      · Keyboard chords: space, j/k, r.
      · Scene 05 runs an rAF tween that morphs the histogram bars and ticks
        the PSI / KS readouts from baseline to drifted while the scene is
        active; resets on re-entry.
      · prefers-reduced-motion short-circuits the auto-progress loop and the
        rAF tween — the page reads as static prose.
*/
(function () {
  'use strict';

  var scenes  = Array.prototype.slice.call(document.querySelectorAll('[data-wt-scene]'));
  var dots    = Array.prototype.slice.call(document.querySelectorAll('[data-wt-jump]'));
  var cursor  = document.querySelector('[data-wt-cursor]');
  var btnPlay = document.querySelector('[data-wt-action="play"]');

  var current   = 0;
  var playing   = false;
  var autoTimer = null;
  var reduce    = window.matchMedia && window.matchMedia('(prefers-reduced-motion: reduce)').matches;

  // ── intersection observer: flag the dominant scene .is-active ────────
  if ('IntersectionObserver' in window) {
    var io = new IntersectionObserver(function (entries) {
      entries.forEach(function (e) {
        var i = parseInt(e.target.getAttribute('data-wt-scene'), 10);
        if (e.isIntersecting && e.intersectionRatio > 0.45) {
          setCurrent(i, /* scrollTo */ false);
          e.target.classList.add('is-active');
        }
      });
    }, { threshold: [0.0, 0.45, 0.85] });
    scenes.forEach(function (s) { io.observe(s); });
  } else {
    // very old browsers: just mark all active so nothing is hidden
    scenes.forEach(function (s) { s.classList.add('is-active'); });
  }

  function setCurrent(i, scrollTo) {
    if (i < 0 || i >= scenes.length) return;
    if (i === current && !scrollTo) {
      if (cursor) cursor.textContent = pad(i + 1);
      dots.forEach(function (d, j) {
        d.setAttribute('aria-current', j === i ? 'true' : 'false');
      });
      if (i === 4) startDrift();
      return;
    }
    current = i;
    if (cursor) cursor.textContent = pad(i + 1);
    dots.forEach(function (d, j) {
      d.setAttribute('aria-current', j === i ? 'true' : 'false');
    });
    if (scrollTo) {
      scenes[i].scrollIntoView({ behavior: reduce ? 'auto' : 'smooth', block: 'start' });
    }
    // re-trigger animation by toggling .is-active
    scenes[i].classList.remove('is-active');
    void scenes[i].offsetWidth;
    scenes[i].classList.add('is-active');
    if (i === 4) startDrift();
  }
  function pad(n) { return n < 10 ? '0' + n : '' + n; }

  // ── play / pause auto-advance ────────────────────────────────────────
  function setPlaying(state) {
    playing = state;
    if (btnPlay) {
      btnPlay.setAttribute('aria-pressed', state ? 'true' : 'false');
      btnPlay.textContent = state ? 'pause' : 'play';
    }
    if (autoTimer) { clearTimeout(autoTimer); autoTimer = null; }
    if (state) tickAdvance();
  }
  function tickAdvance() {
    autoTimer = setTimeout(function () {
      if (!playing) return;
      if (current >= scenes.length - 1) { setPlaying(false); return; }
      setCurrent(current + 1, true);
      tickAdvance();
    }, reduce ? 1200 : 6500);
  }

  // ── controls ─────────────────────────────────────────────────────────
  document.querySelectorAll('[data-wt-action]').forEach(function (b) {
    b.addEventListener('click', function () {
      var a = b.getAttribute('data-wt-action');
      if (a === 'play')    setPlaying(!playing);
      if (a === 'next')    setCurrent(current + 1, true);
      if (a === 'prev')    setCurrent(current - 1, true);
      if (a === 'restart') { setPlaying(false); setCurrent(0, true); }
    });
  });
  dots.forEach(function (d) {
    d.addEventListener('click', function () {
      setPlaying(false);
      setCurrent(parseInt(d.getAttribute('data-wt-jump'), 10), true);
    });
  });
  document.addEventListener('keydown', function (e) {
    var t = e.target;
    if (t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA' || t.isContentEditable)) return;
    if (e.metaKey || e.ctrlKey || e.altKey) return;
    if (e.key === ' ')      { e.preventDefault(); setPlaying(!playing); }
    else if (e.key === 'j') { e.preventDefault(); setCurrent(current + 1, true); }
    else if (e.key === 'k') { e.preventDefault(); setCurrent(current - 1, true); }
    else if (e.key === 'r') { e.preventDefault(); setPlaying(false); setCurrent(0, true); }
  });

  // ── §05 drift tween: bars + readouts from baseline to drifted ────────
  var bars   = Array.prototype.slice.call(document.querySelectorAll('[data-bar]'));
  var psiEl  = document.querySelector('[data-wt-psi]');
  var ksEl   = document.querySelector('[data-wt-ks]');
  var pill   = document.querySelector('[data-wt-pill]');
  // baseline (calm, near-uniform-ish) and drifted (peaks shifted right)
  var BASELINE = [0.40, 0.50, 0.45, 0.35, 0.25, 0.20, 0.15];
  var DRIFTED  = [0.06, 0.08, 0.12, 0.18, 0.42, 0.62, 0.72];
  var driftToken = 0;
  function paintBars(values) {
    bars.forEach(function (b, i) {
      var v = values[i];
      var h = Math.max(2, Math.round(v * 120));
      b.setAttribute('y',      String(140 - h));
      b.setAttribute('height', String(h));
      b.setAttribute('fill',   v > 0.55 ? '#F87171' : 'var(--accent)');
    });
  }
  function startDrift() {
    if (!bars.length) return;
    var token = ++driftToken;
    paintBars(BASELINE);
    if (psiEl)  psiEl.textContent = '0.05';
    if (ksEl)   ksEl.textContent  = '0.04';
    if (pill)   { pill.setAttribute('data-state', 'none'); pill.textContent = 'none'; }
    if (reduce) {
      paintBars(DRIFTED);
      if (psiEl) psiEl.textContent = '0.42';
      if (ksEl)  ksEl.textContent  = '0.31';
      if (pill)  { pill.setAttribute('data-state', 'severe'); pill.textContent = 'severe'; }
      return;
    }
    var start = null;
    var dur   = 4200;
    function frame(t) {
      if (token !== driftToken) return;       // a newer entry started; stop
      if (!start) start = t;
      var raw = (t - start) / dur;
      var k   = Math.min(1, Math.max(0, raw));
      var blend = BASELINE.map(function (b, i) { return b + (DRIFTED[i] - b) * k; });
      paintBars(blend);
      var psi = 0.05 + (0.42 - 0.05) * k;
      var ks  = 0.04 + (0.31 - 0.04) * k;
      if (psiEl) psiEl.textContent = psi.toFixed(2);
      if (ksEl)  ksEl.textContent  = ks.toFixed(2);
      if (pill) {
        var state = psi < 0.10 ? 'none' : psi < 0.25 ? 'minor' : psi < 0.50 ? 'moder' : 'severe';
        if (pill.getAttribute('data-state') !== state) {
          pill.setAttribute('data-state', state === 'severe' ? 'severe' : 'none');
          pill.textContent = state;
        }
      }
      if (k < 1) requestAnimationFrame(frame);
    }
    requestAnimationFrame(frame);
  }

  // ensure the very first scene is marked active on load (above the fold)
  if (scenes[0]) scenes[0].classList.add('is-active');
})();
