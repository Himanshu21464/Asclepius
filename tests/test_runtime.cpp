// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include <doctest/doctest.h>

#include "asclepius/asclepius.hpp"

#include <filesystem>
#include <random>

using namespace asclepius;
using namespace std::chrono_literals;

namespace {

std::filesystem::path tmp_db(const char* tag) {
    auto p = std::filesystem::temp_directory_path()
           / ("asclepius_rt_" + std::string{tag} + "_"
              + std::to_string(std::random_device{}()) + ".db");
    std::filesystem::remove(p);
    std::filesystem::remove(std::filesystem::path{p}.replace_extension(".key"));
    return p;
}

}  // namespace

TEST_CASE("Runtime end-to-end inference: blocked without consent") {
    auto rt = Runtime::open(tmp_db("noconsent"));
    REQUIRE(rt);

    auto inf = rt.value().begin_inference({
        .model     = ModelId{"scribe", "v3"},
        .actor     = ActorId::clinician("smith"),
        .patient   = PatientId::pseudonymous("p"),
        .encounter = EncounterId::make(),
        .purpose   = Purpose::ambient_documentation,
    });
    REQUIRE(!inf);
    CHECK(inf.error().code() == ErrorCode::consent_missing);
}

TEST_CASE("Runtime end-to-end inference: PHI scrubbed, ledger committed") {
    auto rt = Runtime::open(tmp_db("happy")).value();
    rt.policies().push(make_phi_scrubber());

    auto pid = PatientId::pseudonymous("p_happy");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h);
    REQUIRE(tok);

    auto inf = rt.begin_inference({
        .model            = ModelId{"scribe", "v3"},
        .actor            = ActorId::clinician("smith"),
        .patient          = pid,
        .encounter        = EncounterId::make(),
        .purpose          = Purpose::ambient_documentation,
        .tenant           = TenantId{},
        .consent_token_id = tok.value().token_id,
    });
    REQUIRE(inf);

    auto out = inf.value().run("phone 415-555-1234",
                               [](std::string in) -> Result<std::string> { return in; });
    REQUIRE(out);
    CHECK(out.value().find("415-555-1234") == std::string::npos);
    CHECK(out.value().find("[REDACTED:phone]") != std::string::npos);

    REQUIRE(inf.value().commit());
    CHECK(rt.ledger().length() >= 1);
    REQUIRE(rt.ledger().verify());
}

TEST_CASE("Runtime: model error is captured as model_error in ledger") {
    auto rt = Runtime::open(tmp_db("modelerr")).value();

    auto pid = PatientId::pseudonymous("p_err");
    auto tok = rt.consent().grant(pid, {Purpose::diagnostic_suggestion}, 1h);
    REQUIRE(tok);

    auto inf = rt.begin_inference({
        .model            = ModelId{"diag", "v1"},
        .actor            = ActorId::clinician("smith"),
        .patient          = pid,
        .encounter        = EncounterId::make(),
        .purpose          = Purpose::diagnostic_suggestion,
        .tenant           = TenantId{},
        .consent_token_id = tok.value().token_id,
    });
    REQUIRE(inf);

    auto out = inf.value().run("anything",
        [](std::string) -> Result<std::string> { return Error::internal("model exploded"); });
    REQUIRE(!out);
    CHECK(out.error().code() == ErrorCode::internal);

    REQUIRE(inf.value().commit());
    REQUIRE(rt.ledger().verify());
}

TEST_CASE("Runtime: output policy block is recorded with status=blocked.output") {
    auto rt = Runtime::open(tmp_db("blocked")).value();
    rt.policies().push(make_length_limit(0, /*output_max=*/4));

    auto pid = PatientId::pseudonymous("pb");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h);
    REQUIRE(tok);

    auto inf = rt.begin_inference({
        .model            = ModelId{"scribe", "v3"},
        .actor            = ActorId::clinician("smith"),
        .patient          = pid,
        .encounter        = EncounterId::make(),
        .purpose          = Purpose::ambient_documentation,
        .tenant           = TenantId{},
        .consent_token_id = tok.value().token_id,
    });
    REQUIRE(inf);

    auto out = inf.value().run("hi",
        [](std::string) -> Result<std::string> { return std::string{"this output is too long"}; });
    REQUIRE(!out);
    CHECK(out.error().code() == ErrorCode::policy_violation);
    REQUIRE(inf.value().commit());
}
