// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
//
// Worked example: generate and verify an evidence bundle.
//
// Run:
//   ./example_evidence_bundle /tmp/asclepius_demo.db /tmp/evidence.tar
//   tar tvf /tmp/evidence.tar       # inspect
//   ./asclepius evidence verify /tmp/evidence.tar

#include <fmt/core.h>
#include <nlohmann/json.hpp>

#include "asclepius/asclepius.hpp"

#include <chrono>
#include <filesystem>

using namespace asclepius;
using namespace std::chrono_literals;

int main(int argc, char** argv) {
    if (argc < 3) {
        fmt::print(stderr, "usage: example_evidence_bundle <db> <out.tar>\n");
        return 1;
    }
    std::string db = argv[1];
    std::filesystem::path out  = argv[2];

    std::filesystem::remove(db);
    std::filesystem::remove(std::filesystem::path{db}.replace_extension(".key"));

    auto rt = Runtime::open(db).value();
    auto pid = PatientId::pseudonymous("p_evidence");
    auto tok = rt.consent().grant(pid, {Purpose::diagnostic_suggestion}, 1h).value();

    for (int i = 0; i < 25; ++i) {
        auto inf = rt.begin_inference({
            .model            = ModelId{"diag", "v2"},
            .actor            = ActorId::clinician("dr.smith"),
            .patient          = pid,
            .encounter        = EncounterId::make(),
            .purpose          = Purpose::diagnostic_suggestion,
            .tenant           = TenantId{"demo"},
            .consent_token_id = tok.token_id,
        }).value();

        nlohmann::json out_body = {{"predicted_label", i % 5 == 0 ? "ACS" : "non-ACS"},
                                    {"score",           0.5 + 0.4 * ((i % 7) / 7.0)}};

        (void)inf.run("vitals + ecg",
            [&](std::string) -> Result<std::string> { return out_body.dump(); });
        (void)inf.commit();

        if (i % 5 == 0) {
            (void)rt.evaluation().attach_ground_truth({
                .inference_id = "(unknown — example doesn't track ids yet)",
                .truth        = nlohmann::json{{"label","ACS"}},
                .captured_at  = Time::now(),
                .source       = "registry",
            });
        }
    }

    auto window = EvaluationWindow{
        Time::now() - 1h,
        Time::now() + 1h,
    };
    auto bundle = rt.evaluation().export_bundle(window, out);
    if (!bundle) { fmt::print(stderr, "export: {}\n", bundle.error().what()); return 1; }

    fmt::print("wrote {}\n  root_hash={}\n  models={}\n",
               bundle.value().path.string(),
               bundle.value().root_hash.hex(),
               bundle.value().per_model.size());

    auto v = EvaluationHarness::verify_bundle(out);
    if (!v) { fmt::print(stderr, "verify: {}\n", v.error().what()); return 1; }
    fmt::print("bundle verified ok\n");
    return 0;
}
