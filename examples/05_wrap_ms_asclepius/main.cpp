// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
//
// Worked example #5 — wrap the Microsoft `asclepius` sample app.
//
// Microsoft's `microsoft/asclepius` (MIT, github.com/microsoft/asclepius)
// is an Azure-OpenAI demo that generates patient-friendly summaries of
// lab results via Semantic Kernel + a Python Function App. It is a
// clinical-AI tool — exactly the layer this substrate is designed to
// wrap. Different layer, same name; see ADR-012 in /decisions.html.
//
// What this example demonstrates:
//   1. Wrapping an external `summarize_labs(context) -> string` call —
//      here stubbed in `fake_ms_asclepius_summary` — behind the runtime.
//   2. PHI scrubbing on the *input* context before it leaves the box, so
//      the Azure Function never sees patient identifiers.
//   3. JSON-Schema validation on the *output* so a malformed model reply
//      cannot silently land in the chart.
//   4. A length cap commensurate with patient-readable output.
//   5. Verifying the chain after one inference.
//
// The fake here is intentionally byte-identical in shape to what the MS
// sample produces (a one-paragraph plain-English explanation). Swap the
// `fake_ms_asclepius_summary` body with an actual HTTPs call to the
// Function App and the rest of this file is unchanged.
//
// Run:
//   ./example_wrap_ms_asclepius /tmp/ms_asclepius_demo.db

#include <fmt/core.h>
#include <nlohmann/json.hpp>

#include "asclepius/asclepius.hpp"

#include <chrono>
#include <filesystem>
#include <string>

using namespace asclepius;
using namespace std::chrono_literals;

namespace {

// Stand-in for `microsoft/asclepius`'s Function App endpoint. The real
// implementation lives behind an HTTPs URL bound by Azure Functions; we
// stub it here so the example builds without network or Azure auth. The
// shape of the output is what the MS sample actually produces:
// a short, plain-English paragraph for a patient.
Result<std::string> fake_ms_asclepius_summary(std::string context_json) {
    nlohmann::json ctx = nlohmann::json::parse(context_json);
    nlohmann::json reply = {
        {"summary",
         "Your recent blood test shows your kidney and liver markers are "
         "in the normal range. Your cholesterol is slightly above target, "
         "which your clinician will discuss at your next visit. No urgent "
         "action is needed."},
        {"reading_grade", 7},
        {"sources_used", ctx.contains("labs") ? ctx["labs"].size() : 0},
    };
    return reply.dump();
}

constexpr const char* kSummarySchema = R"({
    "type": "object",
    "required": ["summary","reading_grade"],
    "properties": {
        "summary":       {"type":"string","minLength":40,"maxLength":1200},
        "reading_grade": {"type":"integer","minimum":4,"maximum":12},
        "sources_used":  {"type":"integer","minimum":0}
    },
    "additionalProperties": false
})";

}  // namespace

int main(int argc, char** argv) {
    std::string db = argc > 1 ? argv[1] : "/tmp/ms_asclepius_demo.db";
    std::filesystem::remove(db);
    std::filesystem::remove(std::filesystem::path{db}.replace_extension(".key"));

    auto rt = Runtime::open(db);
    if (!rt) {
        fmt::print(stderr, "open: {}\n", rt.error().what());
        return 1;
    }
    auto& runtime = rt.value();

    runtime.policies().push(make_phi_scrubber());
    runtime.policies().push(make_length_limit(/*input_max=*/16000,
                                              /*output_max=*/2000));
    runtime.policies().push(make_schema_validator(kSummarySchema));

    auto patient = PatientId::pseudonymous("p_ms_demo_4f02");
    // The MS sample is patient-facing documentation; the closest
    // canonical purpose in our enum is ambient_documentation.
    auto tok = runtime.consent().grant(patient,
                                       {Purpose::ambient_documentation},
                                       std::chrono::seconds{3600});
    if (!tok) {
        fmt::print(stderr, "consent: {}\n", tok.error().what());
        return 1;
    }

    auto inf = runtime.begin_inference({
        .model            = ModelId{"ms-asclepius:semantic-kernel", "v1"},
        .actor            = ActorId::clinician("dr.park"),
        .patient          = patient,
        .encounter        = EncounterId::make(),
        .purpose          = Purpose::ambient_documentation,
        .tenant           = TenantId{"demo_clinic"},
        .consent_token_id = tok.value().token_id,
    });
    if (!inf) {
        fmt::print(stderr, "begin: {}\n", inf.error().what());
        return 1;
    }

    // The real MS sample receives encounter notes + chief complaint + lab
    // history + clinical assessments. We replicate the shape so the PHI
    // scrubber finds something to do (MRN, phone, name in the chief
    // complaint string).
    nlohmann::json context = {
        {"chief", "F/u for high cholesterol; pt John Doe MRN:00112233 "
                  "reports adherence to atorvastatin 20mg."},
        {"hx",    "HTN x 8y, hyperlipidemia x 4y. Phone (415) 555-9988."},
        {"labs", {
            {{"name","Total Cholesterol"}, {"value", 215}, {"unit","mg/dL"}},
            {{"name","LDL"},               {"value", 138}, {"unit","mg/dL"}},
            {{"name","HDL"},               {"value",  52}, {"unit","mg/dL"}},
            {{"name","ALT"},               {"value",  24}, {"unit","U/L"}},
            {{"name","Cr"},                {"value",  0.9},{"unit","mg/dL"}},
        }},
    };

    auto out = inf.value().run(context.dump(), fake_ms_asclepius_summary);
    if (!out) {
        fmt::print(stderr, "run: {}\n", out.error().what());
        return 1;
    }
    fmt::print("ms-asclepius summary (input PHI-scrubbed, output schema-validated):\n{}\n\n",
               out.value());

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
