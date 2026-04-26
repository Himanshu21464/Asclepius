// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include <doctest/doctest.h>

#include "asclepius/policy.hpp"
#include "asclepius/runtime.hpp"

using namespace asclepius;

namespace {

InferenceContext make_ctx() {
    return InferenceContext{
        "inf_test",
        ModelId{"scribe", "v3"},
        ActorId::clinician("smith"),
        PatientId::pseudonymous("p1"),
        EncounterId::make(),
        Purpose::ambient_documentation,
        TenantId{},
        Time::now(),
    };
}

}  // namespace

TEST_CASE("PHI scrubber redacts SSN, phone, email, MRN") {
    auto ctx    = make_ctx();
    auto p      = make_phi_scrubber();
    auto in     = std::string{
        "Pt John Doe MRN:12345678 ssn 123-45-6789 phone (415) 555-1234, "
        "email john@example.com"
    };
    auto out    = p->evaluate_input(ctx, in);
    REQUIRE(out.decision == PolicyDecision::modify);
    REQUIRE(out.modified_payload.has_value());
    CHECK(out.modified_payload->find("123-45-6789") == std::string::npos);
    CHECK(out.modified_payload->find("john@example.com") == std::string::npos);
    CHECK(out.modified_payload->find("[REDACTED:") != std::string::npos);
    CHECK(out.violations.size() >= 3);
}

TEST_CASE("PHI scrubber leaves clean text alone") {
    auto ctx = make_ctx();
    auto p   = make_phi_scrubber();
    auto out = p->evaluate_input(ctx, "Patient reports chest pain after exertion.");
    CHECK(out.decision == PolicyDecision::allow);
    CHECK(!out.modified_payload.has_value());
}

TEST_CASE("Schema validator blocks invalid JSON") {
    auto ctx = make_ctx();
    auto p   = make_schema_validator(R"({"type":"object","required":["soap"]})");
    auto v1  = p->evaluate_output(ctx, R"({"soap":"S: ..."})");
    CHECK(v1.decision == PolicyDecision::allow);

    auto v2  = p->evaluate_output(ctx, R"({"foo":"bar"})");
    CHECK(v2.decision == PolicyDecision::block);

    auto v3  = p->evaluate_output(ctx, "not even json");
    CHECK(v3.decision == PolicyDecision::block);
}

TEST_CASE("Schema validator enforces min/max length") {
    auto ctx = make_ctx();
    auto p   = make_schema_validator(
        R"({"type":"object","properties":{"s":{"type":"string","maxLength":3}}})");
    auto v1 = p->evaluate_output(ctx, R"({"s":"ok"})");
    CHECK(v1.decision == PolicyDecision::allow);
    auto v2 = p->evaluate_output(ctx, R"({"s":"too long"})");
    CHECK(v2.decision == PolicyDecision::block);
}

TEST_CASE("Clinical action filter blocks non-allowlisted codes") {
    auto ctx = make_ctx();
    auto p   = make_clinical_action_filter({"order_lab", "send_message"});
    auto v1  = p->evaluate_output(ctx,
        R"({"actions":[{"code":"order_lab","details":{}}]})");
    CHECK(v1.decision == PolicyDecision::allow);

    auto v2 = p->evaluate_output(ctx,
        R"({"actions":[{"code":"prescribe_opioid","details":{}}]})");
    CHECK(v2.decision == PolicyDecision::block);
}

TEST_CASE("Length limit blocks oversize inputs/outputs") {
    auto ctx = make_ctx();
    auto p   = make_length_limit(/*input_max=*/8, /*output_max=*/8);
    CHECK(p->evaluate_input(ctx,  "ok").decision == PolicyDecision::allow);
    CHECK(p->evaluate_input(ctx,  "this is too long").decision == PolicyDecision::block);
    CHECK(p->evaluate_output(ctx, "ok").decision == PolicyDecision::allow);
    CHECK(p->evaluate_output(ctx, "this is too long").decision == PolicyDecision::block);
}

TEST_CASE("PolicyChain MODIFY then ALLOW propagates payload") {
    auto ctx = make_ctx();
    PolicyChain chain;
    chain.push(make_phi_scrubber());

    auto out = chain.evaluate_input(ctx, std::string{"phone (415) 555-1234"});
    REQUIRE(out);
    CHECK(out.value().find("[REDACTED:phone]") != std::string::npos);
}

TEST_CASE("PolicyChain BLOCK short-circuits with policy_violation error") {
    auto ctx = make_ctx();
    PolicyChain chain;
    chain.push(make_length_limit(8, 8));
    auto out = chain.evaluate_input(ctx, std::string{"this is too long"});
    REQUIRE(!out);
    CHECK(out.error().code() == ErrorCode::policy_violation);
}
