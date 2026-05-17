  (function() {
    if (!('IntersectionObserver' in window)) {
      // Reduced fallback: just mark every scene active so static SVG shows.
      document.querySelectorAll('[data-scene]').forEach(el => el.classList.add('is-active'));
      return;
    }
    const obs = new IntersectionObserver((entries) => {
      for (const e of entries) {
        if (e.isIntersecting) {
          e.target.classList.add('is-active');
        }
      }
    }, {
      rootMargin: '0px 0px -25% 0px',
      threshold: 0.18,
    });
    document.querySelectorAll('[data-scene]').forEach(el => obs.observe(el));
  })();
