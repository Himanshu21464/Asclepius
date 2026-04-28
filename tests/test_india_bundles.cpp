// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors

#include <doctest/doctest.h>

#include "asclepius/evaluation.hpp"

#include <chrono>

using namespace asclepius;
using namespace std::chrono_literals;

// =============================================================================
// TeleConsultEnvelope
// =============================================================================

TEST_CASE("envelope: make_envelope populates fields") {
    auto t0 = Time::now();
    auto e  = make_envelope("c-1",
                            PatientId::pseudonymous("p1"),
                            ActorId::clinician("dr_a"),
                            "follow-up",
                            t0,
                            t0 + std::chrono::nanoseconds{20min});
    CHECK(e.consult_id      == "c-1");
    CHECK(e.patient.str()   == "pat:p1");
    CHECK(e.clinician.str() == "clinician:dr_a");
    CHECK(e.topic           == "follow-up");
    CHECK(e.started_at      == t0);
    CHECK(e.ended_at        == t0 + std::chrono::nanoseconds{20min});
}

TEST_CASE("envelope: unsigned envelope fails verification on both sides") {
    auto e = make_envelope("c", PatientId::pseudonymous("p"),
                           ActorId::clinician("d"),
                           "topic", Time::now(),
                           Time::now() + std::chrono::nanoseconds{10min});
    CHECK_FALSE(verify_clinician_signature(e));
    CHECK_FALSE(verify_patient_signature(e));
    CHECK_FALSE(is_fully_signed(e));
}

TEST_CASE("envelope: clinician-only signed verifies clinician only") {
    auto k  = KeyStore::generate();
    auto e  = make_envelope("c", PatientId::pseudonymous("p"),
                            ActorId::clinician("d"),
                            "topic", Time::now(),
                            Time::now() + std::chrono::nanoseconds{10min});
    sign_as_clinician(e, k);
    CHECK(verify_clinician_signature(e));
    CHECK_FALSE(verify_patient_signature(e));
    CHECK_FALSE(is_fully_signed(e));
}

TEST_CASE("envelope: both sides signed, is_fully_signed true") {
    auto kc = KeyStore::generate();
    auto kp = KeyStore::generate();
    auto e  = make_envelope("c-2", PatientId::pseudonymous("p2"),
                            ActorId::clinician("d2"),
                            "annual review", Time::now(),
                            Time::now() + std::chrono::nanoseconds{15min});
    sign_as_clinician(e, kc);
    sign_as_patient(e, kp);
    CHECK(is_fully_signed(e));
}

TEST_CASE("envelope: tampering invalidates BOTH signatures") {
    auto kc = KeyStore::generate();
    auto kp = KeyStore::generate();
    auto e  = make_envelope("c", PatientId::pseudonymous("p"),
                            ActorId::clinician("d"),
                            "x", Time::now(),
                            Time::now() + std::chrono::nanoseconds{1min});
    sign_as_clinician(e, kc);
    sign_as_patient(e, kp);
    REQUIRE(is_fully_signed(e));
    e.topic = "tampered";
    CHECK_FALSE(verify_clinician_signature(e));
    CHECK_FALSE(verify_patient_signature(e));
    CHECK_FALSE(is_fully_signed(e));
}

TEST_CASE("envelope: JSON round-trip preserves signatures") {
    auto kc = KeyStore::generate();
    auto kp = KeyStore::generate();
    auto e  = make_envelope("c-3", PatientId::pseudonymous("p3"),
                            ActorId::clinician("d3"),
                            "rx review", Time::now(),
                            Time::now() + std::chrono::nanoseconds{10min});
    sign_as_clinician(e, kc);
    sign_as_patient(e, kp);
    auto js = envelope_to_json(e);
    auto r  = envelope_from_json(js);
    REQUIRE(r);
    const auto& back = r.value();
    CHECK(back.consult_id        == e.consult_id);
    CHECK(back.patient.str()     == e.patient.str());
    CHECK(back.clinician.str()   == e.clinician.str());
    CHECK(back.topic             == e.topic);
    CHECK(back.patient_signature   == e.patient_signature);
    CHECK(back.clinician_signature == e.clinician_signature);
    CHECK(is_fully_signed(back));
}

