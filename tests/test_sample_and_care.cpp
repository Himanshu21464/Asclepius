// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors

#include <doctest/doctest.h>

#include "asclepius/evaluation.hpp"

#include <chrono>

using namespace asclepius;
using namespace std::chrono_literals;

// =============================================================================
// SampleIntegrityBundle
// =============================================================================

namespace {

SampleIntegrityBundle make_bundle(bool all_in_spec = true) {
    SampleIntegrityBundle s;
    s.sample_id    = "smpl-1";
    s.patient      = PatientId::pseudonymous("p");
    s.collected_by = "van:42:phlebo:rohan";
    s.collected_at = Time::now();
    s.resulted_by  = "lab:thyrocare:bom";
    s.resulted_at  = s.collected_at + std::chrono::nanoseconds{4h};
    SampleIntegrityCheckpoint c1{s.collected_at, "field:village_x",
                                 4.5, true, ""};
    SampleIntegrityCheckpoint c2{s.collected_at + std::chrono::nanoseconds{30min},
                                 "transit:van", all_in_spec ? 5.5 : 12.0,
                                 all_in_spec, all_in_spec ? "" : "warm-up"};
    SampleIntegrityCheckpoint c3{s.resulted_at, "lab:thyrocare:bom",
                                 6.0, true, ""};
    s.checkpoints = {c1, c2, c3};
    return s;
}

}  // namespace

TEST_CASE("sample: cold_chain_intact true when all checkpoints in spec") {
    auto s = make_bundle(/*all_in_spec=*/true);
    CHECK(cold_chain_intact(s));
}

TEST_CASE("sample: cold_chain_intact false when any checkpoint out of spec") {
    auto s = make_bundle(/*all_in_spec=*/false);
    CHECK_FALSE(cold_chain_intact(s));
}

TEST_CASE("sample: sign + verify round-trip") {
    auto s = make_bundle();
    auto k = KeyStore::generate();
    sign_sample_integrity(s, k);
    CHECK(verify_sample_integrity(s));
}

TEST_CASE("sample: tampering with sample_id invalidates signature") {
    auto s = make_bundle();
    auto k = KeyStore::generate();
    sign_sample_integrity(s, k);
    REQUIRE(verify_sample_integrity(s));
    s.sample_id = "tampered";
    CHECK_FALSE(verify_sample_integrity(s));
}

TEST_CASE("sample: tampering with checkpoint temperature invalidates signature") {
    auto s = make_bundle();
    auto k = KeyStore::generate();
    sign_sample_integrity(s, k);
    REQUIRE(verify_sample_integrity(s));
    s.checkpoints[1].temperature_c = 50.0;
    CHECK_FALSE(verify_sample_integrity(s));
}

TEST_CASE("sample: JSON round-trip preserves all fields and signature") {
    auto s = make_bundle();
    auto k = KeyStore::generate();
    sign_sample_integrity(s, k);

    auto js = sample_integrity_to_json(s);
    auto r  = sample_integrity_from_json(js);
    REQUIRE(r);
    const auto& b = r.value();
    CHECK(b.sample_id      == s.sample_id);
    CHECK(b.patient.str()  == s.patient.str());
    CHECK(b.collected_by   == s.collected_by);
    CHECK(b.resulted_by    == s.resulted_by);
    REQUIRE(b.checkpoints.size() == s.checkpoints.size());
    for (std::size_t i = 0; i < s.checkpoints.size(); ++i) {
        CHECK(b.checkpoints[i].location      == s.checkpoints[i].location);
        CHECK(b.checkpoints[i].temperature_c == doctest::Approx(s.checkpoints[i].temperature_c));
        CHECK(b.checkpoints[i].within_spec   == s.checkpoints[i].within_spec);
    }
    CHECK(b.signature  == s.signature);
    CHECK(b.public_key == s.public_key);
    CHECK(verify_sample_integrity(b));
}

TEST_CASE("sample: malformed JSON rejected") {
    CHECK(!sample_integrity_from_json(""));
    CHECK(!sample_integrity_from_json("not json"));
    CHECK(!sample_integrity_from_json("[]"));
}

// =============================================================================
// CarePathAttestation
// =============================================================================

TEST_CASE("care_path: make_care_path_attestation captures evaluation") {
    access::Constraint c;
    c.staff_gender = access::Constraint::StaffGender::female;
    access::Context ctx;
    ctx.staff_gender = access::Constraint::StaffGender::female;

    auto a = make_care_path_attestation("att-1",
                                        PatientId::pseudonymous("p"),
                                        ActorId::clinician("d"),
                                        c, ctx);
    CHECK(a.attestation_id == "att-1");
    CHECK(a.decision       == access::Decision::allow);
    CHECK(a.reason.empty());
}

