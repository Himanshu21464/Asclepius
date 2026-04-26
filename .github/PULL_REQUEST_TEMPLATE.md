<!--
Thanks for sending a PR.

Asclepius prefers smaller, well-scoped patches. If a change touches more
than one of (runtime, policy, audit, telemetry, consent, evaluation, CLI),
consider splitting.
-->

## What

<!-- One sentence: what does this PR change? -->

## Why

<!-- The motivating use case, bug, or design pressure. Link issues / specs. -->

## How

<!-- Implementation summary. Call out anything subtle: ABI, on-disk format,
     wire format, signing inputs, canonical-JSON shape. -->

## Tests

<!-- Which doctest cases now exercise this? `ctest --test-dir build` output? -->

## Spec / threat model implications

<!-- If this changes the bytes a verifier sees, the chain semantics, or
     the threat surface, link the relevant docs/*.md update.
     If it does not, write "none". -->

## Checklist

- [ ] Tests pass locally (`ctest --test-dir build --output-on-failure`)
- [ ] No new compiler warnings under `-Wall -Wextra -Wpedantic`
- [ ] Site asset-manifest regenerated if site/ changed
- [ ] CHANGELOG.md updated if user-visible
- [ ] Spec updated if on-disk / on-wire format moved
- [ ] No PHI in test fixtures
- [ ] My changes are Apache-2.0 compatible
