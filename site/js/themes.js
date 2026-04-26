// SPDX-License-Identifier: Apache-2.0
// Asclepius — multi-preset theme picker.
//
// Beyond the basic dark/light pair, the picker lets a user choose from a
// small curated palette set. Each preset overrides a tiny fraction of the
// design tokens (primary + accent + background tint) — everything else
// stays as-defined by styles.css. Persists to localStorage.

(() => {
    'use strict';

    const PRESETS = {
        // (preset id) — { label, scheme: dark|light, vars: {…} }
        obsidian: {
            label:  'obsidian',
            scheme: 'dark',
            vars: {
                '--primary':       '#B57BFF',
                '--primary-2':     '#8B5CF6',
                '--primary-glow':  'rgba(181,123,255,.18)',
                '--accent':        '#34D399',
                '--accent-glow':   'rgba(52,211,153,.16)',
            },
        },
        paperwhite: {
            label:  'paperwhite',
            scheme: 'light',
            vars: {
                '--primary':       '#6B21A8',
                '--primary-2':     '#7C3AED',
                '--primary-glow':  'rgba(107,33,168,.20)',
                '--accent':        '#059669',
                '--accent-glow':   'rgba(5,150,105,.16)',
            },
        },
        amber: {
            label:  'amber terminal',
            scheme: 'dark',
            vars: {
                '--primary':       '#FACC15',
                '--primary-2':     '#EAB308',
                '--primary-glow':  'rgba(250,204,21,.20)',
                '--accent':        '#FB923C',
                '--accent-glow':   'rgba(251,146,60,.18)',
            },
        },
        contrast: {
            label:  'high contrast',
            scheme: 'dark',
            vars: {
                '--primary':       '#FFFFFF',
                '--primary-2':     '#FFFFFF',
                '--primary-glow':  'rgba(255,255,255,.30)',
                '--accent':        '#7CFFB2',
                '--accent-glow':   'rgba(124,255,178,.22)',
            },
        },
        rosewater: {
            label:  'rosewater',
            scheme: 'light',
            vars: {
                '--primary':       '#BE185D',
                '--primary-2':     '#DB2777',
                '--primary-glow':  'rgba(190,24,93,.20)',
                '--accent':        '#0891B2',
                '--accent-glow':   'rgba(8,145,178,.16)',
            },
        },
    };

    const STORE_KEY = 'asc-theme-preset-v1';

    function applyPreset(id) {
        const preset = PRESETS[id] || PRESETS.obsidian;
        const r = document.documentElement;
        // honour the preset's preferred scheme unless user has manually overridden
        const scheme = localStorage.getItem('asclepius-theme') || preset.scheme;
        r.dataset.theme = scheme;
        for (const [k, v] of Object.entries(preset.vars)) {
            r.style.setProperty(k, v);
        }
        // small computed defaults — primary-deep tracks primary
        r.style.setProperty('--primary-deep', preset.vars['--primary-2']);
    }

    // restore saved preset
    const saved = localStorage.getItem(STORE_KEY);
    if (saved && PRESETS[saved]) applyPreset(saved);

    // expose a small api for the picker UI
    window.ascThemePresets = {
        list: () => Object.entries(PRESETS).map(([id, p]) => ({ id, label: p.label, scheme: p.scheme })),
        current: () => localStorage.getItem(STORE_KEY) || 'obsidian',
        set: (id) => {
            if (!PRESETS[id]) return;
            localStorage.setItem(STORE_KEY, id);
            applyPreset(id);
        },
    };

    // build picker UI: hidden until invoked. Floats next to the existing
    // theme toggle (the moon/sun button). Triggered by a keyboard shortcut
    // (T-then-T) or by clicking a small "swatches" affordance in the meta.
    function build() {
        const meta = document.querySelector('.status__meta');
        if (!meta || meta.querySelector('.theme-presets')) return;

        const btn = document.createElement('button');
        btn.type = 'button';
        btn.className = 'theme-presets';
        btn.setAttribute('aria-label', 'choose color preset');
        btn.title = 'palette';
        btn.innerHTML = '<span class="tp-swatch tp-1"></span><span class="tp-swatch tp-2"></span><span class="tp-swatch tp-3"></span>';
        const cta = meta.querySelector('.status__cta');
        if (cta) meta.insertBefore(btn, cta); else meta.appendChild(btn);

        // popover
        const pop = document.createElement('div');
        pop.className = 'tp-pop';
        pop.hidden = true;
        const cur = window.ascThemePresets.current();
        pop.innerHTML = '<header>palette</header><ul>' + window.ascThemePresets.list().map(({ id, label, scheme }) => `
            <li>
              <button type="button" data-id="${id}" class="${id === cur ? 'is-current' : ''}">
                <span class="tp-dot" style="background: var(--p-${id});"></span>
                <span class="tp-label">${label}</span>
                <em>${scheme}</em>
                ${id === cur ? '<span class="tp-check">✓</span>' : ''}
              </button>
            </li>`).join('') + '</ul>';
        meta.appendChild(pop);

        btn.addEventListener('click', () => { pop.hidden = !pop.hidden; });
        document.addEventListener('click', (e) => {
            if (!pop.hidden && !e.target.closest('.theme-presets, .tp-pop')) pop.hidden = true;
        });
        pop.addEventListener('click', (e) => {
            const t = e.target.closest('button[data-id]');
            if (!t) return;
            window.ascThemePresets.set(t.dataset.id);
            pop.hidden = true;
            // re-render so the checkmark moves
            pop.remove();
            meta.querySelector('.theme-presets')?.remove();
            build();
        });
    }

    document.addEventListener('DOMContentLoaded', build);
})();
