// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include <doctest/doctest.h>

#include "asclepius/interop/fhir.hpp"

#include <nlohmann/json.hpp>

using namespace asclepius;
using namespace asclepius::fhir;

TEST_CASE("FHIR parse_reference splits resourceType/id") {
    auto r = parse_reference("Patient/abc123");
    REQUIRE(r);
    CHECK(r.value().resource_type == "Patient");
    CHECK(r.value().id == "abc123");
}

TEST_CASE("FHIR parse_reference rejects bad input") {
    CHECK(!parse_reference("nope"));
    CHECK(!parse_reference("/abc"));
    CHECK(!parse_reference("Patient/"));
}

TEST_CASE("FHIR extract_scope finds Patient and Encounter") {
    auto bundle = nlohmann::json::parse(R"({
        "resourceType":"Bundle",
        "entry":[
          {"resource":{"resourceType":"Patient",  "id":"pat-9"}},
          {"resource":{"resourceType":"Encounter","id":"enc-1"}}
        ]
    })");
    auto s = extract_scope(bundle);
    REQUIRE(s);
    REQUIRE(s.value().patient.has_value());
    REQUIRE(s.value().encounter.has_value());
    CHECK(s.value().patient->str()   == "fhir:Patient/pat-9");
    CHECK(s.value().encounter->str() == "fhir:Encounter/enc-1");
}

TEST_CASE("FHIR purpose_from_v3_code maps known codes") {
    CHECK(purpose_from_v3_code("TREAT")   .value() == Purpose::ambient_documentation);
    CHECK(purpose_from_v3_code("DIAGNOST").value() == Purpose::diagnostic_suggestion);
    CHECK(purpose_from_v3_code("RXMG")    .value() == Purpose::medication_review);
    CHECK(!purpose_from_v3_code("UNKNOWN"));
}

// ---- Round 91 — India profile interop helpers -----------------------------

TEST_CASE("FHIR purpose_from_v3_code maps india codes") {
    CHECK(purpose_from_v3_code("RX-RESOLVE").value() == Purpose::prescription_resolution);
    CHECK(purpose_from_v3_code("2NDOPN")    .value() == Purpose::second_opinion);
    CHECK(purpose_from_v3_code("REFERRAL")  .value() == Purpose::specialist_referral);
    CHECK(purpose_from_v3_code("BILLAUDIT") .value() == Purpose::billing_audit);
    CHECK(purpose_from_v3_code("OUTCOMES")  .value() == Purpose::longitudinal_outcomes_research);
    CHECK(purpose_from_v3_code("ETREAT")    .value() == Purpose::emergency_clinical_access);
}

TEST_CASE("FHIR purpose_to_v3_code round-trip") {
    using P = Purpose;
    for (auto p : {P::ambient_documentation, P::diagnostic_suggestion, P::triage,
                   P::medication_review, P::quality_improvement, P::research,
                   P::operations, P::prescription_resolution, P::second_opinion,
                   P::specialist_referral, P::billing_audit,
                   P::longitudinal_outcomes_research, P::emergency_clinical_access}) {
        const char* code = purpose_to_v3_code(p);
        REQUIRE(std::string{code} != "unknown");
        auto back = purpose_from_v3_code(code);
        REQUIRE(back);
        CHECK(back.value() == p);
    }
}

TEST_CASE("FHIR purpose_to_v3_code returns RISKMG for risk_stratification") {
    // RISKMG is emit-only (we do not parse it back); the symmetric round-trip
    // is intentionally one-way for the risk_stratification purpose.
    CHECK(std::string{purpose_to_v3_code(Purpose::risk_stratification)} == "RISKMG");
}

