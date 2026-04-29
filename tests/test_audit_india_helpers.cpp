// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors

#include <doctest/doctest.h>

#include "asclepius/audit.hpp"
#include "asclepius/consent.hpp"
#include "asclepius/evaluation.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>

using namespace asclepius;
using namespace std::chrono_literals;

namespace {

std::filesystem::path tmp_db(const char* suffix) {
    auto p = std::filesystem::temp_directory_path() /
             (std::string{"asc_round98_"} + suffix + ".db");
    std::filesystem::remove(p);
    return p;
}

}  // namespace

TEST_CASE("audit india: append_human_attestation writes one entry") {
    auto p = tmp_db("attestation");
    auto l = Ledger::open(p); REQUIRE(l);

    auto k = KeyStore::generate();
    auto a = sign_human_attestation(k, ActorId::clinician("dr_a"),
                                    "second_opinion", "case-1",
                                    "Concur with diagnosis A.");
    auto r = append_human_attestation(l.value(), a);
    REQUIRE(r);
    CHECK(r.value().header.event_type == "human.attestation");
    CHECK(r.value().header.actor      == "clinician:dr_a");
    auto body = nlohmann::json::parse(r.value().body_json);
    CHECK(body["subject_kind"] == "second_opinion");
    CHECK(body["subject_id"]   == "case-1");
}

TEST_CASE("audit india: append_consent_artefact_issued and revoked") {
    auto p = tmp_db("artefact");
    auto l = Ledger::open(p); REQUIRE(l);

    ConsentArtefact a;
    a.artefact_id    = "art-1";
    a.patient        = PatientId::pseudonymous("p1");
    a.requester_id   = "hiu:apollo";
    a.fetcher_id     = "hip:lab";
    a.purposes       = {Purpose::triage};
    a.issued_at      = Time::now();
    a.expires_at     = a.issued_at + std::chrono::nanoseconds{1h};
    a.status         = ConsentArtefact::Status::granted;
    a.schema_version = "1.0";

    auto issued = append_consent_artefact_issued(l.value(), a);
    REQUIRE(issued);
    CHECK(issued.value().header.event_type == "consent.artefact.issued");
    CHECK(issued.value().header.actor      == "hiu:apollo");

    a.status = ConsentArtefact::Status::revoked;
    auto rev = append_consent_artefact_revoked(l.value(), a);
    REQUIRE(rev);
    CHECK(rev.value().header.event_type == "consent.artefact.revoked");
    auto body = nlohmann::json::parse(rev.value().body_json);
    CHECK(body["artefact_id"] == "art-1");
    CHECK(body["status"]      == "revoked");
}

TEST_CASE("audit india: append_tele_consult uses clinician as actor") {
    auto p = tmp_db("teleconsult");
    auto l = Ledger::open(p); REQUIRE(l);

    auto e = make_envelope("c-1",
                           PatientId::pseudonymous("p"),
                           ActorId::clinician("dr_b"),
                           "follow-up", Time::now(),
                           Time::now() + std::chrono::nanoseconds{20min});
    auto r = append_tele_consult(l.value(), e);
    REQUIRE(r);
    CHECK(r.value().header.event_type == "consult.tele.closed");
    CHECK(r.value().header.actor      == "clinician:dr_b");
}

TEST_CASE("audit india: append_bill_audit") {
    auto p = tmp_db("billaudit");
    auto l = Ledger::open(p); REQUIRE(l);

    BillAuditBundle b;
    b.bundle_id       = "bundle-1";
    b.patient         = PatientId::pseudonymous("p");
    b.auditor         = ActorId::clinician("auditor");
    b.hospital_id     = "hosp:apollo";
    b.reference_table = "CGHS-2025";
    b.audited_at      = Time::now();
    b.findings.push_back({"x", "x", 100.0, 80.0,
                          BillLineFinding::Severity::consistent, ""});

    auto r = append_bill_audit(l.value(), b);
    REQUIRE(r);
    CHECK(r.value().header.event_type == "bill.audited");
    CHECK(r.value().header.actor      == "clinician:auditor");
}

