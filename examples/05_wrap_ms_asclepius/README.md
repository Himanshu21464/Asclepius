# Example 05 — wrap `microsoft/asclepius`

[`microsoft/asclepius`][ms] is an Azure-OpenAI sample app that generates
patient-friendly summaries of lab results via Semantic Kernel and a Python
Function App. It is a clinical-AI tool — exactly the layer this substrate is
designed to wrap.

> **Different layer, same name.** See [ADR-012] for the broader name-collision
> story. We are the runtime under the model, not the model.

[ms]: https://github.com/microsoft/asclepius
[ADR-012]: ../../site/decisions.html#adr-012

## What this example shows

1. PHI scrubbing on the *input* context before it leaves the box, so the
   Azure Function never sees patient identifiers.
2. JSON-Schema validation on the *output*, so a malformed model reply
   cannot silently land in the chart.
3. A length cap commensurate with patient-readable output.
4. Verifying the chain after one inference.

The Azure Function call itself is stubbed in `fake_ms_asclepius_summary`.
Swap that body with a real HTTPs call (e.g. `cpr_curl`, `cpp-httplib`, or
the Microsoft REST SDK) and the rest of `main.cpp` is unchanged.

## Build & run

From the repo root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j --target example_wrap_ms_asclepius
./build/examples/05_wrap_ms_asclepius/example_wrap_ms_asclepius /tmp/ms.db
```

Expected output is a short patient-readable summary, then four lines of
ledger metadata and `ok`.

## Hooking up the real Function App

The MS sample's Function App expects a JSON envelope of the shape
`{"chief": str, "hx": str, "labs": [...], "assessments": [...]}` and
returns `{"summary": str, "reading_grade": int, "sources_used": int}`.
Both shapes are already what this example uses, so the only change is the
HTTPs call.

```cpp
auto fake_ms_asclepius_summary = [endpoint, key](std::string ctx) {
    auto resp = http::post(endpoint, {{"x-functions-key", key}}, ctx);
    if (!resp.ok()) return Result<std::string>::error(resp.status_string());
    return Result<std::string>::ok(std::move(resp.body));
};
```

The substrate doesn't care what's behind the boundary; it only cares that
the bytes hash and the policies fire on each side.
