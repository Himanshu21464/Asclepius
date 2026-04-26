// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
//
// Two output guards that operate on JSON-shaped agentic outputs:
//
//   make_clinical_action_filter   — block actions whose code is not on the
//                                   caller-supplied allowlist
//   make_length_limit             — bound input/output payload size
//
// Both are deliberately small and stateless. Hosts compose them into a
// PolicyChain alongside phi_scrubber and schema_validator.

#include "asclepius/policy.hpp"

#include <fmt/core.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace asclepius {

namespace {

class ActionFilter final : public IPolicy {
public:
    explicit ActionFilter(std::vector<std::string> allowed) : allowed_(std::move(allowed)) {}
    std::string_view name() const noexcept override { return "clinical_action_filter"; }

    PolicyVerdict evaluate_output(const InferenceContext&, std::string_view payload) override {
        nlohmann::json doc;
        try { doc = nlohmann::json::parse(payload); }
        catch (...) { return {}; }
        if (!doc.is_object() || !doc.contains("actions") || !doc["actions"].is_array()) {
            return {};
        }
        std::vector<std::string> bad;
        for (const auto& a : doc["actions"]) {
            if (!a.is_object() || !a.contains("code")) continue;
            const auto code = a["code"].get<std::string>();
            if (std::find(allowed_.begin(), allowed_.end(), code) == allowed_.end()) {
                bad.push_back(code);
            }
        }
        if (bad.empty()) return {};
        PolicyVerdict v;
        v.decision   = PolicyDecision::block;
        v.rationale  = "clinical action not on allowlist";
        v.violations = std::move(bad);
        return v;
    }

private:
    std::vector<std::string> allowed_;
};

class Limit final : public IPolicy {
public:
    Limit(std::size_t i, std::size_t o) : in_(i), out_(o) {}
    std::string_view name() const noexcept override { return "length_limit"; }

    PolicyVerdict evaluate_input(const InferenceContext&, std::string_view p) override {
        if (in_ && p.size() > in_) return block("input", p.size(), in_);
        return {};
    }
    PolicyVerdict evaluate_output(const InferenceContext&, std::string_view p) override {
        if (out_ && p.size() > out_) return block("output", p.size(), out_);
        return {};
    }

private:
    std::size_t in_;
    std::size_t out_;

    static PolicyVerdict block(const char* side, std::size_t got, std::size_t max) {
        PolicyVerdict v;
        v.decision   = PolicyDecision::block;
        v.rationale  = fmt::format("{} length {} exceeds limit {}", side, got, max);
        v.violations = {std::string{side} + "_too_long"};
        return v;
    }
};

}  // namespace

std::shared_ptr<IPolicy> make_clinical_action_filter(std::vector<std::string> allowed) {
    return std::make_shared<ActionFilter>(std::move(allowed));
}

std::shared_ptr<IPolicy> make_length_limit(std::size_t input_max, std::size_t output_max) {
    return std::make_shared<Limit>(input_max, output_max);
}

}  // namespace asclepius
