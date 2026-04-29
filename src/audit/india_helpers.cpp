// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
//
// Round 98 — Typed-append helpers for round 90-96 evidence shapes.
//
// Each helper picks the canonical event_type from `events::`, serialises
// the bundle's payload (using the existing _to_json functions on each
// type), and calls Ledger::append.

#include "asclepius/audit.hpp"
#include "asclepius/consent.hpp"
#include "asclepius/evaluation.hpp"

#include <nlohmann/json.hpp>

namespace asclepius {

namespace {

// Helper: parse a JSON string into a nlohmann::json value, or return an
// empty object on parse failure (the audit chain still gets the entry
// even if the embedded JSON is malformed, with a hint payload — but we
// never throw out of these helpers).
nlohmann::json safe_parse(std::string_view s) {
    try {
        return nlohmann::json::parse(s);
    } catch (...) {
        return nlohmann::json{{"_parse_error", true}, {"_raw", std::string{s}}};
    }
}

}  // namespace

Result<LedgerEntry>
append_human_attestation(Ledger&                 ledger,
                         const HumanAttestation& a,
                         std::string             tenant) {
    return ledger.append(std::string{events::human_attestation},
                         std::string{a.actor.str()},
                         safe_parse(attestation_to_json(a)),
                         std::move(tenant));
}

Result<LedgerEntry>
append_consent_artefact_issued(Ledger&                ledger,
                               const ConsentArtefact& a,
                               std::string            tenant) {
    return ledger.append(std::string{events::consent_artefact_issued},
                         a.requester_id,
                         safe_parse(to_abdm_json(a)),
                         std::move(tenant));
}

Result<LedgerEntry>
append_consent_artefact_revoked(Ledger&                ledger,
                                const ConsentArtefact& a,
                                std::string            tenant) {
    nlohmann::json body = {
        {"artefact_id",     a.artefact_id},
        {"patient",         std::string{a.patient.str()}},
        {"status",          to_string(a.status)},
        {"schema_version",  a.schema_version.empty() ? "1.0" : a.schema_version},
    };
    return ledger.append(std::string{events::consent_artefact_revoked},
                         a.requester_id,
                         std::move(body),
                         std::move(tenant));
}

Result<LedgerEntry>
append_tele_consult(Ledger&                    ledger,
                    const TeleConsultEnvelope& e,
                    std::string                tenant) {
    return ledger.append(std::string{events::tele_consult_closed},
                         std::string{e.clinician.str()},
                         safe_parse(envelope_to_json(e)),
                         std::move(tenant));
}

Result<LedgerEntry>
append_bill_audit(Ledger&                ledger,
                  const BillAuditBundle& b,
                  std::string            tenant) {
    return ledger.append(std::string{events::bill_audited},
                         std::string{b.auditor.str()},
                         safe_parse(bill_audit_to_json(b)),
                         std::move(tenant));
}

Result<std::pair<LedgerEntry, LedgerEntry>>
append_sample_integrity(Ledger&                      ledger,
                        const SampleIntegrityBundle& s,
                        std::string                  tenant) {
    auto body = safe_parse(sample_integrity_to_json(s));
    auto col = ledger.append(std::string{events::sample_collected},
                             s.collected_by,
                             body,
                             tenant);
    if (!col) return col.error();
    auto res = ledger.append(std::string{events::sample_resulted},
                             s.collected_by,
                             std::move(body),
                             std::move(tenant));
    if (!res) return res.error();
    return std::make_pair(col.value(), res.value());
}

Result<LedgerEntry>
append_care_path(Ledger&                    ledger,
                 const CarePathAttestation& a,
                 std::string                tenant) {
    const std::string event = (a.decision == access::Decision::allow)
        ? "care.path.allow" : "care.path.deny";
    return ledger.append(event,
                         std::string{a.attester.str()},
                         safe_parse(care_path_to_json(a)),
                         std::move(tenant));
}

Result<LedgerEntry>
append_emergency_override_activated(Ledger&                       ledger,
                                    const EmergencyOverrideToken& t,
                                    std::string                   tenant) {
    nlohmann::json body = {
        {"token_id",          t.token_id},
        {"actor",             std::string{t.actor.str()}},
        {"patient",           std::string{t.patient.str()}},
        {"reason",            t.reason},
        {"activated_at",      t.activated_at.iso8601()},
        {"backfill_deadline", t.backfill_deadline.iso8601()},
    };
    return ledger.append(std::string{events::emergency_override_activated},
                         std::string{t.actor.str()},
                         std::move(body),
                         std::move(tenant));
}

Result<LedgerEntry>
append_emergency_override_backfilled(Ledger&                       ledger,
                                     const EmergencyOverrideToken& t,
                                     std::string                   tenant) {
    nlohmann::json body = {
        {"token_id",             t.token_id},
        {"actor",                std::string{t.actor.str()}},
        {"patient",              std::string{t.patient.str()}},
        {"backfill_evidence_id", t.backfill_evidence_id},
        {"activated_at",         t.activated_at.iso8601()},
    };
    return ledger.append(std::string{events::emergency_override_backfilled},
                         std::string{t.actor.str()},
                         std::move(body),
                         std::move(tenant));
}

}  // namespace asclepius
