// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
//
// Worked example: wrap a fake ambient scribe with Asclepius.
//
// What this demonstrates:
//   1. Creating a Runtime with a SQLite-backed signed ledger
//   2. Composing a policy chain: PHI scrubbing + output schema + length limit
//   3. Granting consent and starting a scoped inference
//   4. Calling a "model" (here: a stub that produces a JSON SOAP note)
//   5. Capturing a clinician override
//   6. Verifying the chain
//
// Run:
//   ./example_wrap_scribe /tmp/asclepius_demo.db

#include <fmt/core.h>
#include <nlohmann/json.hpp>

#include "asclepius/asclepius.hpp"

#include <chrono>
#include <filesystem>

using namespace asclepius;
using namespace std::chrono_literals;

namespace {

// Pretend ambient scribe. In production this is a Whisper + LLM pipeline;
// here we hard-code a SOAP-shaped JSON so the schema validator has work to do.
Result<std::string> fake_scribe(std::string transcript) {
    nlohmann::json soap = {
        {"chief_complaint", "chest pain on exertion"},
        {"subjective",      transcript},
        {"objective",       "BP 132/84, HR 88, RR 16, SpO2 97%"},
        {"assessment",      "atypical chest pain; r/o ACS"},
        {"plan",            "ECG, troponin x2, GTN PRN, cards consult"},
    };
    return soap.dump();
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path db = argc > 1 ? argv[1] : "/tmp/asclepius_demo.db";
    std::filesystem::remove(db);
    std::filesystem::remove(std::filesystem::path{db}.replace_extension(".key"));

    auto rt = Runtime::open(db);
    if (!rt) { fmt::print(stderr, "open: {}\n", rt.error().what()); return 1; }
    auto& runtime = rt.value();

    runtime.policies().push(make_phi_scrubber());
    runtime.policies().push(make_length_limit(/*input_max=*/10000, /*output_max=*/4000));
    runtime.policies().push(make_schema_validator(R"({
        "type": "object",
        "required": ["chief_complaint","subjective","assessment","plan"],
        "properties": {
            "chief_complaint": {"type":"string","minLength":3,"maxLength":200},
            "subjective":      {"type":"string","minLength":1,"maxLength":4000},
            "objective":       {"type":"string"},
            "assessment":      {"type":"string","minLength":1,"maxLength":1000},
            "plan":            {"type":"string","minLength":1,"maxLength":1000}
        }
    })"));

    auto patient = PatientId::pseudonymous("p_demo_9f1a");
    auto tok = runtime.consent().grant(patient,
                                        {Purpose::ambient_documentation},
                                        std::chrono::seconds{3600});
    if (!tok) { fmt::print(stderr, "consent: {}\n", tok.error().what()); return 1; }

    auto inf = runtime.begin_inference({
        .model            = ModelId{"scribe", "v3"},
        .actor            = ActorId::clinician("dr.smith"),
        .patient          = patient,
        .encounter        = EncounterId::make(),
        .purpose          = Purpose::ambient_documentation,
        .tenant           = TenantId{"demo_clinic"},
        .consent_token_id = tok.value().token_id,
    });
    if (!inf) { fmt::print(stderr, "begin: {}\n", inf.error().what()); return 1; }

    const std::string transcript =
        "Patient John Doe MRN:12345678 reports 30 minutes of substernal chest "
        "pressure radiating to the left arm. Started during morning jog. Phone "
        "(415) 555-1234. Denies dyspnea, nausea, diaphoresis. Past medical "
        "history of hypertension on lisinopril.";

    auto out = inf.value().run(transcript, fake_scribe);
    if (!out) { fmt::print(stderr, "run: {}\n", out.error().what()); return 1; }
    fmt::print("scribe output (PHI scrubbed, schema-validated):\n{}\n\n", out.value());

    // Clinician edits "atypical chest pain" → "stable angina" — capture it.
    auto override_corrected = nlohmann::json::parse(out.value());
    override_corrected["assessment"] = "stable angina, GTN responsive";
    auto cap = inf.value().capture_override(
        "clinician revised assessment after additional history", override_corrected);
    if (!cap) { fmt::print(stderr, "override: {}\n", cap.error().what()); return 1; }

    auto v = runtime.ledger().verify();
    if (!v) { fmt::print(stderr, "verify: {}\n", v.error().what()); return 1; }

    fmt::print("ledger length          : {}\n", runtime.ledger().length());
    fmt::print("ledger head            : {}\n", runtime.ledger().head().hex());
    fmt::print("ledger signing key id  : {}\n", runtime.ledger().key_id());
    fmt::print("ok\n");
    return 0;
}