TEST_CASE("envelope: JSON malformed input rejected") {
    CHECK(!envelope_from_json("not json"));
    CHECK(!envelope_from_json(""));
    CHECK(!envelope_from_json("[]"));
}

TEST_CASE("envelope: JSON missing required field rejected") {
    auto stripped = nlohmann::json{
        {"consult_id", "c"},
        // patient missing
    }.dump();
    CHECK(!envelope_from_json(stripped));
}

// =============================================================================
// BillAuditBundle
// =============================================================================

TEST_CASE("bill: classify_line — bands and unknown") {
    using S = BillLineFinding::Severity;
    CHECK(classify_line(100.0, 100.0) == S::consistent);
    CHECK(classify_line(149.0, 100.0) == S::consistent);
    CHECK(classify_line( 51.0, 100.0) == S::consistent);
    CHECK(classify_line(151.0, 100.0) == S::over_billed);
    CHECK(classify_line( 49.0, 100.0) == S::under_billed);
    CHECK(classify_line(100.0,   0.0) == S::unknown_item);
    CHECK(classify_line( 0.0,    0.0) == S::unknown_item);
}

TEST_CASE("bill: aggregate_totals sums across findings") {
    BillAuditBundle a;
    a.findings.push_back(BillLineFinding{"x", "x", 100.0,  80.0,
                                         BillLineFinding::Severity::consistent, ""});
    a.findings.push_back(BillLineFinding{"y", "y", 200.0, 150.0,
                                         BillLineFinding::Severity::consistent, ""});
    aggregate_totals(a);
    CHECK(a.total_billed    == doctest::Approx(300.0));
    CHECK(a.total_reference == doctest::Approx(230.0));
}

TEST_CASE("bill: sign and verify round-trip") {
    BillAuditBundle a;
    a.bundle_id       = "bundle-1";
    a.patient         = PatientId::pseudonymous("p");
    a.auditor         = ActorId::clinician("auditor_smith");
    a.hospital_id     = "hosp:apollo:bom";
    a.reference_table = "CGHS-2025";
    a.audited_at      = Time::now();
    a.findings.push_back({"cataract:iol", "cataract w/ IOL", 35000.0, 18000.0,
                          BillLineFinding::Severity::over_billed, ""});
    aggregate_totals(a);
    auto k = KeyStore::generate();
    sign_bill_audit(a, k);
    CHECK(verify_bill_audit(a));
}

TEST_CASE("bill: tampering with billed_amount fails verification") {
    BillAuditBundle a;
    a.bundle_id       = "bundle-2";
    a.patient         = PatientId::pseudonymous("p");
    a.auditor         = ActorId::clinician("auditor");
    a.hospital_id     = "hosp:x";
    a.reference_table = "CGHS-2025";
    a.audited_at      = Time::now();
    a.findings.push_back({"x", "x", 100.0, 80.0,
                          BillLineFinding::Severity::consistent, ""});
    aggregate_totals(a);
    auto k = KeyStore::generate();
    sign_bill_audit(a, k);
    REQUIRE(verify_bill_audit(a));
    a.findings[0].billed_amount = 200.0;  // tamper
    CHECK_FALSE(verify_bill_audit(a));
}

TEST_CASE("bill: summarise_bill_audit counts by severity") {
    BillAuditBundle a;
    a.findings.push_back({"a", "", 100.0, 100.0, BillLineFinding::Severity::consistent,    ""});
    a.findings.push_back({"b", "", 200.0, 100.0, BillLineFinding::Severity::over_billed,   ""});
    a.findings.push_back({"c", "", 100.0, 300.0, BillLineFinding::Severity::under_billed,  ""});
    a.findings.push_back({"d", "", 100.0,   0.0, BillLineFinding::Severity::unknown_item,  ""});
    auto s = summarise_bill_audit(a);
    CHECK(s.total       == 4);
    CHECK(s.consistent  == 1);
    CHECK(s.over_billed == 1);
    CHECK(s.under_billed== 1);
    CHECK(s.unknown_item== 1);
}

