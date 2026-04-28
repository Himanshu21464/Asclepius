# Example 06 — wrap `starmpcc/Asclepius`

[`starmpcc/Asclepius`][sm] is an MIT-licensed clinical LLM family
(7B / 13B / Llama3-8B / Mistral-7B-v0.3) trained on synthetic discharge
summaries; weights live on Hugging Face. The model takes
`(note, instruction)` and returns a structured clinical answer.

> **Same name as us, different layer.** See [ADR-012] — we are the
> substrate that audits any clinical model, including this one.

[sm]: https://github.com/starmpcc/Asclepius
[ADR-012]: ../../site/decisions.html#adr-012

## What this example shows

1. PHI scrubbing on the `(note, instruction)` envelope before it hits
   the model boundary.
2. JSON-Schema validation on the model's structured reply (answer,
   reasoning trace, confidence, citations).
3. A length cap appropriate for an 8K-context model.
4. A clinician override of the model's monitoring cadence — captured
   with rationale and tied to the original `inference_id`.
5. Chain verification.

The actual model call sits in `fake_starmpcc_call`. To use a real model:

```cpp
// Replace fake_starmpcc_call with a pybind11 call into a Python helper
// that loads the model:
//
//     from transformers import AutoModelForCausalLM, AutoTokenizer
//     tok = AutoTokenizer.from_pretrained("starmpcc/Asclepius-Llama3-8B")
//     mdl = AutoModelForCausalLM.from_pretrained("starmpcc/Asclepius-Llama3-8B")
//     def answer(envelope: str) -> str:
//         out = mdl.generate(**tok(format_prompt(envelope), return_tensors="pt"))
//         return tok.decode(out[0], skip_special_tokens=True)
```

The MIMIC-III variant `Asclepius-R` is a drop-in for the model id —
PhysioNet credentialed access only, but the wrap is byte-identical.

## Build & run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j --target example_wrap_starmpcc
./build/examples/06_wrap_starmpcc/example_wrap_starmpcc /tmp/starmpcc.db
```

Expected output: a clinical answer (PHI scrubbed, schema validated),
ledger length 2 (one inference + one override), signed Ed25519 head,
`ok`.

## Why two ledger entries?

`begin_inference → run → capture_override` produces two rows:

```
seq=0  inference.committed   answer.confidence=0.78
seq=1  evaluation.override   refers_to=seq:0  reason="cadence shortened…"
```

`verify()` walks both. The override is bound to its parent by
`refers_to`, signed by the same key, and cannot be added retroactively
without breaking the chain — which is the whole point.