TEST_CASE("audit india: append_sample_integrity writes two entries") {
    auto p = tmp_db("sample");
    auto l = Ledger::open(p); REQUIRE(l);

    SampleIntegrityBundle s;
    s.sample_id    = "smpl-1";
    s.patient      = PatientId::pseudonymous("p");
    s.collected_by = "van:42";
    s.collected_at = Time::now();
    s.resulted_by  = "lab:thyrocare";
    s.resulted_at  = s.collected_at + std::chrono::nanoseconds{4h};

    auto before = l.value().length();
    auto r = append_sample_integrity(l.value(), s);
    REQUIRE(r);
    CHECK(l.value().length() == before + 2);
    CHECK(r.value().first.header.event_type  == "sample.collected");
    CHECK(r.value().second.header.event_type == "sample.resulted");
    CHECK(r.value().first.header.actor       == "van:42");
}

TEST_CASE("audit india: append_care_path uses decision-specific event_type") {
    auto p = tmp_db("carepath");
    auto l = Ledger::open(p); REQUIRE(l);

    access::Constraint c;
    access::Context ctx;
    auto allow = make_care_path_attestation("att-allow",
                                            PatientId::pseudonymous("p"),
                                            ActorId::clinician("d"),
                                            c, ctx);
    auto rA = append_care_path(l.value(), allow);
    REQUIRE(rA);
    CHECK(rA.value().header.event_type == "care.path.allow");

    access::Constraint c2;
    c2.staff_gender = access::Constraint::StaffGender::female;
    access::Context ctx2;
    ctx2.staff_gender = access::Constraint::StaffGender::male;
    auto deny = make_care_path_attestation("att-deny",
                                           PatientId::pseudonymous("p"),
                                           ActorId::clinician("d"),
                                           c2, ctx2);
    auto rD = append_care_path(l.value(), deny);
    REQUIRE(rD);
    CHECK(rD.value().header.event_type == "care.path.deny");
}

TEST_CASE("audit india: emergency-override activate + backfill helpers") {
    auto p = tmp_db("emergency");
    auto l = Ledger::open(p); REQUIRE(l);

    EmergencyOverride eo;
    auto activated = eo.activate(ActorId::clinician("dr_x"),
                                 PatientId::pseudonymous("ed_pt"),
                                 "ED arrival, unconscious");
    REQUIRE(activated);

    auto rA = append_emergency_override_activated(l.value(), activated.value());
    REQUIRE(rA);
    CHECK(rA.value().header.event_type == "override.emergency.activated");
    auto bodyA = nlohmann::json::parse(rA.value().body_json);
    CHECK(bodyA["reason"] == "ED arrival, unconscious");
    CHECK(bodyA.contains("backfill_deadline"));

    REQUIRE(eo.backfill(activated.value().token_id, "note:ed-2026-04-28-x"));
    auto t = eo.get(activated.value().token_id); REQUIRE(t);

    auto rB = append_emergency_override_backfilled(l.value(), t.value());
    REQUIRE(rB);
    CHECK(rB.value().header.event_type == "override.emergency.backfilled");
    auto bodyB = nlohmann::json::parse(rB.value().body_json);
    CHECK(bodyB["backfill_evidence_id"] == "note:ed-2026-04-28-x");
}

TEST_CASE("audit india: helpers respect tenant scope") {
    auto p = tmp_db("tenant");
    auto l = Ledger::open(p); REQUIRE(l);

    auto k = KeyStore::generate();
    auto a = sign_human_attestation(k, ActorId::clinician("dr_t"),
                                    "x", "y", "z");
    auto r = append_human_attestation(l.value(), a, "tenant:central");
    REQUIRE(r);
    CHECK(r.value().header.tenant == "tenant:central");
}

TEST_CASE("audit india: ledger entries verify (chain integrity)") {
    auto p = tmp_db("chain");
    auto l = Ledger::open(p); REQUIRE(l);

    auto k = KeyStore::generate();
    for (int i = 0; i < 5; ++i) {
        auto a = sign_human_attestation(k, ActorId::clinician("dr"),
                                        "k", std::string{"id-"} + std::to_string(i),
                                        "stmt");
        REQUIRE(append_human_attestation(l.value(), a));
    }
    CHECK(l.value().length() == 5);
    auto v = l.value().verify();
    REQUIRE(v);
}
