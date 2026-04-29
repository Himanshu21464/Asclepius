// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
//
// Round 95 — TeleConsultEnvelope and BillAuditBundle.
//
// Both are signed evidence shapes anchored in the audit chain via the
// `events::tele_consult_closed` and `events::bill_audited` event codes.
// Canonical bytes are field-concatenation (no JSON), the same convention
// HumanAttestation uses, so signing is reproducible across serialisers.

#include "asclepius/evaluation.hpp"
#include "asclepius/canonical.hpp"

#include <fmt/core.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace asclepius {

namespace {

// Round 103: hex / sign / verify hoisted to canonical:: — only the
// shape-specific canonical-bytes builders stay here.

// ---- TeleConsultEnvelope canonical bytes -----------------------------------

std::vector<std::uint8_t> envelope_canonical_bytes(const TeleConsultEnvelope& e) {
    std::string s;
    s.reserve(e.consult_id.size() + e.topic.size() + 256);
    s.append(e.consult_id);                                s.push_back('\n');
    s.append(e.patient.str());                             s.push_back('\n');
    s.append(e.clinician.str());                           s.push_back('\n');
    s.append(e.topic);                                     s.push_back('\n');
    s.append(fmt::format("{}", e.started_at.nanos_since_epoch())); s.push_back('\n');
    s.append(fmt::format("{}", e.ended_at.nanos_since_epoch()));   s.push_back('\n');
    s.append(e.video_hash.hex());                          s.push_back('\n');
    s.append(e.transcript_hash.hex());
    return std::vector<std::uint8_t>{s.begin(), s.end()};
}

// ---- BillAuditBundle canonical bytes ---------------------------------------

std::vector<std::uint8_t> bill_audit_canonical_bytes(const BillAuditBundle& a) {
    std::string s;
    s.reserve(256 + a.findings.size() * 64);
    s.append(a.bundle_id);                  s.push_back('\n');
    s.append(a.patient.str());              s.push_back('\n');
    s.append(a.auditor.str());              s.push_back('\n');
    s.append(a.hospital_id);                s.push_back('\n');
    s.append(a.reference_table);            s.push_back('\n');
    s.append(fmt::format("{}", a.audited_at.nanos_since_epoch())); s.push_back('\n');
    s.append(fmt::format("{:.6f}", a.total_billed));               s.push_back('\n');
    s.append(fmt::format("{:.6f}", a.total_reference));            s.push_back('\n');
    for (const auto& f : a.findings) {
        s.append(f.item_code);              s.push_back('|');
        s.append(f.item_description);       s.push_back('|');
        s.append(fmt::format("{:.6f}", f.billed_amount));   s.push_back('|');
        s.append(fmt::format("{:.6f}", f.reference_amount)); s.push_back('|');
        s.append(to_string(f.severity));    s.push_back('\n');
    }
    return std::vector<std::uint8_t>{s.begin(), s.end()};
}

}  // namespace

// ============================================================================
// TeleConsultEnvelope
// ============================================================================

TeleConsultEnvelope
make_envelope(std::string consult_id,
              PatientId   patient,
              ActorId     clinician,
              std::string topic,
              Time        started_at,
              Time        ended_at,
              Hash        video_hash,
              Hash        transcript_hash) {
    TeleConsultEnvelope e;
    e.consult_id      = std::move(consult_id);
    e.patient         = std::move(patient);
    e.clinician       = std::move(clinician);
    e.topic           = std::move(topic);
    e.started_at      = started_at;
    e.ended_at        = ended_at;
    e.video_hash      = video_hash;
    e.transcript_hash = transcript_hash;
    return e;
}

TeleConsultEnvelope&
sign_as_clinician(TeleConsultEnvelope& env, const KeyStore& key) {
    auto bytes = envelope_canonical_bytes(env);
    env.clinician_signature  = canonical::sign(key, bytes);
    env.clinician_public_key = key.public_key();
    return env;
}

TeleConsultEnvelope&
sign_as_patient(TeleConsultEnvelope& env, const KeyStore& key) {
    auto bytes = envelope_canonical_bytes(env);
    env.patient_signature  = canonical::sign(key, bytes);
    env.patient_public_key = key.public_key();
    return env;
}

