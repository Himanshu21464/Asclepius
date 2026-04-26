// SPDX-License-Identifier: Apache-2.0
// Asclepius — badges.html click-to-copy snippet handler.
//
// Each .bg-snippet element holds the snippet text in `data-copy`. Click it,
// copy to clipboard, flash "copied" via a CSS pseudo-element. No animation
// dependency on a feature that isn't there.

(() => {
    'use strict';
    document.querySelectorAll('.bg-snippet[data-copy]').forEach((el) => {
        el.addEventListener('click', async () => {
            try {
                await navigator.clipboard.writeText(el.dataset.copy);
                el.dataset.copied = '1';
                setTimeout(() => el.removeAttribute('data-copied'), 1500);
            } catch (e) {
                // most likely a non-secure context — fall back gracefully
                el.dataset.copied = 'err';
                setTimeout(() => el.removeAttribute('data-copied'), 1500);
            }
        });
    });
})();
