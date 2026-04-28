# Example 07 — India profile primitives

Demonstrates the three vendor-neutral types added by [ADR-013] and the
ABDM-aligned `Purpose` values added by [ADR-014]. No network, no ABDM
mock — every primitive runs locally against in-memory registries.

[ADR-013]: ../../site/decisions.html#adr-013
[ADR-014]: ../../site/decisions.html#adr-014

## What this example shows

| § | Primitive | What it demonstrates |
|---|---|---|
| 1 | `EmergencyOverride`  | DPDP § 7 break-glass for an unconscious ED patient; activate, then backfill within the 72h window with an evidence id. |
| 2 | `FamilyGraph`        | Parent records authority over a minor; `can_consent_for(parent, minor)` returns `true`. |
| 3 | `ConsentArtefact`    | Project a `ConsentToken` to ABDM-shaped JSON (the wire format HIPs / HIUs / the Gateway speak), then round-trip back. |

## Build & run

```sh
cmake --build build -j --target example_india_profile
./build/examples/07_india_profile/example_india_profile
```

Expected output is three labelled sections — emergency override
activated and backfilled, family-graph edge recorded, ABDM artefact
round-tripped — followed by `ok`.

## Where it fits

The same three primitives are exercised by the kernel's unit tests
(`tests/test_emergency_override.cpp`, `tests/test_family_graph.cpp`,
`tests/test_consent_artefact.cpp`) and projected onto the wire by the
ABDM mapping at `src/consent/artefact.cpp`. This example is the
end-to-end smoke test — wire all three together, run them as an
operator would, exit `ok`.
