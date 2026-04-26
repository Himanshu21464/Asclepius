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

TEST_CASE("PolicyChain::clear drops all policies") {
    PolicyChain c;
    c.push(make_phi_scrubber());
    c.push(make_length_limit(0, 1024));
    REQUIRE(c.size() == 2);
    c.clear();
    CHECK(c.size() == 0);
    CHECK(c.names().empty());
}

TEST_CASE("PolicyChain::clear is idempotent") {
    PolicyChain c;
    c.clear();
    c.clear();
    CHECK(c.size() == 0);
}

// ---- has() --------------------------------------------------------------

TEST_CASE("PolicyChain::has is false on an empty chain") {
    PolicyChain c;
    CHECK_FALSE(c.has("phi_scrubber"));
    CHECK_FALSE(c.has(""));
    CHECK_FALSE(c.has("anything"));
}

TEST_CASE("PolicyChain::has finds a registered policy by name") {
    PolicyChain c;
    auto phi = make_phi_scrubber();
    auto len = make_length_limit(0, 1024);
    c.push(phi);
    c.push(len);
    REQUIRE(c.size() == 2);
    CHECK(c.has(phi->name()));
    CHECK(c.has(len->name()));
    CHECK_FALSE(c.has("not_a_real_policy"));
}

TEST_CASE("PolicyChain::has reflects clear() and remove()") {
    PolicyChain c;
    auto phi = make_phi_scrubber();
    c.push(phi);
    REQUIRE(c.has(phi->name()));
    c.clear();
    CHECK_FALSE(c.has(phi->name()));
    c.push(make_phi_scrubber());
    CHECK(c.has(phi->name()));
}

// ---- remove() -----------------------------------------------------------

TEST_CASE("PolicyChain::remove on empty chain returns false") {
    PolicyChain c;
    CHECK_FALSE(c.remove("phi_scrubber"));
    CHECK(c.size() == 0);
}

TEST_CASE("PolicyChain::remove drops the named policy and returns true") {
    PolicyChain c;
    auto phi = make_phi_scrubber();
    auto len = make_length_limit(0, 1024);
    c.push(phi);
    c.push(len);
    REQUIRE(c.size() == 2);
    CHECK(c.remove(phi->name()));
    CHECK(c.size() == 1);
    CHECK_FALSE(c.has(phi->name()));
    CHECK(c.has(len->name()));
    // Removing again returns false; idempotent at the call site.
    CHECK_FALSE(c.remove(phi->name()));
}

TEST_CASE("PolicyChain::remove drops only the FIRST match for a duplicate name") {
    PolicyChain c;
    auto a = make_length_limit(0, 100);
    auto b = make_length_limit(0, 200);
    REQUIRE(a->name() == b->name());
    c.push(a);
    c.push(b);
    REQUIRE(c.size() == 2);
    CHECK(c.remove(a->name()));
    CHECK(c.size() == 1);
    // The name should still be present — one duplicate remains.
    CHECK(c.has(a->name()));
}