namespace {
bool sig_is_zero(std::span<const std::uint8_t, KeyStore::sig_bytes> s) noexcept {
    for (auto b : s) if (b != 0) return false;
    return true;
}
}  // namespace

bool verify_clinician_signature(const TeleConsultEnvelope& env) noexcept {
    if (sig_is_zero(env.clinician_signature)) return false;
    try {
        auto bytes = envelope_canonical_bytes(env);
        return canonical::verify(
            bytes,
            std::span<const std::uint8_t, KeyStore::sig_bytes>{env.clinician_signature},
            std::span<const std::uint8_t, KeyStore::pk_bytes>{env.clinician_public_key});
    } catch (...) { return false; }
}

bool verify_patient_signature(const TeleConsultEnvelope& env) noexcept {
    if (sig_is_zero(env.patient_signature)) return false;
    try {
        auto bytes = envelope_canonical_bytes(env);
        return canonical::verify(
            bytes,
            std::span<const std::uint8_t, KeyStore::sig_bytes>{env.patient_signature},
            std::span<const std::uint8_t, KeyStore::pk_bytes>{env.patient_public_key});
    } catch (...) { return false; }
}

bool is_fully_signed(const TeleConsultEnvelope& env) noexcept {
    return verify_clinician_signature(env) && verify_patient_signature(env);
}

std::string envelope_to_json(const TeleConsultEnvelope& e) {
    nlohmann::json j = {
        {"consult_id",            e.consult_id},
        {"patient",               std::string{e.patient.str()}},
        {"clinician",             std::string{e.clinician.str()}},
        {"topic",                 e.topic},
        {"started_at",            e.started_at.iso8601()},
        {"ended_at",              e.ended_at.iso8601()},
        {"video_hash",            e.video_hash.hex()},
        {"transcript_hash",       e.transcript_hash.hex()},
        {"patient_signature",     canonical::to_hex(e.patient_signature)},
        {"patient_public_key",    canonical::to_hex(e.patient_public_key)},
        {"clinician_signature",   canonical::to_hex(e.clinician_signature)},
        {"clinician_public_key",  canonical::to_hex(e.clinician_public_key)},
    };
    return j.dump();
}

Result<TeleConsultEnvelope> envelope_from_json(std::string_view s) {
    nlohmann::json j;
    try { j = nlohmann::json::parse(s); }
    catch (const std::exception& e) {
        return Error::invalid(std::string{"malformed envelope json: "} + e.what());
    }
    if (!j.is_object()) return Error::invalid("envelope json must be an object");

    auto get_str = [&](const char* k) -> Result<std::string> {
        if (!j.contains(k) || !j[k].is_string()) {
            return Error::invalid(std::string{"missing or non-string field: "} + k);
        }
        return j[k].get<std::string>();
    };
    auto fill_array = [&](const char* k, std::size_t expected,
                           std::vector<std::uint8_t>& out) -> Result<void> {
        auto sv = get_str(k); if (!sv) return sv.error();
        auto raw = canonical::from_hex(sv.value()); if (!raw) return raw.error();
        if (raw.value().size() != expected) {
            return Error::invalid(std::string{"wrong-size hex field: "} + k);
        }
        out = std::move(raw.value());
        return Result<void>::ok();
    };

    TeleConsultEnvelope e;

    auto consult_id = get_str("consult_id"); if (!consult_id) return consult_id.error();
    auto patient    = get_str("patient");    if (!patient)    return patient.error();
    auto clinician  = get_str("clinician");  if (!clinician)  return clinician.error();
    auto topic      = get_str("topic");      if (!topic)      return topic.error();
    auto started    = get_str("started_at"); if (!started)    return started.error();
    auto ended      = get_str("ended_at");   if (!ended)      return ended.error();
    auto vh         = get_str("video_hash"); if (!vh)         return vh.error();
    auto th         = get_str("transcript_hash"); if (!th)    return th.error();

    e.consult_id   = std::move(consult_id.value());
    e.patient      = PatientId{patient.value()};
    e.clinician    = ActorId{clinician.value()};
    e.topic        = std::move(topic.value());
    e.started_at   = Time::from_iso8601(started.value());
    e.ended_at     = Time::from_iso8601(ended.value());

    auto vh_bytes = canonical::from_hex(vh.value()); if (!vh_bytes) return vh_bytes.error();
    auto th_bytes = canonical::from_hex(th.value()); if (!th_bytes) return th_bytes.error();
    if (vh_bytes.value().size() != Hash::size && !vh_bytes.value().empty()) {
        return Error::invalid("video_hash wrong byte length");
    }
    if (th_bytes.value().size() != Hash::size && !th_bytes.value().empty()) {
        return Error::invalid("transcript_hash wrong byte length");
    }
    if (!vh_bytes.value().empty()) {
        Hash h{};
        std::copy(vh_bytes.value().begin(), vh_bytes.value().end(), h.bytes.begin());
        e.video_hash = h;
    }
    if (!th_bytes.value().empty()) {
        Hash h{};
        std::copy(th_bytes.value().begin(), th_bytes.value().end(), h.bytes.begin());
        e.transcript_hash = h;
    }

    std::vector<std::uint8_t> tmp;
    auto r = fill_array("patient_signature", KeyStore::sig_bytes, tmp);   if (!r) return r.error();
    std::copy(tmp.begin(), tmp.end(), e.patient_signature.begin());
    r = fill_array("patient_public_key",  KeyStore::pk_bytes, tmp);       if (!r) return r.error();
    std::copy(tmp.begin(), tmp.end(), e.patient_public_key.begin());
    r = fill_array("clinician_signature", KeyStore::sig_bytes, tmp);      if (!r) return r.error();
    std::copy(tmp.begin(), tmp.end(), e.clinician_signature.begin());
    r = fill_array("clinician_public_key", KeyStore::pk_bytes, tmp);      if (!r) return r.error();
    std::copy(tmp.begin(), tmp.end(), e.clinician_public_key.begin());

    return e;
}

