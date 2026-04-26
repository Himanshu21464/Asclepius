// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#ifndef ASCLEPIUS_POLICY_HPP
#define ASCLEPIUS_POLICY_HPP

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "asclepius/core.hpp"

namespace asclepius {

class InferenceContext;  // declared in runtime.hpp

// ---- Verdict -------------------------------------------------------------

enum class PolicyDecision : std::uint8_t {
    allow  = 0,  // pass through unchanged
    modify = 1,  // pass through with replacement payload
    block  = 2,  // halt the inference
};

const char* to_string(PolicyDecision) noexcept;

struct PolicyVerdict {
    PolicyDecision             decision = PolicyDecision::allow;
    std::optional<std::string> modified_payload{};
    std::string                rationale{};
    std::vector<std::string>   violations{};
};

// ---- Policy interface ---------------------------------------------------
//
// A Policy is a stateless (or thread-safe) inspector of inputs and outputs.
// Implementations override either or both of the two evaluators; default
// implementations allow.

class IPolicy {
public:
    virtual ~IPolicy() = default;

    virtual std::string_view name() const noexcept = 0;

    virtual PolicyVerdict evaluate_input(const InferenceContext&, std::string_view /*payload*/) {
        return PolicyVerdict{};
    }
    virtual PolicyVerdict evaluate_output(const InferenceContext&, std::string_view /*payload*/) {
        return PolicyVerdict{};
    }
};

// ---- Chain ---------------------------------------------------------------
//
// Policies compose left-to-right. The first BLOCK short-circuits with the
// blocker's rationale carried in the returned Error. MODIFY rewrites the
// payload visible to subsequent policies.

class PolicyChain {
public:
    void push(std::shared_ptr<IPolicy> policy);

    // Number of registered policies.
    std::size_t size() const noexcept;

    // Names of registered policies, in order.
    std::vector<std::string> names() const;

    // Drop all policies. Used by tests and live policy-reload operations.
    // Not thread-safe with concurrent evaluate_*() — caller is responsible
    // for ordering reload outside the inference hot path.
    void clear();

    // Evaluate against input. Returns the (possibly modified) payload, or the
    // first blocker's Error.
    Result<std::string> evaluate_input(const InferenceContext& ctx, std::string payload) const;

    // Same for output.
    Result<std::string> evaluate_output(const InferenceContext& ctx, std::string payload) const;

private:
    std::vector<std::shared_ptr<IPolicy>> policies_;
};

// ---- Built-in policies --------------------------------------------------

// Detects and replaces obvious PHI in free text: names, MRNs, SSNs, phone
// numbers, addresses, dates of birth. Conservative — favors false positives.
// Decision is MODIFY (replaces detected PHI with [REDACTED]).
std::shared_ptr<IPolicy> make_phi_scrubber();

// Validates a JSON payload against a JSON Schema (subset: type, required,
// properties, enum, minimum, maximum, minLength, maxLength). Decision is
// BLOCK on violation.
std::shared_ptr<IPolicy> make_schema_validator(std::string schema_json);

// Restricts allowed clinical actions in an output. Used when wrapping
// agentic tools that emit orders, prescriptions, or messages. Decision is
// BLOCK if the output contains a non-allowlisted action.
std::shared_ptr<IPolicy> make_clinical_action_filter(std::vector<std::string> allowed_action_codes);

// Maximum input/output length. Cheap, guards against runaway model output.
std::shared_ptr<IPolicy> make_length_limit(std::size_t input_max, std::size_t output_max);

}  // namespace asclepius

#endif  // ASCLEPIUS_POLICY_HPP
