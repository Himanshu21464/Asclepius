// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
//
// Worked example #6 — wrap `starmpcc/Asclepius`, an open clinical LLM.
//
// `starmpcc/Asclepius` (MIT, github.com/starmpcc/Asclepius) is a
// publicly-shareable clinical LLM family (7B / 13B / Llama3-8B /
// Mistral-7B-v0.3) trained on synthetic discharge summaries. Weights
// live on Hugging Face. The model takes (note, instruction) and returns
// a structured clinical answer. Same name as us, different layer; we
// are the substrate that audits any clinical model — see ADR-012.
//
// The model itself runs in Python (transformers / vLLM / llama.cpp,
// caller's choice). This example demonstrates the C++ side: PHI scrub
// on the (note, instruction) tuple, output schema validation, drift
// telemetry, ledger commit. The Python boundary is stubbed in
// `fake_starmpcc_call` for offline build.
//
// The MIMIC-III variant `Asclepius-R` (PhysioNet, credentialed access)
// is a drop-in for the model id; we don't redistribute its weights but
// the wrap is byte-identical.
//
// Run:
//   ./example_wrap_starmpcc /tmp/starmpcc_demo.db

#include <fmt/core.h>
#include <nlohmann/json.hpp>

#include "asclepius/asclepius.hpp"

#include <chrono>
#include <filesystem>
#include <string>

using namespace asclepius;
using namespace std::chrono_literals;

namespace {

// Stand-in for a Python boundary call to a starmpcc/Asclepius model.
// The real call (via pybind11, gRPC, or a localhost HTTP shim) hands a
// (note, instruction) JSON envelope across and gets back a structured
// response. We stub a clinically-shaped reply so the schema validator
// has work to do.
Result<std::string> fake_starmpcc_call(std::string envelope_json) {
    nlohmann::json env = nlohmann::json::parse(envelope_json);
    nlohmann::json reply = {
        {"answer",
         "Recommend monitoring blood pressure twice daily for two weeks "
         "and follow up with primary care if systolic readings exceed 150 "
         "mmHg. Continue current antihypertensive regimen as prescribed."},
        {"reasoning_trace",
         "Patient has stage 1 hypertension; current regimen is "
         "lisinopril 10 mg. Follow-up interval consistent with JNC-8."},
        {"confidence",   0.78},
        {"citations",    nlohmann::json::array({
            "JNC-8 hypertension guidelines",
            "discharge_note.day1.bp_trend"
        })},
        {"input_tokens", env.value("note", std::string{}).size() / 4},
    };
    return reply.dump();
}

constexpr const char* kAnswerSchema = R"({
    "type": "object",
    "required": ["answer","confidence"],
    "properties": {
        "answer":          {"type":"string","minLength":1,"maxLength":4000},
        "reasoning_trace": {"type":"string","maxLength":4000},
        "confidence":      {"type":"number","minimum":0,"maximum":1},
        "citations":       {"type":"array","items":{"type":"string"}},
        "input_tokens":    {"type":"integer","minimum":0}
    },
    "additionalProperties": false
})";

}  // namespace

int main(int argc, char** argv) {
    std::string db = argc > 1 ? argv[1] : "/tmp/starmpcc_demo.db";
    std::filesystem::remove(db);
    std::filesystem::remove(std::filesystem::path{db}.replace_extension(".key"));

    auto rt = Runtime::open(db);
    if (!rt) {
        fmt::print(stderr, "open: {}\n", rt.error().what());
        return 1;
    }
    auto& runtime = rt.value();

    runtime.policies().push(make_phi_scrubber());
    runtime.policies().push(make_length_limit(/*input_max=*/24000,
                                              /*output_max=*/4000));
    runtime.policies().push(make_schema_validator(kAnswerSchema));

    auto patient = PatientId::pseudonymous("p_starmpcc_7c4b");
    auto tok = runtime.consent().grant(patient,
                                       {Purpose::diagnostic_suggestion,
                                        Purpose::medication_review},
                                       std::chrono::seconds{3600});
    if (!tok) {
        fmt::print(stderr, "consent: {}\n", tok.error().what());
        return 1;
    }

    auto inf = runtime.begin_inference({
        .model            = ModelId{"starmpcc/Asclepius-Llama3-8B", "hf-3c81"},
        .actor            = ActorId::clinician("dr.kwon"),
        .patient          = patient,
        .encounter        = EncounterId::make(),
        .purpose          = Purpose::diagnostic_suggestion,
        .tenant           = TenantId{"demo_clinic"},
        .consent_token_id = tok.value().token_id,
    });
    if (!inf) {
        fmt::print(stderr, "begin: {}\n", inf.error().what());
        return 1;
    }

    nlohmann::json envelope = {
        {"note",
         "Patient Sarah Connor, MRN:55667788, 56F with HTN x 6y on "
         "lisinopril 10mg daily. BP at discharge 148/92. Phone (212) "
         "555-0142. No medication changes. Pt verbalised understanding "
         "of low-sodium diet."},
        {"instr",
         "Summarise post-discharge follow-up plan and any monitoring "
         "the patient should perform at home."},
    };

    auto out = inf.value().run(envelope.dump(), fake_starmpcc_call);
    if (!out) {
        fmt::print(stderr, "run: {}\n", out.error().what());
        return 1;
    }
    fmt::print("starmpcc answer (input PHI-scrubbed, output schema-validated):\n{}\n\n",
               out.value());

    // The clinician disagrees with the recommended monitoring cadence.
    // Capture the override so the chain reflects what was actually
    // followed, not what the model proposed.
    auto over = nlohmann::json::parse(out.value());
    over["answer"] = "Monitor BP daily for one week, then twice weekly "
                     "for three weeks. Follow up at one month.";
    auto cap = inf.value().capture_override(
        "clinician shortened monitoring cadence based on home-monitor reliability",
        over);
    if (!cap) {
        fmt::print(stderr, "override: {}\n", cap.error().what());
        return 1;
    }

    auto v = runtime.ledger().verify();
    if (!v) {
        fmt::print(stderr, "verify: {}\n", v.error().what());
        return 1;
    }

    fmt::print("ledger length          : {}\n", runtime.ledger().length());
    fmt::print("ledger head            : {}\n", runtime.ledger().head().hex());
    fmt::print("ledger signing key id  : {}\n", runtime.ledger().key_id());
    fmt::print("ok\n");
    return 0;
}