// ============================================================================
// BillAuditBundle
// ============================================================================

const char* to_string(BillLineFinding::Severity s) noexcept {
    switch (s) {
        case BillLineFinding::Severity::consistent:   return "consistent";
        case BillLineFinding::Severity::over_billed:  return "over_billed";
        case BillLineFinding::Severity::under_billed: return "under_billed";
        case BillLineFinding::Severity::unknown_item: return "unknown_item";
    }
    return "unknown";
}

BillLineFinding::Severity
classify_line(double billed_amount,
              double reference_amount,
              ClassifyThresholds t) noexcept {
    if (reference_amount <= 0.0) return BillLineFinding::Severity::unknown_item;
    const double ratio = billed_amount / reference_amount;
    if (ratio > t.over_factor)  return BillLineFinding::Severity::over_billed;
    if (ratio < t.under_factor) return BillLineFinding::Severity::under_billed;
    return BillLineFinding::Severity::consistent;
}

BillAuditBundle& aggregate_totals(BillAuditBundle& a) {
    a.total_billed    = 0.0;
    a.total_reference = 0.0;
    for (const auto& f : a.findings) {
        a.total_billed    += f.billed_amount;
        a.total_reference += f.reference_amount;
    }
    return a;
}

BillAuditBundle& sign_bill_audit(BillAuditBundle& a, const KeyStore& key) {
    auto bytes = bill_audit_canonical_bytes(a);
    a.signature  = canonical::sign(key, bytes);
    a.public_key = key.public_key();
    return a;
}

bool verify_bill_audit(const BillAuditBundle& a) noexcept {
    try {
        auto bytes = bill_audit_canonical_bytes(a);
        return canonical::verify(
            bytes,
            std::span<const std::uint8_t, KeyStore::sig_bytes>{a.signature},
            std::span<const std::uint8_t, KeyStore::pk_bytes>{a.public_key});
    } catch (...) { return false; }
}

BillAuditSummary summarise_bill_audit(const BillAuditBundle& a) noexcept {
    BillAuditSummary s{a.findings.size(), 0, 0, 0, 0};
    for (const auto& f : a.findings) {
        switch (f.severity) {
            case BillLineFinding::Severity::consistent:   s.consistent++;   break;
            case BillLineFinding::Severity::over_billed:  s.over_billed++;  break;
            case BillLineFinding::Severity::under_billed: s.under_billed++; break;
            case BillLineFinding::Severity::unknown_item: s.unknown_item++; break;
        }
    }
    return s;
}

