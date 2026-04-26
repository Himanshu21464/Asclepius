# Asclepius

> A trust substrate for clinical AI. *"The Linux kernel for clinical AI."*

Asclepius is an open, vendor-neutral C++20 runtime that wraps any clinical AI
tool with audit-grade safety: composable input/output guardrails, a
cryptographically signed Merkle ledger, drift detection, override telemetry,
consent-and-scope enforcement, and an evidence pipeline that emits regulator-
ready bundles.

It is the substrate, not the app. It does not care if the wrapped tool is a
GPT-class scribe, a radiology CNN, an internal LLM agent, or rule-based CDS —
any clinical AI inference becomes auditable, rate-limited, schema-checked,
consent-aware, and replayable through one runtime.

## Status

Early prototype. The runtime, ledger, policy chain, telemetry, consent registry,
evaluation harness, CLI, and Python bindings are functional and tested. The
HTTP/gRPC sidecar and managed evidence service are stubs.

## Build

```sh
git clone https://github.com/Himanshu21464/Asclepius
cd asclepius
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Required system packages on Debian/Ubuntu:

```sh
sudo apt install build-essential cmake ninja-build \
                 libsodium-dev libsqlite3-dev
```

Header-only deps (`fmt`, `nlohmann_json`, `spdlog`, `doctest`, `pybind11`) are
fetched at configure time via CMake's `FetchContent`.

## Storage

One backend: SQLite in WAL mode. Single-node by design — sidecars get one
DB file each, one trust anchor each, one chain each. Open by path:

```cpp
Runtime::open("/var/asc/ledger.db");
```

```sh
asclepius ledger verify /var/asc/ledger.db
```

A previous prototype shipped a parallel Postgres backend; it was ripped in
v0.4.0 after benchmarks came in 30× in SQLite's favour and the
substrate-not-app posture made shared-write semantics irrelevant. *One
backend, less drift.*

## Quick start (C++)

```cpp
#include <asclepius/asclepius.hpp>

using namespace asclepius;

int main() {
    auto rt = Runtime::open("./asclepius.db").value();

    rt.policies().push(make_phi_scrubber());
    rt.policies().push(make_schema_validator(R"({"type":"object"})"));

    auto inference = rt.begin_inference({
        .model_id   = "scribe-v3",
        .actor      = ActorId::clinician("dr.smith"),
        .patient    = PatientId::pseudonymous("p-9f1a"),
        .encounter  = EncounterId::make(),
        .purpose    = Purpose::ambient_documentation,
    });

    auto result = inference.run("Patient reports chest pain ...",
                                /* model invocation */ my_scribe);

    if (!result) {
        spdlog::warn("blocked by policy: {}", result.error().what());
        return 1;
    }

    inference.commit();
}
```

## Quick start (Python)

```python
import asclepius as ax

rt = ax.Runtime("./asclepius.db")
rt.policies.push(ax.policies.phi_scrubber())
rt.policies.push(ax.policies.schema_validator({"type": "object"}))

with rt.begin_inference(
    model_id="scribe-v3",
    actor=ax.Actor.clinician("dr.smith"),
    patient=ax.Patient.pseudonymous("p-9f1a"),
    encounter=ax.Encounter.new(),
    purpose=ax.Purpose.AMBIENT_DOCUMENTATION,
) as inf:
    text = inf.run("Patient reports chest pain ...", my_scribe)
```

## CLI

```sh
asclepius ledger verify  ./asclepius.db          # cryptographic integrity
asclepius ledger inspect ./asclepius.db --tail 50
asclepius drift report   ./asclepius.db --since 7d --model scribe-v3
asclepius policy lint    ./policies/scribe.policy
asclepius evidence bundle ./asclepius.db --window 30d --out evidence.tar
asclepius evidence verify ./evidence.tar
```

## Architecture

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the full diagram and
data-flow description, [`docs/SPEC.md`](docs/SPEC.md) for the on-disk and
on-wire formats, and [`docs/THREAT_MODEL.md`](docs/THREAT_MODEL.md) for what
the audit ledger does and does not protect against.

## Layout

```
include/asclepius/   public C++ headers (the API contract)
src/                 implementations
apps/cli             `asclepius` CLI binary
apps/svc             HTTP/gRPC sidecar for non-C++ tools
bindings/python      pybind11 bindings → `import asclepius`
examples/            worked end-to-end examples
tests/               doctest unit + integration tests
docs/                architecture, spec, threat model
proto/               on-wire schemas (protobuf)
cmake/               build helpers
```

## License

Apache 2.0. A trust substrate must be auditable to be trusted.

## Project brief

See [`Asclepius.tex`](Asclepius.tex) — part of the Future project portfolio
alongside Vita, Kepler, Mnemos, and NeuroKernel.

## Website

The product page lives in [`site/`](site/) — single-file static HTML+CSS,
no build step. `cd site && python3 -m http.server` to view locally.
