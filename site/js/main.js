// SPDX-License-Identifier: Apache-2.0
// asclepius site — minimal vanilla JS. ES2020. No framework, no build.
//
// Scope:
//   1. Theme toggle (dark / light), persisted, respects prefers-color-scheme.
//   2. Interactive policy-chain demo (mirrors src/policy/phi_scrubber.cpp).
//   3. Copy-to-clipboard on every <pre> code block.
//   4. Scroll progress indicator + active-section nav highlighting.
//   5. FAQ <details> single-open accordion behaviour.
(() => {
  'use strict';

  // ─── 1. theme toggle ────────────────────────────────────────────────────
  const THEME_KEY = 'asclepius-theme';
  const root = document.documentElement;

  const detectTheme = () => {
    const stored = localStorage.getItem(THEME_KEY);
    if (stored === 'dark' || stored === 'light') return stored;
    return matchMedia('(prefers-color-scheme: light)').matches ? 'light' : 'dark';
  };

  const applyTheme = (t) => {
    root.dataset.theme = t;
    const btn = document.querySelector('[data-theme-toggle]');
    if (btn) {
      btn.setAttribute('aria-pressed', t === 'light' ? 'true' : 'false');
      btn.setAttribute('aria-label', `switch to ${t === 'light' ? 'dark' : 'light'} theme`);
    }
  };

  applyTheme(detectTheme());

  document.addEventListener('click', (e) => {
    const btn = e.target.closest('[data-theme-toggle]');
    if (!btn) return;
    const next = root.dataset.theme === 'light' ? 'dark' : 'light';
    localStorage.setItem(THEME_KEY, next);
    applyTheme(next);
  });

  // honor system change while user hasn't opted in
  matchMedia('(prefers-color-scheme: light)').addEventListener('change', (e) => {
    if (!localStorage.getItem(THEME_KEY)) applyTheme(e.matches ? 'light' : 'dark');
  });

  // ─── 2. interactive policy-chain demo ───────────────────────────────────
  // Patterns mirror src/policy/phi_scrubber.cpp. Order matters: SSN before phone
  // (3-2-4 vs 3-3-4) so the more-specific class wins.
  const PHI_PATTERNS = [
    { cls: 'ssn',   re: /\b\d{3}-\d{2}-\d{4}\b/g },
    { cls: 'phone', re: /\b\(?\d{3}\)?[\s\-]?\d{3}[\s\-]?\d{4}\b/g },
    { cls: 'email', re: /\b[A-Z0-9._%+\-]+@[A-Z0-9.\-]+\.[A-Z]{2,}\b/gi },
    { cls: 'mrn',   re: /\bMRN[:#\-\s]*\d{4,12}\b/gi },
    { cls: 'date',  re: /\b(0?[1-9]|1[0-2])[/\-](0?[1-9]|[12]\d|3[01])[/\-](19|20)\d{2}\b/g },
    { cls: 'cc',    re: /\b\d{4}[\-]\d{4}[\-]\d{4}[\-]\d{4}\b/g },
  ];

  // crude but useful name detector for the demo: capitalized two-word name.
  // (the C++ runtime is regex-only; in production hosts compose this with NER.)
  const NAME_RE = /\b(Mr|Mrs|Ms|Dr|Pt|Patient|Mister)\.?\s+([A-Z][a-z]+)(?:\s+([A-Z][a-z]+))?/g;

  const SAMPLE_TRANSCRIPT =
    'Patient John Doe MRN:12345678 reports 30 minutes of substernal chest pressure ' +
    'radiating to the left arm. Started during morning jog on 04/12/2026. ' +
    'Phone (415) 555-1234. Email: jdoe@example.com. SSN 123-45-6789. ' +
    'Past medical history of hypertension on lisinopril.';

  const escape = (s) => s.replace(/[&<>"']/g, (c) =>
    ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

  const runChain = (text, opts) => {
    const verdicts = [];
    let payload = text;
    let blocked = null;

    if (opts.length_limit && payload.length > opts.length_max) {
      verdicts.push({ name: 'length_limit', decision: 'block',
        rationale: `input length ${payload.length} exceeds limit ${opts.length_max}`,
        violations: ['input_too_long'] });
      blocked = 'length_limit';
      return { payload, verdicts, blocked, classesSeen: [] };
    }

    if (opts.phi_scrubber) {
      const seen = [];
      // mark redactions with a placeholder so we can highlight in HTML
      let scrubbed = payload;
      // optional name match (for demo richness)
      if (opts.scrub_names) {
        scrubbed = scrubbed.replace(NAME_RE, (m) => {
          seen.push('name');
          return `REDACTED:name`;
        });
      }
      for (const { cls, re } of PHI_PATTERNS) {
        scrubbed = scrubbed.replace(re, () => {
          seen.push(cls);
          return `REDACTED:${cls}`;
        });
      }
      const verdict = seen.length === 0
        ? { name: 'phi_scrubber', decision: 'allow', rationale: 'no PHI detected', violations: [] }
        : { name: 'phi_scrubber', decision: 'modify',
            rationale: 'PHI patterns detected and redacted',
            violations: [...new Set(seen)] };
      verdicts.push(verdict);
      payload = scrubbed;
      var classesSeen = [...new Set(seen)];
    } else {
      classesSeen = [];
    }

    if (opts.schema_validator) {
      // demo: requires the shape `{ "soap": "..." }` at output time. since we
      // are showing the input pass, this is informational only.
      verdicts.push({ name: 'schema_validator', decision: 'allow',
        rationale: 'output-only policy, deferred until model returns',
        violations: [] });
    }

    if (opts.action_filter) {
      verdicts.push({ name: 'clinical_action_filter', decision: 'allow',
        rationale: 'output-only policy, deferred until model returns',
        violations: [] });
    }

    return { payload, verdicts, blocked, classesSeen };
  };

  const renderPayload = (raw) => {
    // first escape, then translate sentinel chars into highlighted spans.
    const esc = escape(raw);
    return esc.replace(/REDACTED:([a-z]+)/g,
      (_, cls) => `<span class="redact" data-cls="${cls}">[REDACTED:${cls}]</span>`);
  };

  // tiny djb2 hash → hex16 (for visual ledger entry only — not crypto).
  const fakeHash = (s) => {
    let h1 = 5381, h2 = 52711;
    for (let i = 0; i < s.length; i++) {
      h1 = ((h1 * 33) ^ s.charCodeAt(i)) >>> 0;
      h2 = ((h2 * 41) ^ s.charCodeAt(i)) >>> 0;
    }
    return (h1.toString(16).padStart(8, '0') + h2.toString(16).padStart(8, '0')).slice(0, 16);
  };

  const initDemo = () => {
    const root = document.querySelector('[data-demo]');
    if (!root) return;

    const $in   = root.querySelector('[data-demo-input]');
    const $out  = root.querySelector('[data-demo-output]');
    const $vlst = root.querySelector('[data-demo-verdicts]');
    const $hin  = root.querySelector('[data-demo-input-hash]');
    const $hout = root.querySelector('[data-demo-output-hash]');
    const $stat = root.querySelector('[data-demo-status]');
    const $tags = root.querySelector('[data-demo-tags]');
    const $reset = root.querySelector('[data-demo-reset]');

    if ($in && !$in.value) $in.value = SAMPLE_TRANSCRIPT;

    const opts = () => ({
      phi_scrubber:     root.querySelector('[name=phi_scrubber]')?.checked,
      scrub_names:      root.querySelector('[name=phi_scrubber]')?.checked,
      schema_validator: root.querySelector('[name=schema_validator]')?.checked,
      length_limit:     root.querySelector('[name=length_limit]')?.checked,
      length_max:       parseInt(root.querySelector('[name=length_max]')?.value || '600', 10),
      action_filter:    root.querySelector('[name=action_filter]')?.checked,
    });

    const update = () => {
      const text = $in.value;
      const o = opts();
      const result = runChain(text, o);

      $out.innerHTML = renderPayload(result.payload);
      $hin.textContent  = fakeHash(text).match(/.{1,4}/g).join(' ');
      $hout.textContent = fakeHash(result.payload).match(/.{1,4}/g).join(' ');

      // verdict list
      $vlst.innerHTML = '';
      if (result.verdicts.length === 0) {
        const li = document.createElement('li');
        li.className = 'verdict verdict--idle';
        li.textContent = 'no policies active — chain pass-through';
        $vlst.append(li);
      } else {
        for (const v of result.verdicts) {
          const li = document.createElement('li');
          li.className = `verdict verdict--${v.decision}`;
          li.innerHTML = `
            <span class="verdict__decision">${v.decision}</span>
            <span class="verdict__name">${v.name}</span>
            <span class="verdict__rat">${escape(v.rationale)}</span>
            ${v.violations.length ? `<span class="verdict__viol">${v.violations.map(x=>`<i>${x}</i>`).join('')}</span>` : ''}
          `;
          $vlst.append(li);
        }
      }

      // status pill
      if (result.blocked) {
        $stat.textContent = `blocked · ${result.blocked}`;
        $stat.dataset.kind = 'block';
      } else if (result.classesSeen?.length) {
        $stat.textContent = `ok · modified by phi_scrubber`;
        $stat.dataset.kind = 'modify';
      } else {
        $stat.textContent = 'ok · pass-through';
        $stat.dataset.kind = 'ok';
      }

      // class tags
      $tags.innerHTML = '';
      if (result.classesSeen?.length) {
        for (const c of result.classesSeen) {
          const t = document.createElement('span');
          t.className = 'class-tag';
          t.textContent = c;
          $tags.append(t);
        }
      }
    };

    root.addEventListener('input', update);
    root.addEventListener('change', update);
    $reset?.addEventListener('click', () => {
      $in.value = SAMPLE_TRANSCRIPT;
      root.querySelectorAll('input[type=checkbox]').forEach((c) => {
        const def = c.dataset.default;
        c.checked = def === 'true';
      });
      update();
    });

    update();
  };

  // ─── 3. (copy-to-clipboard handled by js/extras.js to avoid double-wiring)

  // ─── 4. scroll progress + active-section spy ───────────────────────────
  const initScrollSpy = () => {
    const bar = document.querySelector('[data-scroll-progress]');
    const navLinks = Array.from(document.querySelectorAll('.status__nav a[href^="#"]'));
    const sectionByHash = new Map();
    navLinks.forEach((a) => {
      const id = a.getAttribute('href').slice(1);
      const sec = document.getElementById(id);
      if (sec) sectionByHash.set(sec, a);
    });

    const onScroll = () => {
      if (bar) {
        const h = document.documentElement;
        const pct = h.scrollTop / Math.max(1, h.scrollHeight - h.clientHeight);
        bar.style.transform = `scaleX(${Math.min(1, Math.max(0, pct))})`;
      }
    };

    if (bar) {
      onScroll();
      addEventListener('scroll', onScroll, { passive: true });
    }

    if ('IntersectionObserver' in window && sectionByHash.size) {
      const io = new IntersectionObserver((entries) => {
        for (const e of entries) {
          const link = sectionByHash.get(e.target);
          if (!link) continue;
          if (e.isIntersecting) {
            navLinks.forEach((l) => l.removeAttribute('aria-current'));
            link.setAttribute('aria-current', 'true');
          }
        }
      }, { rootMargin: '-40% 0px -55% 0px', threshold: 0 });
      sectionByHash.forEach((_, sec) => io.observe(sec));
    }
  };

  // ─── 5. FAQ single-open accordion ──────────────────────────────────────
  const initFAQ = () => {
    const items = document.querySelectorAll('.faq details');
    items.forEach((d) => {
      d.addEventListener('toggle', () => {
        if (d.open) items.forEach((o) => { if (o !== d) o.open = false; });
      });
    });
  };

  // ─── go ────────────────────────────────────────────────────────────────
  const ready = (fn) => document.readyState === 'loading'
    ? document.addEventListener('DOMContentLoaded', fn)
    : fn();

  ready(() => {
    initDemo();
    initScrollSpy();
    initFAQ();
  });
})();
