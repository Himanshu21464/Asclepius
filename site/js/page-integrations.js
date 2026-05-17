(() => {
    'use strict';
    document.querySelectorAll('[data-tabs]').forEach((tabs) => {
        tabs.querySelectorAll('button[data-pane]').forEach((btn) => {
            btn.addEventListener('click', () => {
                const target = btn.dataset.pane;
                const recipe = btn.closest('.in-recipe');
                recipe.querySelectorAll('.in-tabs button').forEach((b) => b.classList.remove('is-active'));
                recipe.querySelectorAll('.in-pane').forEach((p) => p.classList.remove('is-active'));
                btn.classList.add('is-active');
                const pane = recipe.querySelector('#' + target);
                if (pane) pane.classList.add('is-active');
            });
        });
    });
})();
