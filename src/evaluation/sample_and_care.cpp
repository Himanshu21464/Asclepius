// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
//
// Round 96 — SampleIntegrityBundle and CarePathAttestation.
//
// Both are single-signer evidence shapes anchored in the audit chain.
// Canonical bytes are field-concatenation, matching round 92/95 idiom.

#include "asclepius/evaluation.hpp"

#include <fmt/core.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace asclepius {

namespace {

std::string to_hex(std::span<const std::uint8_t> bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (auto b : bytes) {
        out.push_back(kHex[(b >> 4) & 0xF]);
        out.push_back(kHex[b & 0xF]);
    }
    return out;
}

Result<std::vector<std::uint8_t>> from_hex(std::string_view s) {
    if (s.size() % 2 != 0) return Error::invalid("hex must have even length");
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<std::uint8_t> out;
    out.reserve(s.size() / 2);
    for (std::size_t i = 0; i < s.size(); i += 2) {
        int hi = val(s[i]);
        int lo = val(s[i + 1]);
        if (hi < 0 || lo < 0) return Error::invalid("hex non-digit");
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return out;
}

// Canonical bytes for a SampleIntegrityBundle.
std::vector<std::uint8_t> sample_canonical_bytes(const SampleIntegrityBundle& s) {
    std::string out;
    out.reserve(256 + s.checkpoints.size() * 64);
    out.append(s.sample_id);                                    out.push_back('\n');
    out.append(s.patient.str());                                out.push_back('\n');
    out.append(s.collected_by);                                 out.push_back('\n');
    out.append(fmt::format("{}", s.collected_at.nanos_since_epoch())); out.push_back('\n');
    out.append(s.result_hash.hex());                            out.push_back('\n');
    out.append(s.resulted_by);                                  out.push_back('\n');
    out.append(fmt::format("{}", s.resulted_at.nanos_since_epoch())); out.push_back('\n');
    for (const auto& c : s.checkpoints) {
        out.append(fmt::format("{}", c.at.nanos_since_epoch())); out.push_back('|');
        out.append(c.location);                                 out.push_back('|');
        out.append(fmt::format("{:.4f}", c.temperature_c));     out.push_back('|');
        out.append(c.within_spec ? "1" : "0");                  out.push_back('|');
        out.append(c.note);                                     out.push_back('\n');
    }
    return std::vector<std::uint8_t>{out.begin(), out.end()};
}

// Canonical bytes for a CarePathAttestation.
std::vector<std::uint8_t> care_path_canonical_bytes(const CarePathAttestation& a) {
    std::string out;
    out.reserve(256);
    out.append(a.attestation_id);                                      out.push_back('\n');
    out.append(a.patient.str());                                       out.push_back('\n');
    out.append(a.attester.str());                                      out.push_back('\n');
    // Constraint
    out.append(asclepius::access::to_string(a.constraint.staff_gender)); out.push_back('|');
    out.append(asclepius::access::to_string(a.constraint.device_mode));  out.push_back('|');
    for (const auto& l : a.constraint.allowed_languages) {
        out.append(l); out.push_back(',');
    }
    out.push_back('|');
    if (a.constraint.required_role_code) out.append(*a.constraint.required_role_code);
    out.push_back('\n');
    // Context
    if (a.context.staff_gender) out.append(asclepius::access::to_string(*a.context.staff_gender));
    out.push_back('|');
    if (a.context.device_mode)  out.append(asclepius::access::to_string(*a.context.device_mode));
    out.push_back('|');
    if (a.context.language)     out.append(*a.context.language);
    out.push_back('|');
    if (a.context.role_code)    out.append(*a.context.role_code);
    out.push_back('\n');
    // Decision
    out.append(asclepius::access::to_string(a.decision));
    out.push_back('\n');
    out.append(a.reason);
    out.push_back('\n');
    out.append(fmt::format("{}", a.attested_at.nanos_since_epoch()));
    return std::vector<std::uint8_t>{out.begin(), out.end()};
}

}  // namespace

// ============================================================================
// SampleIntegrityBundle
// ============================================================================

SampleIntegrityBundle&
sign_sample_integrity(SampleIntegrityBundle& s, const KeyStore& signer) {
    auto bytes = sample_canonical_bytes(s);
    s.signature  = signer.sign(Bytes{bytes.data(), bytes.size()});
    s.public_key = signer.public_key();
    return s;
}

bool verify_sample_integrity(const SampleIntegrityBundle& s) noexcept {
    try {
        auto bytes = sample_canonical_bytes(s);
        return KeyStore::verify(
            Bytes{bytes.data(), bytes.size()},
            std::span<const std::uint8_t, KeyStore::sig_bytes>{s.signature},
            std::span<const std::uint8_t, KeyStore::pk_bytes>{s.public_key});
    } catch (...) { return false; }
}

bool cold_chain_intact(const SampleIntegrityBundle& s) noexcept {
    for (const auto& c : s.checkpoints) {
        if (!c.within_spec) return false;
    }
    return true;
}

std::string sample_integrity_to_json(const SampleIntegrityBundle& s) {
    nlohmann::json checkpoints = nlohmann::json::array();
    for (const auto& c : s.checkpoints) {
        checkpoints.push_back({
            {"at",            c.at.iso8601()},
            {"location",      c.location},
            {"temperature_c", c.temperature_c},
            {"within_spec",   c.within_spec},
            {"note",          c.note},
        });
    }
    nlohmann::json j = {
        {"sample_id",    s.sample_id},
        {"patient",      std::string{s.patient.str()}},
        {"collected_by", s.collected_by},
        {"collected_at", s.collected_at.iso8601()},
        {"checkpoints",  checkpoints},
        {"result_hash",  s.result_hash.hex()},
        {"resulted_by",  s.resulted_by},
        {"resulted_at",  s.resulted_at.iso8601()},
        {"signature",    to_hex(s.signature)},
        {"public_key",   to_hex(s.public_key)},
    };
    return j.dump();
}

Result<SampleIntegrityBundle>
sample_integrity_from_json(std::string_view sv) {
    nlohmann::json j;
    try { j = nlohmann::json::parse(sv); }
    catch (const std::exception& e) {
        return Error::invalid(std::string{"malformed sample json: "} + e.what());
    }
    if (!j.is_object()) return Error::invalid("sample json must be object");

    SampleIntegrityBundle s;
    auto get_str = [&](const char* k, std::string& out) -> Result<void> {
        if (!j.contains(k) || !j[k].is_string()) {
            return Error::invalid(std::string{"missing or non-string field: "} + k);
        }
        out = j[k].get<std::string>();
        return Result<void>::ok();
    };

    auto r = get_str("sample_id",    s.sample_id);    if (!r) return r.error();
    std::string patient_s;
    r = get_str("patient",           patient_s);      if (!r) return r.error();
    s.patient = PatientId{patient_s};
    r = get_str("collected_by",      s.collected_by); if (!r) return r.error();

    std::string at_s;
    r = get_str("collected_at",      at_s);           if (!r) return r.error();
    s.collected_at = Time::from_iso8601(at_s);

    r = get_str("resulted_by",       s.resulted_by);  if (!r) return r.error();
    r = get_str("resulted_at",       at_s);           if (!r) return r.error();
    s.resulted_at = Time::from_iso8601(at_s);

    std::string rh_s;
    r = get_str("result_hash",       rh_s);           if (!r) return r.error();
    if (!rh_s.empty()) {
        auto rb = from_hex(rh_s);
        if (!rb) return rb.error();
        if (rb.value().size() != Hash::size) {
            return Error::invalid("result_hash wrong byte length");
        }
        Hash h{};
        std::copy(rb.value().begin(), rb.value().end(), h.bytes.begin());
        s.result_hash = h;
    }

    if (!j.contains("checkpoints") || !j["checkpoints"].is_array()) {
        return Error::invalid("missing or non-array field: checkpoints");
    }
    for (const auto& c : j["checkpoints"]) {
        if (!c.is_object()) return Error::invalid("checkpoint not an object");
        SampleIntegrityCheckpoint cp;
        cp.at            = Time::from_iso8601(c.value("at",       std::string{}));
        cp.location      = c.value("location",                    std::string{});
        cp.temperature_c = c.value("temperature_c",               0.0);
        cp.within_spec   = c.value("within_spec",                 true);
        cp.note          = c.value("note",                        std::string{});
        s.checkpoints.push_back(std::move(cp));
    }

    auto sig_hex = j.value("signature",  std::string{});
    auto pk_hex  = j.value("public_key", std::string{});
    auto sig = from_hex(sig_hex); if (!sig) return sig.error();
    auto pk  = from_hex(pk_hex);  if (!pk)  return pk.error();
    if (sig.value().size() != KeyStore::sig_bytes) return Error::invalid("bad signature size");
    if (pk.value().size()  != KeyStore::pk_bytes)  return Error::invalid("bad public_key size");
    std::copy(sig.value().begin(), sig.value().end(), s.signature.begin());
    std::copy(pk.value().begin(),  pk.value().end(),  s.public_key.begin());

    return s;
}

// ============================================================================
// CarePathAttestation
// ============================================================================

CarePathAttestation
make_care_path_attestation(std::string                attestation_id,
                           PatientId                  patient,
                           ActorId                    attester,
                           const access::Constraint&  constraint,
                           const access::Context&     context) {
    CarePathAttestation a;
    a.attestation_id = std::move(attestation_id);
    a.patient        = std::move(patient);
    a.attester       = std::move(attester);
    a.constraint     = constraint;
    a.context        = context;
    auto eval        = access::evaluate(constraint, context);
    a.decision       = eval.decision;
    a.reason         = eval.reason;
    a.attested_at    = Time::now();
    return a;
}

CarePathAttestation&
sign_care_path(CarePathAttestation& a, const KeyStore& signer) {
    auto bytes = care_path_canonical_bytes(a);
    a.signature  = signer.sign(Bytes{bytes.data(), bytes.size()});
    a.public_key = signer.public_key();
    return a;
}

bool verify_care_path(const CarePathAttestation& a) noexcept {
    try {
        auto bytes = care_path_canonical_bytes(a);
        return KeyStore::verify(
            Bytes{bytes.data(), bytes.size()},
            std::span<const std::uint8_t, KeyStore::sig_bytes>{a.signature},
            std::span<const std::uint8_t, KeyStore::pk_bytes>{a.public_key});
    } catch (...) { return false; }
}

namespace {

std::string staff_gender_str(asclepius::access::Constraint::StaffGender g) {
    return std::string{asclepius::access::to_string(g)};
}

std::string device_mode_str(asclepius::access::Constraint::DeviceMode d) {
    return std::string{asclepius::access::to_string(d)};
}

Result<asclepius::access::Constraint::StaffGender>
parse_staff_gender(std::string_view s) {
    if (s == "any")    return asclepius::access::Constraint::StaffGender::any;
    if (s == "female") return asclepius::access::Constraint::StaffGender::female;
    if (s == "male")   return asclepius::access::Constraint::StaffGender::male;
    return Error::invalid(std::string{"unknown staff_gender: "} + std::string{s});
}

Result<asclepius::access::Constraint::DeviceMode>
parse_device_mode(std::string_view s) {
    if (s == "any")                return asclepius::access::Constraint::DeviceMode::any;
    if (s == "on_device_only")     return asclepius::access::Constraint::DeviceMode::on_device_only;
    if (s == "off_device_allowed") return asclepius::access::Constraint::DeviceMode::off_device_allowed;
    return Error::invalid(std::string{"unknown device_mode: "} + std::string{s});
}

}  // namespace

std::string care_path_to_json(const CarePathAttestation& a) {
    nlohmann::json constraint = {
        {"staff_gender",      staff_gender_str(a.constraint.staff_gender)},
        {"device_mode",       device_mode_str(a.constraint.device_mode)},
        {"allowed_languages", a.constraint.allowed_languages},
    };
    if (a.constraint.required_role_code) {
        constraint["required_role_code"] = *a.constraint.required_role_code;
    }

    nlohmann::json context = nlohmann::json::object();
    if (a.context.staff_gender) {
        context["staff_gender"] = staff_gender_str(*a.context.staff_gender);
    }
    if (a.context.device_mode) {
        context["device_mode"]  = device_mode_str(*a.context.device_mode);
    }
    if (a.context.language) context["language"] = *a.context.language;
    if (a.context.role_code) context["role_code"] = *a.context.role_code;

    nlohmann::json j = {
        {"attestation_id", a.attestation_id},
        {"patient",        std::string{a.patient.str()}},
        {"attester",       std::string{a.attester.str()}},
        {"constraint",     constraint},
        {"context",        context},
        {"decision",       std::string{asclepius::access::to_string(a.decision)}},
        {"reason",         a.reason},
        {"attested_at",    a.attested_at.iso8601()},
        {"signature",      to_hex(a.signature)},
        {"public_key",     to_hex(a.public_key)},
    };
    return j.dump();
}

Result<CarePathAttestation>
care_path_from_json(std::string_view sv) {
    nlohmann::json j;
    try { j = nlohmann::json::parse(sv); }
    catch (const std::exception& e) {
        return Error::invalid(std::string{"malformed care_path json: "} + e.what());
    }
    if (!j.is_object()) return Error::invalid("care_path json must be object");

    CarePathAttestation a;
    auto get_str = [&](const char* k, std::string& out) -> Result<void> {
        if (!j.contains(k) || !j[k].is_string()) {
            return Error::invalid(std::string{"missing or non-string field: "} + k);
        }
        out = j[k].get<std::string>();
        return Result<void>::ok();
    };

    auto r = get_str("attestation_id", a.attestation_id); if (!r) return r.error();
    std::string p_s, at_s;
    r = get_str("patient",  p_s);   if (!r) return r.error();  a.patient  = PatientId{p_s};
    r = get_str("attester", at_s);  if (!r) return r.error();  a.attester = ActorId{at_s};

    if (!j.contains("constraint") || !j["constraint"].is_object()) {
        return Error::invalid("missing or non-object: constraint");
    }
    {
        const auto& c = j["constraint"];
        auto sg = parse_staff_gender(c.value("staff_gender", std::string{"any"}));
        if (!sg) return sg.error();
        a.constraint.staff_gender = sg.value();

        auto dm = parse_device_mode(c.value("device_mode", std::string{"any"}));
        if (!dm) return dm.error();
        a.constraint.device_mode = dm.value();

        if (c.contains("allowed_languages") && c["allowed_languages"].is_array()) {
            for (const auto& l : c["allowed_languages"]) {
                if (l.is_string()) a.constraint.allowed_languages.push_back(l.get<std::string>());
            }
        }
        if (c.contains("required_role_code") && c["required_role_code"].is_string()) {
            a.constraint.required_role_code = c["required_role_code"].get<std::string>();
        }
    }

    if (j.contains("context") && j["context"].is_object()) {
        const auto& ctx = j["context"];
        if (ctx.contains("staff_gender") && ctx["staff_gender"].is_string()) {
            auto sg = parse_staff_gender(ctx["staff_gender"].get<std::string>());
            if (!sg) return sg.error();
            a.context.staff_gender = sg.value();
        }
        if (ctx.contains("device_mode") && ctx["device_mode"].is_string()) {
            auto dm = parse_device_mode(ctx["device_mode"].get<std::string>());
            if (!dm) return dm.error();
            a.context.device_mode = dm.value();
        }
        if (ctx.contains("language")  && ctx["language"].is_string())  a.context.language  = ctx["language"].get<std::string>();
        if (ctx.contains("role_code") && ctx["role_code"].is_string()) a.context.role_code = ctx["role_code"].get<std::string>();
    }

    std::string dec_s;
    r = get_str("decision", dec_s);  if (!r) return r.error();
    if      (dec_s == "allow") a.decision = access::Decision::allow;
    else if (dec_s == "deny")  a.decision = access::Decision::deny;
    else return Error::invalid(std::string{"unknown decision: "} + dec_s);

    a.reason = j.value("reason", std::string{});

    std::string at_str;
    r = get_str("attested_at", at_str);  if (!r) return r.error();
    a.attested_at = Time::from_iso8601(at_str);

    auto sig_hex = j.value("signature",  std::string{});
    auto pk_hex  = j.value("public_key", std::string{});
    auto sig = from_hex(sig_hex); if (!sig) return sig.error();
    auto pk  = from_hex(pk_hex);  if (!pk)  return pk.error();
    if (sig.value().size() != KeyStore::sig_bytes) return Error::invalid("bad signature size");
    if (pk.value().size()  != KeyStore::pk_bytes)  return Error::invalid("bad public_key size");
    std::copy(sig.value().begin(), sig.value().end(), a.signature.begin());
    std::copy(pk.value().begin(),  pk.value().end(),  a.public_key.begin());

    return a;
}

}  // namespace asclepius
