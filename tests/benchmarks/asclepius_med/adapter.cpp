// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include "adapter.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>

namespace asclepius::bench::asclepius_med {

asclepius::Result<std::vector<BenchItem>>
load_fixture(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        return asclepius::Error::invalid("fixture not found: " + path);
    }
    std::stringstream ss;
    ss << in.rdbuf();

    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(ss.str());
    } catch (const std::exception& e) {
        return asclepius::Error::invalid(std::string{"fixture parse: "} + e.what());
    }
    if (!doc.contains("items") || !doc["items"].is_array()) {
        return asclepius::Error::invalid("fixture missing items[] array");
    }

    std::vector<BenchItem> out;
    out.reserve(doc["items"].size());
    for (auto& it : doc["items"]) {
        if (!it.contains("id") || !it.contains("specialty") ||
            !it.contains("category") || !it.contains("sub_task") ||
            !it.contains("prompt")) {
            return asclepius::Error::invalid("fixture item missing required field");
        }
        out.push_back({
            it.value("id", std::string{}),
            it.value("specialty", std::string{}),
            it.value("category", std::string{}),
            it.value("sub_task", std::string{}),
            it.value("prompt", std::string{}),
        });
    }
    return out;
}

asclepius::Result<std::string>
drive_one(asclepius::Runtime& runtime,
          const BenchItem& item,
          const asclepius::ConsentToken& token,
          asclepius::PatientId patient,
          asclepius::ActorId actor,
          asclepius::TenantId tenant,
          asclepius::ModelId model,
          std::function<asclepius::Result<std::string>(std::string)> call_model) {
    auto inf_r = runtime.begin_inference({
        .model            = model,
        .actor            = actor,
        .patient          = patient,
        .encounter        = asclepius::EncounterId::make(),
        .purpose          = asclepius::Purpose::diagnostic_suggestion,
        .tenant           = tenant,
        .consent_token_id = token.token_id,
    });
    if (!inf_r) return inf_r.error();
    auto& inf = inf_r.value();

    // Tag the inference body with the benchmark item identifiers so a
    // regulator can subset by specialty / sub_task without re-running
    // the model. The values are bound into payload_hash and the chain.
    auto m1 = inf.add_metadata("benchmark", std::string{"asclepius-med"});
    if (!m1) return m1.error();
    auto m2 = inf.add_metadata("bench_item_id", item.id);
    if (!m2) return m2.error();
    auto m3 = inf.add_metadata("specialty", item.specialty);
    if (!m3) return m3.error();
    auto m4 = inf.add_metadata("category", item.category);
    if (!m4) return m4.error();
    auto m5 = inf.add_metadata("sub_task", item.sub_task);
    if (!m5) return m5.error();

    auto out = inf.run(item.prompt, call_model);
    if (!out) return out.error();

    auto c = inf.commit();
    if (!c) return c.error();

    return out;
}

}  // namespace asclepius::bench::asclepius_med
