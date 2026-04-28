// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors

#include <doctest/doctest.h>

#include "asclepius/audit.hpp"

#include <nlohmann/json.hpp>

using namespace asclepius;

TEST_CASE("attestation: well_known_events lists all canonical codes") {
    auto v = well_known_events();
    CHECK(v.size() == 12);
    // Sorted by string ordering.
    for (std::size_t i = 1; i < v.size(); ++i) {
        CHECK(v[i - 1] < v[i]);
    }
}

TEST_CASE("attestation: is_well_known_event recognises canonical codes") {
    CHECK(is_well_known_event(events::prescription_parsed));
    CHECK(is_well_known_event(events::triage_decision));
    CHECK(is_well_known_event(events::tele_consult_closed));
    CHECK(is_well_known_event(events::bill_audited));
    CHECK(is_well_known_event(events::substitution_event));
    CHECK(is_well_known_event(events::consent_artefact_issued));
    CHECK(is_well_known_event(events::consent_artefact_revoked));
    CHECK(is_well_known_event(events::sample_collected));
    CHECK(is_well_known_event(events::sample_resulted));
    CHECK(is_well_known_event(events::human_attestation));
    CHECK(is_well_known_event(events::emergency_override_activated));
    CHECK(is_well_known_event(events::emergency_override_backfilled));
}

TEST_CASE("attestation: is_well_known_event rejects custom codes") {
    CHECK_FALSE(is_well_known_event("custom.event"));
    CHECK_FALSE(is_well_known_event(""));
    CHECK_FALSE(is_well_known_event("rx.parsed.ext"));  // no prefix-match
}

TEST_CASE("attestation: sign_human_attestation populates all fields") {
    auto k = KeyStore::generate();
    auto a = sign_human_attestation(k,
                                    ActorId::clinician("dr_iyer"),
                                    "second_opinion",
                                    "case-42",
                                    "Concur with diagnosis A; specialist review complete.");
    CHECK(a.actor.str()        == "clinician:dr_iyer");
    CHECK(a.subject_kind       == "second_opinion");
    CHECK(a.subject_id         == "case-42");
    CHECK(a.statement          == "Concur with diagnosis A; specialist review complete.");
    CHECK(a.attested_at.nanos_since_epoch() > 0);
    // public_key embedded matches signer's public_key.
    CHECK(a.public_key == k.public_key());
}

TEST_CASE("attestation: verify_human_attestation succeeds for fresh signature") {
    auto k = KeyStore::generate();
    auto a = sign_human_attestation(k,
                                    ActorId::clinician("dr_a"),
                                    "triage_override",
                                    "ed-2026-04-28-001",
                                    "Override: patient complained of chest pain at triage.");
    CHECK(verify_human_attestation(a));
}

TEST_CASE("attestation: verify rejects tampered statement") {
    auto k = KeyStore::generate();
    auto a = sign_human_attestation(k,
                                    ActorId::clinician("dr_b"),
                                    "second_opinion",
                                    "case-1",
                                    "Original statement.");
    a.statement = "Tampered statement.";
    CHECK_FALSE(verify_human_attestation(a));
}

TEST_CASE("attestation: verify rejects tampered subject_id") {
    auto k = KeyStore::generate();
    auto a = sign_human_attestation(k,
                                    ActorId::clinician("dr_b"),
                                    "second_opinion",
                                    "case-1",
                                    "stmt");
    a.subject_id = "case-2";
    CHECK_FALSE(verify_human_attestation(a));
}

TEST_CASE("attestation: verify rejects swapped public_key") {
    auto k1 = KeyStore::generate();
    auto k2 = KeyStore::generate();
    auto a  = sign_human_attestation(k1, ActorId::clinician("dr_b"),
                                     "k", "id", "stmt");
    a.public_key = k2.public_key();
    CHECK_FALSE(verify_human_attestation(a));
}

TEST_CASE("attestation: to_json contains all fields") {
    auto k = KeyStore::generate();
    auto a = sign_human_attestation(k, ActorId::clinician("dr_c"),
                                    "billing_audit", "bill-9", "ok");
    auto js = attestation_to_json(a);
    auto j  = nlohmann::json::parse(js);
    CHECK(j["actor"]        == "clinician:dr_c");
    CHECK(j["subject_kind"] == "billing_audit");
    CHECK(j["subject_id"]   == "bill-9");
    CHECK(j["statement"]    == "ok");
    CHECK(j.contains("attested_at"));
    CHECK(j.contains("signature"));
    CHECK(j.contains("public_key"));
    CHECK(j["signature"].get<std::string>().size()  == KeyStore::sig_bytes * 2);
    CHECK(j["public_key"].get<std::string>().size() == KeyStore::pk_bytes  * 2);
}

TEST_CASE("attestation: from_json round-trips a signed attestation") {
    auto k = KeyStore::generate();
    auto a = sign_human_attestation(k, ActorId::clinician("dr_d"),
                                    "triage_decision", "case-x",
                                    "Acuity = 2 (urgent).");
    auto js = attestation_to_json(a);
    auto r  = attestation_from_json(js);
    REQUIRE(r);
    const auto& a2 = r.value();
    CHECK(a2.actor.str()    == a.actor.str());
    CHECK(a2.subject_kind   == a.subject_kind);
    CHECK(a2.subject_id     == a.subject_id);
    CHECK(a2.statement      == a.statement);
    CHECK(a2.signature      == a.signature);
    CHECK(a2.public_key     == a.public_key);
    CHECK(verify_human_attestation(a2));
}

TEST_CASE("attestation: from_json rejects malformed input") {
    CHECK(!attestation_from_json("not json"));
    CHECK(!attestation_from_json(""));
    CHECK(!attestation_from_json("[]"));  // not an object
}

TEST_CASE("attestation: from_json rejects missing required fields") {
    auto missing_actor = nlohmann::json{
        {"subject_kind","k"},{"subject_id","s"},{"statement","x"},
        {"attested_at","2026-04-28T10:00:00Z"},
        {"signature",   std::string(KeyStore::sig_bytes * 2, '0')},
        {"public_key",  std::string(KeyStore::pk_bytes  * 2, '0')},
    }.dump();
    CHECK(!attestation_from_json(missing_actor));
}

TEST_CASE("attestation: from_json rejects wrong-size hex blobs") {
    auto bad_sig = nlohmann::json{
        {"actor",       "clinician:x"},
        {"subject_kind","k"},
        {"subject_id",  "s"},
        {"statement",   "x"},
        {"attested_at", "2026-04-28T10:00:00Z"},
        {"signature",   "deadbeef"},   // way too short
        {"public_key",  std::string(KeyStore::pk_bytes * 2, '0')},
    }.dump();
    CHECK(!attestation_from_json(bad_sig));
}

TEST_CASE("attestation: from_json rejects non-hex bytes") {
    auto bad = nlohmann::json{
        {"actor",       "clinician:x"},
        {"subject_kind","k"},
        {"subject_id",  "s"},
        {"statement",   "x"},
        {"attested_at", "2026-04-28T10:00:00Z"},
        {"signature",   std::string(KeyStore::sig_bytes * 2 - 2, '0') + "ZZ"},
        {"public_key",  std::string(KeyStore::pk_bytes  * 2, '0')},
    }.dump();
    CHECK(!attestation_from_json(bad));
}