std::string bill_audit_to_json(const BillAuditBundle& a) {
    nlohmann::json findings = nlohmann::json::array();
    for (const auto& f : a.findings) {
        findings.push_back({
            {"item_code",        f.item_code},
            {"item_description", f.item_description},
            {"billed_amount",    f.billed_amount},
            {"reference_amount", f.reference_amount},
            {"severity",         to_string(f.severity)},
            {"note",             f.note},
        });
    }
    nlohmann::json j = {
        {"bundle_id",       a.bundle_id},
        {"patient",         std::string{a.patient.str()}},
        {"auditor",         std::string{a.auditor.str()}},
        {"hospital_id",     a.hospital_id},
        {"reference_table", a.reference_table},
        {"findings",        findings},
        {"total_billed",    a.total_billed},
        {"total_reference", a.total_reference},
        {"audited_at",      a.audited_at.iso8601()},
        {"signature",       canonical::to_hex(a.signature)},
        {"public_key",      canonical::to_hex(a.public_key)},
    };
    return j.dump();
}

Result<BillAuditBundle> bill_audit_from_json(std::string_view s) {
    nlohmann::json j;
    try { j = nlohmann::json::parse(s); }
    catch (const std::exception& e) {
        return Error::invalid(std::string{"malformed bill_audit json: "} + e.what());
    }
    if (!j.is_object()) return Error::invalid("bill_audit json must be an object");

    BillAuditBundle a;

    auto get_str = [&](const char* k, std::string& out) -> Result<void> {
        if (!j.contains(k) || !j[k].is_string()) {
            return Error::invalid(std::string{"missing or non-string field: "} + k);
        }
        out = j[k].get<std::string>();
        return Result<void>::ok();
    };
    {
        auto r = get_str("bundle_id",       a.bundle_id);       if (!r) return r.error();
        std::string patient_s, auditor_s;
        r = get_str("patient",              patient_s);         if (!r) return r.error();
        r = get_str("auditor",              auditor_s);         if (!r) return r.error();
        a.patient = PatientId{patient_s};
        a.auditor = ActorId{auditor_s};
        r = get_str("hospital_id",          a.hospital_id);     if (!r) return r.error();
        r = get_str("reference_table",      a.reference_table); if (!r) return r.error();
        std::string at;
        r = get_str("audited_at",           at);                if (!r) return r.error();
        a.audited_at = Time::from_iso8601(at);
    }

    if (!j.contains("findings") || !j["findings"].is_array()) {
        return Error::invalid("missing or non-array field: findings");
    }
    try {
        for (const auto& f : j["findings"]) {
            if (!f.is_object()) return Error::invalid("finding entry not an object");
            BillLineFinding lf;
            lf.item_code        = f.value("item_code",        std::string{});
            lf.item_description = f.value("item_description", std::string{});
            lf.billed_amount    = f.value("billed_amount",    0.0);
            lf.reference_amount = f.value("reference_amount", 0.0);
            lf.note             = f.value("note",             std::string{});
            const auto sev_s    = f.value("severity",         std::string{"consistent"});
            if      (sev_s == "consistent")   lf.severity = BillLineFinding::Severity::consistent;
            else if (sev_s == "over_billed")  lf.severity = BillLineFinding::Severity::over_billed;
            else if (sev_s == "under_billed") lf.severity = BillLineFinding::Severity::under_billed;
            else if (sev_s == "unknown_item") lf.severity = BillLineFinding::Severity::unknown_item;
            else return Error::invalid(std::string{"unknown finding severity: "} + sev_s);
            a.findings.push_back(std::move(lf));
        }
    } catch (const nlohmann::json::exception& e) {
        return Error::invalid(std::string{"finding has wrong-type field: "} + e.what());
    }

    std::string sig_hex, pk_hex;
    try {
        a.total_billed    = j.value("total_billed",    0.0);
        a.total_reference = j.value("total_reference", 0.0);
        sig_hex = j.value("signature",  std::string{});
        pk_hex  = j.value("public_key", std::string{});
    } catch (const nlohmann::json::exception& e) {
        return Error::invalid(std::string{"bill_audit field type error: "} + e.what());
    }
    auto fields = canonical::parse_signature_fields(sig_hex, pk_hex);
    if (!fields) return fields.error();
    a.signature  = fields.value().signature;
    a.public_key = fields.value().public_key;

    return a;
}

}  // namespace asclepius
