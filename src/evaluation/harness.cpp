// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include "asclepius/evaluation.hpp"

#include "asclepius/audit.hpp"
#include "asclepius/telemetry.hpp"

#include <fmt/core.h>
#include <nlohmann/json.hpp>

#include <unordered_map>

namespace asclepius {

EvaluationHarness::EvaluationHarness(Ledger& ledger, DriftMonitor& drift)
    : ledger_(ledger), drift_(drift) {}

Result<void> EvaluationHarness::attach_ground_truth(GroundTruth t) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        ground_truth_[t.inference_id] = t;
    }

    nlohmann::json body;
    body["inference_id"] = t.inference_id;
    body["truth"]        = t.truth;
    body["source"]       = t.source;
    body["captured_at"]  = t.captured_at.iso8601();

    auto e = ledger_.append("evaluation.ground_truth", t.source, std::move(body));
    if (!e) return e.error();
    return Result<void>::ok();
}

Result<void> EvaluationHarness::capture_override(OverrideEvent ev) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        overrides_.push_back(ev);
    }

    nlohmann::json body;
    body["inference_id"] = ev.inference_id;
    body["rationale"]    = ev.rationale;
    body["corrected"]    = ev.corrected;
    body["clinician"]    = std::string(ev.clinician.str());
    body["occurred_at"]  = ev.occurred_at.iso8601();

    auto e = ledger_.append("evaluation.override",
                            std::string(ev.clinician.str()),
                            std::move(body));
    if (!e) return e.error();
    return Result<void>::ok();
}

Result<std::vector<ModelMetrics>> EvaluationHarness::metrics(EvaluationWindow window) const {
    auto entries = ledger_.range(1, ledger_.length() + 1);
    if (!entries) return entries.error();

    struct Tally {
        std::uint64_t n_inferences = 0;
        std::uint64_t n_overrides  = 0;
        std::uint64_t n_blocked    = 0;
        std::uint64_t n_truth      = 0;
        std::uint64_t tp = 0, fp = 0, tn = 0, fn = 0;
    };
    std::unordered_map<std::string, Tally> per_model;
    std::unordered_map<std::string, std::string> inf_to_model;
    std::unordered_map<std::string, nlohmann::json> inf_to_output_summary;

    auto in_window = [&](const Time& t) {
        return t >= window.start && t < window.end;
    };

    // First pass: index inferences and tallies.
    for (const auto& e : entries.value()) {
        if (!in_window(e.header.ts)) continue;
        nlohmann::json body;
        try { body = nlohmann::json::parse(e.body_json); } catch (...) { continue; }

        if (e.header.event_type == "inference.committed") {
            auto model = body.value("model", std::string{});
            auto inf   = body.value("inference_id", std::string{});
            auto& t    = per_model[model];
            ++t.n_inferences;
            inf_to_model[inf] = model;
            if (body.value("status", std::string{}) == "blocked.output" ||
                body.value("status", std::string{}) == "blocked.input") {
                ++t.n_blocked;
            }
            inf_to_output_summary[inf] = body;
        } else if (e.header.event_type == "evaluation.override") {
            auto inf = body.value("inference_id", std::string{});
            auto it  = inf_to_model.find(inf);
            if (it != inf_to_model.end()) ++per_model[it->second].n_overrides;
        } else if (e.header.event_type == "evaluation.ground_truth") {
            auto inf = body.value("inference_id", std::string{});
            auto it  = inf_to_model.find(inf);
            if (it != inf_to_model.end()) {
                auto& t = per_model[it->second];
                ++t.n_truth;

                // Best-effort accuracy: if both output_hash and a "result"
                // field exist on truth, compare. This is intentionally
                // primitive — real evaluation belongs to the customer's
                // adjudication pipeline and reads from the bundle.
                const auto& truth     = body["truth"];
                const auto& inf_body  = inf_to_output_summary[inf];
                if (truth.is_object() && truth.contains("label") &&
                    inf_body.contains("predicted_label")) {
                    const auto truth_l = truth["label"];
                    const auto pred_l  = inf_body["predicted_label"];
                    if (truth_l == pred_l) ++t.tp; else ++t.fp;
                }
            }
        }
    }

    std::vector<ModelMetrics> out;
    out.reserve(per_model.size());
    for (auto& [model, t] : per_model) {
        ModelMetrics m;
        m.model         = model;
        m.n_inferences  = t.n_inferences;
        m.n_overrides   = t.n_overrides;
        m.n_blocked     = t.n_blocked;
        m.n_with_truth  = t.n_truth;
        m.override_rate = t.n_inferences == 0
                              ? 0.0
                              : static_cast<double>(t.n_overrides) /
                                    static_cast<double>(t.n_inferences);
        const std::uint64_t denom = t.tp + t.fp + t.tn + t.fn;
        m.accuracy    = denom == 0 ? 0.0
                                   : static_cast<double>(t.tp + t.tn) / static_cast<double>(denom);
        const std::uint64_t pos = t.tp + t.fn;
        const std::uint64_t neg = t.tn + t.fp;
        m.sensitivity = pos == 0 ? 0.0 : static_cast<double>(t.tp) / static_cast<double>(pos);
        m.specificity = neg == 0 ? 0.0 : static_cast<double>(t.tn) / static_cast<double>(neg);
        out.push_back(std::move(m));
    }
    return out;
}

}  // namespace asclepius
