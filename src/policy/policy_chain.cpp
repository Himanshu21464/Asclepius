// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include "asclepius/policy.hpp"

#include "asclepius/runtime.hpp"

#include <fmt/core.h>

namespace asclepius {

const char* to_string(PolicyDecision d) noexcept {
    switch (d) {
        case PolicyDecision::allow:  return "allow";
        case PolicyDecision::modify: return "modify";
        case PolicyDecision::block:  return "block";
    }
    return "unknown";
}

void PolicyChain::push(std::shared_ptr<IPolicy> policy) {
    if (policy) {
        policies_.push_back(std::move(policy));
    }
}

std::size_t PolicyChain::size() const noexcept {
    return policies_.size();
}

void PolicyChain::clear() {
    policies_.clear();
}

bool PolicyChain::has(std::string_view name) const noexcept {
    for (const auto& p : policies_) {
        if (p && p->name() == name) return true;
    }
    return false;
}

bool PolicyChain::remove(std::string_view name) {
    for (auto it = policies_.begin(); it != policies_.end(); ++it) {
        if (*it && (*it)->name() == name) {
            policies_.erase(it);
            return true;
        }
    }
    return false;
}

std::vector<std::string> PolicyChain::names() const {
    std::vector<std::string> out;
    out.reserve(policies_.size());
    for (const auto& p : policies_) {
        out.emplace_back(p->name());
    }
    return out;
}

namespace {

Result<std::string> run_chain(const std::vector<std::shared_ptr<IPolicy>>& policies,
                              const InferenceContext&                       ctx,
                              std::string                                   payload,
                              bool                                          is_input) {
    for (const auto& p : policies) {
        const auto verdict = is_input
                                 ? p->evaluate_input(ctx, payload)
                                 : p->evaluate_output(ctx, payload);

        switch (verdict.decision) {
            case PolicyDecision::allow:
                continue;
            case PolicyDecision::modify:
                if (verdict.modified_payload) {
                    payload = *verdict.modified_payload;
                }
                continue;
            case PolicyDecision::block: {
                std::string detail = std::string{p->name()};
                if (!verdict.rationale.empty()) {
                    detail += ": " + verdict.rationale;
                }
                return Error::policy(std::move(detail));
            }
        }
    }
    return payload;
}

}  // namespace

Result<std::string> PolicyChain::evaluate_input(const InferenceContext& ctx,
                                                std::string             payload) const {
    return run_chain(policies_, ctx, std::move(payload), /*is_input=*/true);
}

Result<std::string> PolicyChain::evaluate_output(const InferenceContext& ctx,
                                                 std::string             payload) const {
    return run_chain(policies_, ctx, std::move(payload), /*is_input=*/false);
}

}  // namespace asclepius
