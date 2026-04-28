// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include <doctest/doctest.h>
#include "asclepius/consent.hpp"
#include "asclepius/identity.hpp"
#include "asclepius/core.hpp"
#include <chrono>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace asclepius;
using namespace std::chrono_literals;

namespace {

// ---- helpers ------------------------------------------------------------

ConsentToken make_active_token(const PatientId&     patient,
                               std::string          token_id,
                               std::vector<Purpose> purposes,
                               std::chrono::seconds ttl = 1h) {
    ConsentToken t;
    t.token_id   = std::move(token_id);
    t.patient    = patient;
    t.purposes   = std::move(purposes);
    t.issued_at  = Time::now();
    t.expires_at = t.issued_at + std::chrono::nanoseconds{ttl};
    t.revoked    = false;
    return t;
}

ConsentArtefact make_basic_artefact(std::string id = "art_001") {
    ConsentArtefact a;
    a.artefact_id    = std::move(id);
    a.patient        = PatientId::pseudonymous("p_artefact");
    a.requester_id   = "hiu:hospital_a";
    a.fetcher_id     = "hip:lab_b";
    a.purposes       = {Purpose::triage, Purpose::second_opinion};
    a.issued_at      = Time::now();
    a.expires_at     = a.issued_at + std::chrono::nanoseconds{24h};
    a.status         = ConsentArtefact::Status::granted;
    a.schema_version = "1.0";
    return a;
}

// Build a minimal valid ABDM payload as a JSON object so individual tests
// can mutate one field at a time and re-serialise.
nlohmann::json make_valid_payload(std::string id = "art_payload") {
    nlohmann::json j;
    j["artefact_id"]    = std::move(id);
    j["patient"]        = "pat:p_payload";
    j["requester_id"]   = "hiu:a";
    j["fetcher_id"]     = "hip:b";
    j["purposes"]       = nlohmann::json::array({"triage", "medication_review"});
    j["issued_at"]      = Time::now().iso8601();
    j["expires_at"]     = (Time::now() + std::chrono::nanoseconds{1h}).iso8601();
    j["status"]         = "granted";
    j["schema_version"] = "1.0";
    return j;
}

}  // namespace

// ---- Status <-> string -------------------------------------------------

TEST_CASE("artefact: to_string covers all three valid statuses") {
    CHECK(std::string{to_string(ConsentArtefact::Status::granted)} == "granted");
    CHECK(std::string{to_string(ConsentArtefact::Status::revoked)} == "revoked");
    CHECK(std::string{to_string(ConsentArtefact::Status::expired)} == "expired");
}

TEST_CASE("artefact: to_string falls through to unknown for invalid cast") {
    auto bogus = static_cast<ConsentArtefact::Status>(99);
    CHECK(std::string{to_string(bogus)} == "unknown");
}

TEST_CASE("artefact: artefact_status_from_string round-trips all valid values") {
    for (auto s : {ConsentArtefact::Status::granted,
                   ConsentArtefact::Status::revoked,
                   ConsentArtefact::Status::expired}) {
        auto r = artefact_status_from_string(to_string(s));
        REQUIRE(r);
        CHECK(r.value() == s);
    }
}

