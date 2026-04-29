// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
//
// Worked example #8 — end-to-end India healthtech journey.
//
// One synthetic patient (a 72-year-old, P1) flows through a realistic
// care pathway. The adult-child (A1) consents on her behalf; an issued
// consent-artefact is anchored in the audit ledger; a triage classifier's
// performance is tracked by a CalibrationMonitor; an ED arrival kicks
// off an emergency-override that backfills within the 72h window; a
// specialist tele-consult is two-party signed; an itemised bill is
// audited against the CGHS-2025 reference; a sample collected by a
// rural diagnostic van is cold-chain attested; a care-path constraint
// (female-only, on-device-only) is evaluated and attested; finally the
// Merkle chain is verified end-to-end.
//
// Every step writes one or more typed ledger entries via the round-98
// helpers (append_consent_artefact_issued, append_tele_consult, ...).
// The whole demo runs offline in well under a second.

#include <fmt/core.h>

#include "asclepius/asclepius.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

using namespace asclepius;
using namespace std::chrono_literals;

namespace {

void hr() {
    fmt::print("──────────────────────────────────────────────────────────────\n");
}

}  // namespace

int main() {
    fmt::print("\nAsclepius — India full journey (example 08)\n");
    fmt::print("Substrate primitives shipped in rounds 90–99 composed end-to-end.\n");
    hr();

    // ---- Setup ---------------------------------------------------------
    auto db_path = std::filesystem::temp_directory_path() /
                   "asclepius_india_full_journey.db";
    std::filesystem::remove(db_path);

    auto ledger_r = Ledger::open(db_path);
    if (!ledger_r) {
        fmt::print(stderr, "open: {}\n", ledger_r.error().what());
        return 2;
    }
    auto& ledger = ledger_r.value();

    ConsentRegistry  consent_registry;
    FamilyGraph      family;
    EmergencyOverride emergency;

    auto P1 = PatientId::pseudonymous("p_elder_72");
    auto A1 = PatientId::pseudonymous("p_adult_child");
    auto MD = ActorId::clinician("dr_iyer");
    auto AU = ActorId::clinician("auditor_sharma");
    auto SP = ActorId::clinician("dr_kapoor_cardio");

    auto org_key = KeyStore::generate();    // operator org signing key
    auto md_key  = KeyStore::generate();    // clinician
    auto pt_key  = KeyStore::generate();    // patient/proxy
    auto sp_key  = KeyStore::generate();    // specialist

    fmt::print(" Patient        : {}\n", P1.str());
    fmt::print(" Proxy          : {}\n", A1.str());
    fmt::print(" DB path        : {}\n", db_path.string());
    hr();

    // ---- Step 1 — FamilyGraph ----------------------------------------
    fmt::print("§1 FamilyGraph — adult-child for elder parent\n");
    if (auto r = family.record_relation(
            A1, P1, FamilyRelation::adult_child_for_elder_parent); !r) {
        fmt::print(stderr, "record_relation: {}\n", r.error().what());
        return 2;
    }
    fmt::print("    can_consent_for(A1 -> P1) = {}\n",
               family.can_consent_for(A1, P1));
    fmt::print("    summary: {}\n", family.summary_string());
    hr();

    // ---- Step 2 — ConsentArtefact (issued + ledger) ------------------
    fmt::print("§2 ConsentArtefact — ABDM wire-shape, issued and anchored\n");
    auto tok = consent_registry.grant(P1,
                                      {Purpose::triage,
                                       Purpose::specialist_referral,
                                       Purpose::billing_audit},
                                      24h);
    if (!tok) {
        fmt::print(stderr, "grant: {}\n", tok.error().what());
        return 2;
    }
    auto artefact = artefact_from_token(tok.value(),
                                        "hiu:apollo.bangalore",
                                        "hip:abha.gateway",
                                        "art-india-08-001");
    auto issued = append_consent_artefact_issued(ledger, artefact);
    if (!issued) {
        fmt::print(stderr, "append_consent_artefact_issued: {}\n",
                   issued.error().what());
        return 2;
    }
    fmt::print("    artefact_id     : {}\n", artefact.artefact_id);
    fmt::print("    purposes        : {}\n", artefact.purposes.size());
    fmt::print("    ledger entry seq: {}\n", issued.value().header.seq);
    hr();

    // ---- Step 3 — CalibrationMonitor (triage) ------------------------
    fmt::print("§3 CalibrationMonitor — triage model rolling sensitivity\n");
    CalibrationMonitor cal({/*sens=*/0.95, /*spec=*/0.85, /*tol=*/0.0});
    cal.record_n(CalibrationMonitor::Outcome::true_positive,  47);
    cal.record_n(CalibrationMonitor::Outcome::false_negative,  2);
    cal.record_n(CalibrationMonitor::Outcome::true_negative,  90);
    cal.record_n(CalibrationMonitor::Outcome::false_positive,  6);
    auto snap = cal.snapshot(/*min_samples=*/30);
    fmt::print("    sensitivity     : {:.4f}\n", snap.sensitivity);
    fmt::print("    specificity     : {:.4f}\n", snap.specificity);
    fmt::print("    below floor     : {}\n", snap.below_floor ? "yes" : "no");
    fmt::print("    summary         : {}\n", cal.summary_string());
    hr();

    // ---- Step 4 — EmergencyOverride ----------------------------------
    fmt::print("§4 EmergencyOverride — DPDP § 7 break-glass with backfill\n");
    auto eo = emergency.activate(MD, P1, "ED arrival, unconscious");
    if (!eo) {
        fmt::print(stderr, "emergency.activate: {}\n", eo.error().what());
        return 2;
    }
    if (auto r = append_emergency_override_activated(ledger, eo.value()); !r) {
        fmt::print(stderr, "append_emergency_override_activated: {}\n",
                   r.error().what());
        return 2;
    }
    if (auto r = emergency.backfill(eo.value().token_id,
                                    "note:ed-2026-04-29-iyer-mvA-3a91"); !r) {
        fmt::print(stderr, "backfill: {}\n", r.error().what());
        return 2;
    }
    auto eo2 = emergency.get(eo.value().token_id);
    if (!eo2) {
        fmt::print(stderr, "get: {}\n", eo2.error().what());
        return 2;
    }
    if (auto r = append_emergency_override_backfilled(ledger, eo2.value()); !r) {
        fmt::print(stderr, "append_emergency_override_backfilled: {}\n",
                   r.error().what());
        return 2;
    }
    fmt::print("    token_id        : {}\n", eo2.value().token_id);
    fmt::print("    backfilled      : {}\n", eo2.value().backfilled);
    fmt::print("    evidence_id     : {}\n", eo2.value().backfill_evidence_id);
    hr();

    // ---- Step 5 — TeleConsultEnvelope --------------------------------
    fmt::print("§5 TeleConsultEnvelope — two-party signed specialist consult\n");
    auto envelope = make_envelope("c-india-08-001",
                                  P1,
                                  SP,
                                  "post-ED specialist review (cardio)",
                                  Time::now(),
                                  Time::now() + std::chrono::nanoseconds{15min});
    sign_as_clinician(envelope, sp_key);
    sign_as_patient(envelope, pt_key);
    auto consult = append_tele_consult(ledger, envelope);
    if (!consult) {
        fmt::print(stderr, "append_tele_consult: {}\n",
                   consult.error().what());
        return 2;
    }
    fmt::print("    consult_id      : {}\n", envelope.consult_id);
    fmt::print("    fully_signed    : {}\n",
               is_fully_signed(envelope) ? "yes" : "no");
    fmt::print("    ledger entry seq: {}\n", consult.value().header.seq);
    hr();

    // ---- Step 6 — BillAuditBundle ------------------------------------
    fmt::print("§6 BillAuditBundle — itemised bill audit vs CGHS-2025\n");
    BillAuditBundle bill;
    bill.bundle_id       = "bill-india-08-001";
    bill.patient         = P1;
    bill.auditor         = AU;
    bill.hospital_id     = "hosp:apollo:bom";
    bill.reference_table = "CGHS-2025";
    bill.audited_at      = Time::now();
    bill.findings.push_back({"cataract:iol",
                             "Cataract w/ intra-ocular lens",
                             32500.0, 18000.0,
                             classify_line(32500.0, 18000.0),
                             "billed > 1.5x reference"});
    bill.findings.push_back({"ECG-12-lead",
                             "12-lead ECG", 800.0, 700.0,
                             classify_line(800.0, 700.0), ""});
    bill.findings.push_back({"private:room",
                             "Private-room day rate (no public reference)",
                             6500.0, 0.0,
                             classify_line(6500.0, 0.0),
                             "no public reference"});
    aggregate_totals(bill);
    sign_bill_audit(bill, org_key);
    auto bill_entry = append_bill_audit(ledger, bill);
    if (!bill_entry) {
        fmt::print(stderr, "append_bill_audit: {}\n",
                   bill_entry.error().what());
        return 2;
    }
    auto sum = summarise_bill_audit(bill);
    fmt::print("    findings        : {}\n", sum.total);
    fmt::print("    over_billed     : {}\n", sum.over_billed);
    fmt::print("    consistent      : {}\n", sum.consistent);
    fmt::print("    unknown_item    : {}\n", sum.unknown_item);
    fmt::print("    total_billed    : {:.2f}\n", bill.total_billed);
    fmt::print("    total_reference : {:.2f}\n", bill.total_reference);
    fmt::print("    signature_ok    : {}\n",
               verify_bill_audit(bill) ? "yes" : "no");
    hr();

    // ---- Step 7 — SampleIntegrityBundle ------------------------------
    fmt::print("§7 SampleIntegrityBundle — rural-van cold-chain custody\n");
    SampleIntegrityBundle sample;
    sample.sample_id    = "smpl-india-08-001";
    sample.patient      = P1;
    sample.collected_by = "van:42:phlebo:rohan";
    sample.collected_at = Time::now();
    sample.resulted_by  = "lab:thyrocare:bom";
    sample.resulted_at  = sample.collected_at + std::chrono::nanoseconds{4h};
    sample.checkpoints  = {
        {sample.collected_at,                                   "field:village_x", 4.5, true,  ""},
        {sample.collected_at + std::chrono::nanoseconds{30min}, "transit:van",     5.5, true,  ""},
        {sample.resulted_at,                                    "lab:thyrocare:bom", 6.0, true, ""},
    };
    sign_sample_integrity(sample, org_key);
    auto pair = append_sample_integrity(ledger, sample);
    if (!pair) {
        fmt::print(stderr, "append_sample_integrity: {}\n",
                   pair.error().what());
        return 2;
    }
    fmt::print("    sample_id       : {}\n", sample.sample_id);
    fmt::print("    cold_chain_ok   : {}\n",
               cold_chain_intact(sample) ? "yes" : "no");
    fmt::print("    seq collected   : {}\n", pair.value().first.header.seq);
    fmt::print("    seq resulted    : {}\n", pair.value().second.header.seq);
    hr();

    // ---- Step 8 — CarePathAttestation --------------------------------
    fmt::print("§8 CarePathAttestation — Sakhi-style female-only constraint\n");
    access::Constraint constraint;
    constraint.staff_gender    = access::Constraint::StaffGender::female;
    constraint.allowed_languages = {"hi", "en", "ta"};

    access::Context  context;
    context.staff_gender = access::Constraint::StaffGender::female;
    context.language     = "hi";

    auto care = make_care_path_attestation("att-india-08-001",
                                           P1, MD, constraint, context);
    sign_care_path(care, org_key);
    auto care_entry = append_care_path(ledger, care);
    if (!care_entry) {
        fmt::print(stderr, "append_care_path: {}\n",
                   care_entry.error().what());
        return 2;
    }
    fmt::print("    decision        : {}\n", access::to_string(care.decision));
    fmt::print("    reason          : {}\n",
               care.reason.empty() ? "(allow)" : care.reason);
    fmt::print("    signature_ok    : {}\n",
               verify_care_path(care) ? "yes" : "no");
    fmt::print("    ledger entry seq: {}\n", care_entry.value().header.seq);
    hr();

    // ---- Step 9 — Chain integrity ------------------------------------
    fmt::print("§9 Ledger::verify — end-to-end Merkle chain integrity\n");
    auto v = ledger.verify();
    if (!v) {
        fmt::print(stderr, "verify: {}\n", v.error().what());
        return 1;
    }
    fmt::print("    chain length    : {}\n", ledger.length());
    fmt::print("    head hash       : {}\n", ledger.head().hex().substr(0, 16) + "...");
    fmt::print("    verify          : OK\n");
    hr();

    fmt::print("\nok\n");
    return 0;
}