TEST_CASE("care_path: deny path captures reason") {
    access::Constraint c;
    c.staff_gender = access::Constraint::StaffGender::female;
    access::Context ctx;
    ctx.staff_gender = access::Constraint::StaffGender::male;

    auto a = make_care_path_attestation("att-2",
                                        PatientId::pseudonymous("p"),
                                        ActorId::clinician("d"),
                                        c, ctx);
    CHECK(a.decision == access::Decision::deny);
    CHECK(a.reason.find("staff_gender") != std::string::npos);
}

TEST_CASE("care_path: sign + verify round-trip") {
    access::Constraint c;
    access::Context    ctx;
    auto a = make_care_path_attestation("att", PatientId::pseudonymous("p"),
                                        ActorId::clinician("d"), c, ctx);
    auto k = KeyStore::generate();
    sign_care_path(a, k);
    CHECK(verify_care_path(a));
}

TEST_CASE("care_path: tampering invalidates signature") {
    access::Constraint c;
    access::Context    ctx;
    auto a = make_care_path_attestation("att", PatientId::pseudonymous("p"),
                                        ActorId::clinician("d"), c, ctx);
    auto k = KeyStore::generate();
    sign_care_path(a, k);
    REQUIRE(verify_care_path(a));
    a.reason = "tampered";
    CHECK_FALSE(verify_care_path(a));
}

TEST_CASE("care_path: JSON round-trip preserves constraint, context, decision") {
    access::Constraint c;
    c.staff_gender      = access::Constraint::StaffGender::female;
    c.device_mode       = access::Constraint::DeviceMode::on_device_only;
    c.allowed_languages = {"hi", "ta"};
    c.required_role_code = "nurse";

    access::Context ctx;
    ctx.staff_gender = access::Constraint::StaffGender::female;
    ctx.device_mode  = access::Constraint::DeviceMode::on_device_only;
    ctx.language     = "ta";
    ctx.role_code    = "nurse";

    auto a = make_care_path_attestation("att-rt",
                                        PatientId::pseudonymous("p_rt"),
                                        ActorId::clinician("d_rt"),
                                        c, ctx);
    auto k = KeyStore::generate();
    sign_care_path(a, k);

    auto js = care_path_to_json(a);
    auto r  = care_path_from_json(js);
    REQUIRE(r);
    const auto& b = r.value();
    CHECK(b.attestation_id == a.attestation_id);
    CHECK(b.patient.str()  == a.patient.str());
    CHECK(b.attester.str() == a.attester.str());
    CHECK(b.constraint.staff_gender == c.staff_gender);
    CHECK(b.constraint.device_mode  == c.device_mode);
    REQUIRE(b.constraint.allowed_languages.size() == 2);
    CHECK(b.constraint.allowed_languages[0] == "hi");
    CHECK(b.constraint.allowed_languages[1] == "ta");
    REQUIRE(b.constraint.required_role_code.has_value());
    CHECK(*b.constraint.required_role_code == "nurse");
    REQUIRE(b.context.staff_gender.has_value());
    CHECK(*b.context.staff_gender == access::Constraint::StaffGender::female);
    REQUIRE(b.context.language.has_value());
    CHECK(*b.context.language == "ta");
    CHECK(b.decision == a.decision);
    CHECK(verify_care_path(b));
}

TEST_CASE("care_path: malformed JSON rejected") {
    CHECK(!care_path_from_json(""));
    CHECK(!care_path_from_json("not json"));
    CHECK(!care_path_from_json("[]"));
}

TEST_CASE("care_path: unknown decision string rejected") {
    auto j = nlohmann::json{
        {"attestation_id", "x"}, {"patient", "pat:p"},
        {"attester", "clinician:d"},
        {"constraint", {{"staff_gender","any"}, {"device_mode","any"},
                        {"allowed_languages", nlohmann::json::array()}}},
        {"context", nlohmann::json::object()},
        {"decision", "MAYBE"}, {"reason", ""},
        {"attested_at", Time::now().iso8601()},
        {"signature",  std::string(KeyStore::sig_bytes * 2, '0')},
        {"public_key", std::string(KeyStore::pk_bytes  * 2, '0')},
    }.dump();
    CHECK(!care_path_from_json(j));
}