TEST_CASE("artefact: artefact_status_from_string rejects unknown literal") {
    auto r = artefact_status_from_string("not_a_status");
    REQUIRE(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("artefact: artefact_status_from_string rejects empty input") {
    auto r = artefact_status_from_string("");
    REQUIRE(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("artefact: artefact_status_from_string is case sensitive") {
    auto r = artefact_status_from_string("GRANTED");
    REQUIRE(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
}

// ---- to_abdm_json shape ------------------------------------------------

TEST_CASE("artefact: to_abdm_json emits all required top-level fields") {
    auto a = make_basic_artefact();
    auto j = nlohmann::json::parse(to_abdm_json(a));
    REQUIRE(j.is_object());
    CHECK(j.contains("artefact_id"));
    CHECK(j.contains("patient"));
    CHECK(j.contains("requester_id"));
    CHECK(j.contains("fetcher_id"));
    CHECK(j.contains("purposes"));
    CHECK(j.contains("issued_at"));
    CHECK(j.contains("expires_at"));
    CHECK(j.contains("status"));
    CHECK(j.contains("schema_version"));
}

TEST_CASE("artefact: to_abdm_json field types are wire-stable") {
    auto a = make_basic_artefact();
    auto j = nlohmann::json::parse(to_abdm_json(a));
    CHECK(j["artefact_id"].is_string());
    CHECK(j["patient"].is_string());
    CHECK(j["requester_id"].is_string());
    CHECK(j["fetcher_id"].is_string());
    CHECK(j["purposes"].is_array());
    CHECK(j["issued_at"].is_string());
    CHECK(j["expires_at"].is_string());
    CHECK(j["status"].is_string());
    CHECK(j["schema_version"].is_string());
}

TEST_CASE("artefact: to_abdm_json stringifies purposes via to_string(Purpose)") {
    auto a = make_basic_artefact();
    a.purposes = {Purpose::triage, Purpose::second_opinion};
    auto j = nlohmann::json::parse(to_abdm_json(a));
    REQUIRE(j["purposes"].is_array());
    REQUIRE(j["purposes"].size() == 2);
    CHECK(j["purposes"][0].get<std::string>() == "triage");
    CHECK(j["purposes"][1].get<std::string>() == "second_opinion");
}

TEST_CASE("artefact: to_abdm_json defaults schema_version to 1.0 when blank") {
    auto a = make_basic_artefact("art_blank_sv");
    a.schema_version.clear();
    auto j = nlohmann::json::parse(to_abdm_json(a));
    REQUIRE(j.contains("schema_version"));
    CHECK(j["schema_version"].get<std::string>() == "1.0");
}

// ---- from_abdm_json + JSON round trip ----------------------------------

TEST_CASE("artefact: to_abdm_json + from_abdm_json round-trips every field") {
    auto a = make_basic_artefact("art_round_trip");
    a.purposes = {Purpose::triage,
                  Purpose::second_opinion,
                  Purpose::specialist_referral,
                  Purpose::longitudinal_outcomes_research};
    a.status = ConsentArtefact::Status::revoked;

    auto s = to_abdm_json(a);
    auto r = from_abdm_json(s);
    REQUIRE(r);
    const auto& b = r.value();

    CHECK(b.artefact_id    == a.artefact_id);
    CHECK(b.patient.str()  == a.patient.str());
    CHECK(b.requester_id   == a.requester_id);
    CHECK(b.fetcher_id     == a.fetcher_id);
    REQUIRE(b.purposes.size() == a.purposes.size());
    for (std::size_t i = 0; i < a.purposes.size(); ++i) {
        CHECK(b.purposes[i] == a.purposes[i]);
    }
    CHECK(b.issued_at.iso8601()  == a.issued_at.iso8601());
    CHECK(b.expires_at.iso8601() == a.expires_at.iso8601());
    CHECK(b.status         == a.status);
    CHECK(b.schema_version == a.schema_version);
}

TEST_CASE("artefact: from_abdm_json rejects empty input") {
    auto r = from_abdm_json("");
    REQUIRE(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("artefact: from_abdm_json rejects malformed JSON text") {
    auto r = from_abdm_json("{not json");
    REQUIRE(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("artefact: from_abdm_json rejects non-object roots (array)") {
    auto r = from_abdm_json("[1,2,3]");
    REQUIRE(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("artefact: from_abdm_json rejects non-object roots (string)") {
    auto r = from_abdm_json("\"hello\"");
    REQUIRE(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("artefact: from_abdm_json rejects missing artefact_id") {
    auto j = make_valid_payload();
    j.erase("artefact_id");
    auto r = from_abdm_json(j.dump());
    REQUIRE(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
    CHECK(std::string{r.error().what()}.find("artefact_id") != std::string::npos);
}

TEST_CASE("artefact: from_abdm_json rejects missing patient") {
    auto j = make_valid_payload();
    j.erase("patient");
    auto r = from_abdm_json(j.dump());
    REQUIRE(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("artefact: from_abdm_json rejects missing purposes") {
    auto j = make_valid_payload();
    j.erase("purposes");
    auto r = from_abdm_json(j.dump());
    REQUIRE(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("artefact: from_abdm_json rejects missing status") {
    auto j = make_valid_payload();
    j.erase("status");
    auto r = from_abdm_json(j.dump());
    REQUIRE(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("artefact: from_abdm_json rejects unknown purpose strings") {
    auto j = make_valid_payload();
    j["purposes"] = nlohmann::json::array({"triage", "made_up_purpose"});
    auto r = from_abdm_json(j.dump());
    REQUIRE(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
    CHECK(std::string{r.error().what()}.find("made_up_purpose") != std::string::npos);
}

TEST_CASE("artefact: from_abdm_json rejects non-string entry inside purposes") {
    auto j = make_valid_payload();
    j["purposes"] = nlohmann::json::array({"triage", 42});
    auto r = from_abdm_json(j.dump());
    REQUIRE(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("artefact: from_abdm_json rejects unknown status string") {
    auto j = make_valid_payload();
    j["status"] = "wibble";
    auto r = from_abdm_json(j.dump());
    REQUIRE(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("artefact: from_abdm_json rejects wrong-typed artefact_id") {
    auto j = make_valid_payload();
    j["artefact_id"] = 123;
    auto r = from_abdm_json(j.dump());
    REQUIRE(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("artefact: from_abdm_json rejects purposes that are not an array") {
    auto j = make_valid_payload();
    j["purposes"] = "triage";
    auto r = from_abdm_json(j.dump());
    REQUIRE(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("artefact: from_abdm_json defaults schema_version to 1.0 when absent") {
    auto j = make_valid_payload();
    j.erase("schema_version");
    auto r = from_abdm_json(j.dump());
    REQUIRE(r);
    CHECK(r.value().schema_version == "1.0");
}

TEST_CASE("artefact: from_abdm_json accepts an explicit non-default schema_version") {
    auto j = make_valid_payload();
    j["schema_version"] = "2.5";
    auto r = from_abdm_json(j.dump());
    REQUIRE(r);
    CHECK(r.value().schema_version == "2.5");
}

// ---- artefact_from_token ------------------------------------------------

TEST_CASE("artefact: artefact_from_token copies identifiers and times from inputs") {
    auto p = PatientId::pseudonymous("p_active");
    auto t = make_active_token(p, "tok_active",
                               {Purpose::triage, Purpose::medication_review},
                               1h);

    auto a = artefact_from_token(t, "hiu:a", "hip:b", "art_a");
    CHECK(a.artefact_id   == "art_a");
    CHECK(a.requester_id  == "hiu:a");
    CHECK(a.fetcher_id    == "hip:b");
    CHECK(a.patient.str() == p.str());
    REQUIRE(a.purposes.size() == 2);
    CHECK(a.purposes[0]   == Purpose::triage);
    CHECK(a.purposes[1]   == Purpose::medication_review);
    CHECK(a.issued_at     == t.issued_at);
    CHECK(a.expires_at    == t.expires_at);
    CHECK(a.schema_version == "1.0");
}

TEST_CASE("artefact: artefact_from_token marks an active token as granted") {
    auto p = PatientId::pseudonymous("p_g");
    auto t = make_active_token(p, "tok_g", {Purpose::triage}, 1h);
    auto a = artefact_from_token(t, "hiu:a", "hip:b", "art_g");
    CHECK(a.status == ConsentArtefact::Status::granted);
}

TEST_CASE("artefact: artefact_from_token marks a revoked token as revoked") {
    auto p = PatientId::pseudonymous("p_r");
    auto t = make_active_token(p, "tok_r",
                               {Purpose::medication_review}, 1h);
    t.revoked = true;
    auto a = artefact_from_token(t, "hiu:a", "hip:b", "art_r");
    CHECK(a.status == ConsentArtefact::Status::revoked);
}

TEST_CASE("artefact: artefact_from_token marks an expired token as expired") {
    ConsentToken t;
    t.token_id   = "tok_expired";
    t.patient    = PatientId::pseudonymous("p_e");
    t.purposes   = {Purpose::quality_improvement};
    t.issued_at  = Time::now() - std::chrono::nanoseconds{2h};
    t.expires_at = Time::now() - std::chrono::nanoseconds{1h};
    t.revoked    = false;

    auto a = artefact_from_token(t, "hiu:a", "hip:b", "art_e");
    CHECK(a.status == ConsentArtefact::Status::expired);
}

TEST_CASE("artefact: artefact_from_token prefers revoked over expired") {
    ConsentToken t;
    t.token_id   = "tok_re";
    t.patient    = PatientId::pseudonymous("p_re");
    t.purposes   = {Purpose::triage};
    t.issued_at  = Time::now() - std::chrono::nanoseconds{2h};
    t.expires_at = Time::now() - std::chrono::nanoseconds{1h};
    t.revoked    = true;

    auto a = artefact_from_token(t, "hiu:a", "hip:b", "art_re");
    CHECK(a.status == ConsentArtefact::Status::revoked);
}

// ---- token_from_artefact ------------------------------------------------

TEST_CASE("artefact: token_from_artefact preserves token_id, patient, purposes, times") {
    auto a = make_basic_artefact("art_t");
    a.purposes = {Purpose::triage, Purpose::specialist_referral};
    auto t = token_from_artefact(a);
    CHECK(t.token_id        == a.artefact_id);
    CHECK(t.patient.str()   == a.patient.str());
    REQUIRE(t.purposes.size() == a.purposes.size());
    for (std::size_t i = 0; i < a.purposes.size(); ++i) {
        CHECK(t.purposes[i] == a.purposes[i]);
    }
    CHECK(t.issued_at  == a.issued_at);
    CHECK(t.expires_at == a.expires_at);
}

TEST_CASE("artefact: token_from_artefact maps revoked status to revoked=true") {
    auto a = make_basic_artefact("art_rv");
    a.status = ConsentArtefact::Status::revoked;
    auto t = token_from_artefact(a);
    CHECK(t.revoked == true);
}

TEST_CASE("artefact: token_from_artefact maps granted status to revoked=false") {
    auto a = make_basic_artefact("art_gr");
    a.status = ConsentArtefact::Status::granted;
    auto t = token_from_artefact(a);
    CHECK(t.revoked == false);
}

TEST_CASE("artefact: token_from_artefact maps expired status to revoked=false") {
    auto a = make_basic_artefact("art_ex");
    a.status = ConsentArtefact::Status::expired;
    auto t = token_from_artefact(a);
    CHECK(t.revoked == false);
}

// ---- bidirectional round-trips -----------------------------------------

TEST_CASE("artefact: artefact -> token -> artefact preserves identity (granted)") {
    auto a1 = make_basic_artefact("art_atat");
    a1.purposes = {Purpose::triage, Purpose::risk_stratification};
    a1.status   = ConsentArtefact::Status::granted;
    // Use a long horizon so re-mapping does not flip granted->expired.
    a1.expires_at = Time::now() + std::chrono::nanoseconds{24h};

    auto t  = token_from_artefact(a1);
    auto a2 = artefact_from_token(t, a1.requester_id, a1.fetcher_id,
                                  a1.artefact_id);

    CHECK(a2.artefact_id   == a1.artefact_id);
    CHECK(a2.patient.str() == a1.patient.str());
    CHECK(a2.requester_id  == a1.requester_id);
    CHECK(a2.fetcher_id    == a1.fetcher_id);
    REQUIRE(a2.purposes.size() == a1.purposes.size());
    for (std::size_t i = 0; i < a1.purposes.size(); ++i) {
        CHECK(a2.purposes[i] == a1.purposes[i]);
    }
    CHECK(a2.issued_at  == a1.issued_at);
    CHECK(a2.expires_at == a1.expires_at);
    CHECK(a2.status     == a1.status);
}

TEST_CASE("artefact: artefact -> token -> artefact preserves identity (revoked)") {
    auto a1 = make_basic_artefact("art_atat_rv");
    a1.status     = ConsentArtefact::Status::revoked;
    a1.expires_at = Time::now() + std::chrono::nanoseconds{24h};

    auto t  = token_from_artefact(a1);
    auto a2 = artefact_from_token(t, a1.requester_id, a1.fetcher_id,
                                  a1.artefact_id);

    CHECK(a2.status == ConsentArtefact::Status::revoked);
}

TEST_CASE("artefact: token -> artefact -> token preserves identity") {
    ConsentToken t1;
    t1.token_id   = "tok_tat";
    t1.patient    = PatientId::pseudonymous("p_tat");
    t1.purposes   = {Purpose::triage,
                     Purpose::medication_review,
                     Purpose::second_opinion};
    t1.issued_at  = Time::now();
    t1.expires_at = t1.issued_at + std::chrono::nanoseconds{6h};
    t1.revoked    = false;

    auto a  = artefact_from_token(t1, "hiu:tat", "hip:tat", t1.token_id);
    auto t2 = token_from_artefact(a);

    CHECK(t2.token_id      == t1.token_id);
    CHECK(t2.patient.str() == t1.patient.str());
    REQUIRE(t2.purposes.size() == t1.purposes.size());
    for (std::size_t i = 0; i < t1.purposes.size(); ++i) {
        CHECK(t2.purposes[i] == t1.purposes[i]);
    }
    CHECK(t2.issued_at  == t1.issued_at);
    CHECK(t2.expires_at == t1.expires_at);
    CHECK(t2.revoked    == t1.revoked);
}

TEST_CASE("artefact: token -> artefact -> token preserves revoked flag") {
    auto p = PatientId::pseudonymous("p_tat_rv");
    auto t1 = make_active_token(p, "tok_tat_rv",
                                {Purpose::triage}, 1h);
    t1.revoked = true;

    auto a  = artefact_from_token(t1, "hiu:a", "hip:b", t1.token_id);
    auto t2 = token_from_artefact(a);
    CHECK(t2.revoked == true);
    CHECK(t2.token_id == t1.token_id);
}

TEST_CASE("artefact: end-to-end token -> artefact -> JSON -> artefact -> token") {
    ConsentToken t1;
    t1.token_id   = "ct_e2e";
    t1.patient    = PatientId::pseudonymous("p_e2e");
    t1.purposes   = {Purpose::triage,
                     Purpose::medication_review,
                     Purpose::second_opinion,
                     Purpose::billing_audit};
    t1.issued_at  = Time::now();
    t1.expires_at = t1.issued_at + std::chrono::nanoseconds{6h};
    t1.revoked    = false;

    auto a1 = artefact_from_token(t1, "hiu:e2e", "hip:e2e", "art_e2e");
    auto js = to_abdm_json(a1);
    auto r  = from_abdm_json(js);
    REQUIRE(r);
    const auto& a2 = r.value();

    CHECK(a2.artefact_id   == a1.artefact_id);
    CHECK(a2.patient.str() == a1.patient.str());
    CHECK(a2.requester_id  == a1.requester_id);
    CHECK(a2.fetcher_id    == a1.fetcher_id);
    REQUIRE(a2.purposes.size() == a1.purposes.size());
    for (std::size_t i = 0; i < a1.purposes.size(); ++i) {
        CHECK(a2.purposes[i] == a1.purposes[i]);
    }
    CHECK(a2.status == a1.status);

    auto t2 = token_from_artefact(a2);
    // token_from_artefact intentionally sets token_id = artefact_id (the
    // artefact-side id is the wire-side identifier; the original token's
    // own id is not preserved through the round-trip). All other token
    // fields are preserved.
    CHECK(t2.token_id      == a1.artefact_id);
    CHECK(t2.patient.str() == t1.patient.str());
    REQUIRE(t2.purposes.size() == t1.purposes.size());
    for (std::size_t i = 0; i < t1.purposes.size(); ++i) {
        CHECK(t2.purposes[i] == t1.purposes[i]);
    }
    CHECK(t2.revoked == t1.revoked);
}