TEST_CASE("FHIR family_relation_to_role_code and back") {
    using FR = FamilyRelation;
    CHECK(std::string{family_relation_to_role_code(FR::adult_child_for_elder_parent)} == "CHILD");
    CHECK(std::string{family_relation_to_role_code(FR::parent_for_minor)}             == "PRN");
    CHECK(std::string{family_relation_to_role_code(FR::legal_guardian_for_ward)}      == "GUARD");
    CHECK(std::string{family_relation_to_role_code(FR::spouse_for_spouse)}            == "SPS");

    CHECK(family_relation_from_role_code("CHILD").value() == FR::adult_child_for_elder_parent);
    CHECK(family_relation_from_role_code("PRN").value()   == FR::parent_for_minor);
    CHECK(family_relation_from_role_code("GUARD").value() == FR::legal_guardian_for_ward);
    CHECK(family_relation_from_role_code("SPS").value()   == FR::spouse_for_spouse);

    CHECK(!family_relation_from_role_code("UNKNOWN"));
}

TEST_CASE("FHIR well_formed_bundle accepts minimal good bundle") {
    auto b = nlohmann::json::parse(R"({
        "resourceType":"Bundle",
        "type":"collection",
        "entry":[
          {"resource":{"resourceType":"Patient","id":"p"}}
        ]
    })");
    auto r = well_formed_bundle(b);
    REQUIRE(r);
    CHECK(r.value() == 1);
}

TEST_CASE("FHIR well_formed_bundle rejects non-object") {
    auto arr = nlohmann::json::array();
    CHECK(!well_formed_bundle(arr));
}

TEST_CASE("FHIR well_formed_bundle rejects wrong resourceType") {
    auto b = nlohmann::json::parse(R"({"resourceType":"Patient","entry":[]})");
    CHECK(!well_formed_bundle(b));
}

TEST_CASE("FHIR well_formed_bundle rejects missing entry array") {
    auto b = nlohmann::json::parse(R"({"resourceType":"Bundle"})");
    CHECK(!well_formed_bundle(b));
}

TEST_CASE("FHIR well_formed_bundle rejects malformed entry") {
    auto b = nlohmann::json::parse(R"({
        "resourceType":"Bundle",
        "entry":[{"notResource":{}}]
    })");
    CHECK(!well_formed_bundle(b));
}

TEST_CASE("FHIR extract_hip_id and extract_hiu_id pull from meta.tag") {
    auto b = nlohmann::json::parse(R"({
      "resourceType":"Bundle",
      "meta":{"tag":[
        {"system":"https://abdm.gov.in/CodeSystem/hip","code":"hip:apollo.bom"},
        {"system":"https://abdm.gov.in/CodeSystem/hiu","code":"hiu:1mg.del"}
      ]},
      "entry":[]
    })");
    auto hip = extract_hip_id(b);
    auto hiu = extract_hiu_id(b);
    REQUIRE(hip.has_value());
    REQUIRE(hiu.has_value());
    CHECK(*hip == "hip:apollo.bom");
    CHECK(*hiu == "hiu:1mg.del");
}

TEST_CASE("FHIR extract_hip_id returns nullopt when absent") {
    auto b = nlohmann::json::parse(R"({"resourceType":"Bundle","entry":[]})");
    CHECK(!extract_hip_id(b).has_value());
    CHECK(!extract_hiu_id(b).has_value());
}

TEST_CASE("FHIR extract_abha_id pulls 14-digit identifier") {
    auto pat = nlohmann::json::parse(R"({
      "resourceType":"Patient",
      "id":"p-1",
      "identifier":[
        {"system":"https://other.id/","value":"ignore"},
        {"system":"https://healthid.ndhm.gov.in/","value":"91-1234-5678-9012"}
      ]
    })");
    auto a = extract_abha_id(pat);
    REQUIRE(a.has_value());
    CHECK(*a == "91-1234-5678-9012");
}

TEST_CASE("FHIR extract_abha_id returns nullopt when absent or malformed") {
    auto p1 = nlohmann::json::parse(R"({"resourceType":"Patient","id":"p"})");
    CHECK(!extract_abha_id(p1).has_value());
    auto p2 = nlohmann::json::parse(R"({"resourceType":"Patient","identifier":[]})");
    CHECK(!extract_abha_id(p2).has_value());
    auto arr = nlohmann::json::array();
    CHECK(!extract_abha_id(arr).has_value());
}