TEST_CASE("bill: JSON round-trip preserves all fields and signature") {
    BillAuditBundle a;
    a.bundle_id       = "rt";
    a.patient         = PatientId::pseudonymous("p_rt");
    a.auditor         = ActorId::clinician("auditor_rt");
    a.hospital_id     = "hosp:rt";
    a.reference_table = "CGHS-2025";
    a.audited_at      = Time::now();
    a.findings.push_back({"x", "ix", 100.0, 80.0,
                          BillLineFinding::Severity::consistent, "ok"});
    a.findings.push_back({"y", "iy", 200.0, 100.0,
                          BillLineFinding::Severity::over_billed, "review"});
    aggregate_totals(a);
    auto k = KeyStore::generate();
    sign_bill_audit(a, k);

    auto js = bill_audit_to_json(a);
    auto r  = bill_audit_from_json(js);
    REQUIRE(r);
    const auto& b = r.value();
    CHECK(b.bundle_id        == a.bundle_id);
    CHECK(b.patient.str()    == a.patient.str());
    CHECK(b.auditor.str()    == a.auditor.str());
    CHECK(b.hospital_id      == a.hospital_id);
    CHECK(b.reference_table  == a.reference_table);
    REQUIRE(b.findings.size() == a.findings.size());
    for (std::size_t i = 0; i < a.findings.size(); ++i) {
        CHECK(b.findings[i].item_code        == a.findings[i].item_code);
        CHECK(b.findings[i].billed_amount    == doctest::Approx(a.findings[i].billed_amount));
        CHECK(b.findings[i].reference_amount == doctest::Approx(a.findings[i].reference_amount));
        CHECK(b.findings[i].severity         == a.findings[i].severity);
        CHECK(b.findings[i].note             == a.findings[i].note);
    }
    CHECK(b.total_billed    == doctest::Approx(a.total_billed));
    CHECK(b.total_reference == doctest::Approx(a.total_reference));
    CHECK(b.signature  == a.signature);
    CHECK(b.public_key == a.public_key);
    CHECK(verify_bill_audit(b));
}

TEST_CASE("bill: JSON malformed input rejected") {
    CHECK(!bill_audit_from_json(""));
    CHECK(!bill_audit_from_json("not json"));
    CHECK(!bill_audit_from_json("[]"));
}

TEST_CASE("bill: JSON unknown severity rejected") {
    auto j = nlohmann::json{
        {"bundle_id", "x"}, {"patient", "pat:p"}, {"auditor", "clinician:a"},
        {"hospital_id", "h"}, {"reference_table", "t"},
        {"audited_at", Time::now().iso8601()},
        {"findings", nlohmann::json::array({
            {{"item_code", "i"}, {"item_description", ""},
             {"billed_amount", 0.0}, {"reference_amount", 0.0},
             {"severity", "GIBBERISH"}, {"note", ""}},
        })},
        {"total_billed", 0.0}, {"total_reference", 0.0},
        {"signature",  std::string(KeyStore::sig_bytes * 2, '0')},
        {"public_key", std::string(KeyStore::pk_bytes  * 2, '0')},
    }.dump();
    CHECK(!bill_audit_from_json(j));
}

TEST_CASE("bill: to_string covers all severity values") {
    using S = BillLineFinding::Severity;
    CHECK(std::string{to_string(S::consistent)}   == "consistent");
    CHECK(std::string{to_string(S::over_billed)}  == "over_billed");
    CHECK(std::string{to_string(S::under_billed)} == "under_billed");
    CHECK(std::string{to_string(S::unknown_item)} == "unknown_item");
}