TEST_CASE("FHIR bundle_from_artefact emits well-formed Bundle with Consent and Patient") {
    asclepius::ConsentArtefact a;
    a.artefact_id    = "art-123";
    a.patient        = asclepius::PatientId::pseudonymous("p");
    a.requester_id   = "hiu:1mg";
    a.fetcher_id     = "hip:apollo";
    a.purposes       = {asclepius::Purpose::triage,
                        asclepius::Purpose::second_opinion};
    a.issued_at      = asclepius::Time::now();
    a.expires_at     = a.issued_at + std::chrono::nanoseconds{std::chrono::hours(24)};
    a.status         = asclepius::ConsentArtefact::Status::granted;
    a.schema_version = "1.0";

    auto js = bundle_from_artefact(a);
    auto b  = nlohmann::json::parse(js);
    auto wf = well_formed_bundle(b);
    REQUIRE(wf);
    CHECK(wf.value() == 2);  // Consent + Patient

    CHECK(extract_hip_id(b).value() == "hip:apollo");
    CHECK(extract_hiu_id(b).value() == "hiu:1mg");
}

TEST_CASE("FHIR artefact_from_bundle round-trips a granted artefact") {
    asclepius::ConsentArtefact a;
    a.artefact_id    = "art-rt";
    a.patient        = asclepius::PatientId::pseudonymous("p_rt");
    a.requester_id   = "hiu:rt";
    a.fetcher_id     = "hip:rt";
    a.purposes       = {asclepius::Purpose::specialist_referral,
                        asclepius::Purpose::billing_audit};
    a.issued_at      = asclepius::Time::from_iso8601("2026-04-28T10:00:00Z");
    a.expires_at     = asclepius::Time::from_iso8601("2026-04-29T10:00:00Z");
    a.status         = asclepius::ConsentArtefact::Status::granted;
    a.schema_version = "1.0";

    auto js  = bundle_from_artefact(a);
    auto b   = nlohmann::json::parse(js);
    auto r   = artefact_from_bundle(b);
    REQUIRE(r);
    const auto& a2 = r.value();

    CHECK(a2.artefact_id    == a.artefact_id);
    CHECK(a2.patient.str()  == a.patient.str());
    CHECK(a2.requester_id   == a.requester_id);
    CHECK(a2.fetcher_id     == a.fetcher_id);
    REQUIRE(a2.purposes.size() == a.purposes.size());
    for (std::size_t i = 0; i < a.purposes.size(); ++i) {
        CHECK(a2.purposes[i] == a.purposes[i]);
    }
    CHECK(a2.status         == asclepius::ConsentArtefact::Status::granted);
    CHECK(a2.schema_version == "1.0");
}

TEST_CASE("FHIR artefact_from_bundle rejects bundle without Consent entry") {
    auto b = nlohmann::json::parse(R"({
        "resourceType":"Bundle",
        "entry":[{"resource":{"resourceType":"Patient","id":"p"}}]
    })");
    CHECK(!artefact_from_bundle(b));
}

TEST_CASE("FHIR artefact_from_bundle rejects malformed bundle") {
    auto b = nlohmann::json::parse(R"({"not":"a bundle"})");
    CHECK(!artefact_from_bundle(b));
}

TEST_CASE("FHIR artefact_from_bundle rejects unknown purpose code") {
    asclepius::ConsentArtefact a;
    a.artefact_id  = "art-bad";
    a.patient      = asclepius::PatientId::pseudonymous("p");
    a.requester_id = "hiu";
    a.fetcher_id   = "hip";
    a.purposes     = {asclepius::Purpose::triage};
    a.issued_at    = asclepius::Time::now();
    a.expires_at   = a.issued_at + std::chrono::nanoseconds{std::chrono::hours(1)};
    a.status       = asclepius::ConsentArtefact::Status::granted;

    auto js = bundle_from_artefact(a);
    auto b  = nlohmann::json::parse(js);
    // Mutate one purpose code to something unknown.
    auto& consent = b["entry"][0]["resource"];
    consent["provision"]["purpose"][0]["code"] = "MADE_UP";
    CHECK(!artefact_from_bundle(b));
}
