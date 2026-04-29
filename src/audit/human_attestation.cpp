// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
//
// Round 92 — HumanAttestation: signed clinician sign-off primitive.
// Round 103 — refactored to use canonical:: primitive (signed bytes are
// computed here per-shape; hex/sign/verify are shared substrate).
//
// The kernel produces and verifies attestations; callers anchor them in
// the audit ledger via `events::human_attestation`. The signature is
// computed over a canonical concatenation of the attestation's fields
// to avoid JSON-serialiser drift surprises (different libraries can emit
// the same object with different key orders or whitespace).

#include "asclepius/audit.hpp"
#include "asclepius/canonical.hpp"

#include <fmt/core.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace asclepius {

namespace {

// Build the canonical bytes that get signed. The order is fixed:
//   actor.str() | "\n"
//   subject_kind | "\n"
//   subject_id | "\n"
//   statement | "\n"
//   attested_at.nanos_since_epoch (decimal)
// No JSON, no escaping — the inputs are caller-controlled strings and
// the kernel's job is reproducibility, not parser-roundtrip safety.
std::vector<std::uint8_t> canonical_bytes(const HumanAttestation& a) {
    std::string s;
    s.reserve(a.subject_kind.size() + a.subject_id.size() +
              a.statement.size() + a.actor.str().size() + 32);
    s.append(a.actor.str());
    s.push_back('\n');
    s.append(a.subject_kind);
    s.push_back('\n');
    s.append(a.subject_id);
    s.push_back('\n');
    s.append(a.statement);
    s.push_back('\n');
    s.append(fmt::format("{}", a.attested_at.nanos_since_epoch()));
    return std::vector<std::uint8_t>{s.begin(), s.end()};
}

}  // namespace

HumanAttestation
sign_human_attestation(const KeyStore& signer,
                       ActorId         actor,
                       std::string     subject_kind,
                       std::string     subject_id,
                       std::string     statement) {
    HumanAttestation a;
    a.actor        = std::move(actor);
    a.subject_kind = std::move(subject_kind);
    a.subject_id   = std::move(subject_id);
    a.statement    = std::move(statement);
    a.attested_at  = Time::now();

    auto bytes = canonical_bytes(a);
    a.signature  = canonical::sign(signer, bytes);
    a.public_key = signer.public_key();
    return a;
}

bool verify_human_attestation(const HumanAttestation& a) noexcept {
    try {
        auto bytes = canonical_bytes(a);
        return canonical::verify(
            bytes,
            std::span<const std::uint8_t, KeyStore::sig_bytes>{a.signature},
            std::span<const std::uint8_t, KeyStore::pk_bytes>{a.public_key});
    } catch (...) {
        return false;
    }
}

std::string attestation_to_json(const HumanAttestation& a) {
    nlohmann::json j = {
        {"actor",        std::string{a.actor.str()}},
        {"subject_kind", a.subject_kind},
        {"subject_id",   a.subject_id},
        {"statement",    a.statement},
        {"attested_at",  a.attested_at.iso8601()},
        {"signature",    canonical::to_hex(a.signature)},
        {"public_key",   canonical::to_hex(a.public_key)},
    };
    return j.dump();
}

Result<HumanAttestation> attestation_from_json(std::string_view s) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(s);
    } catch (const std::exception& e) {
        return Error::invalid(std::string{"malformed attestation json: "} + e.what());
    }
    if (!j.is_object()) return Error::invalid("attestation json must be an object");

    auto get_str = [&](const char* key) -> Result<std::string> {
        if (!j.contains(key) || !j[key].is_string()) {
            return Error::invalid(std::string{"missing or non-string field: "} + key);
        }
        return j[key].get<std::string>();
    };

    auto actor_s        = get_str("actor");        if (!actor_s)        return actor_s.error();
    auto subject_kind   = get_str("subject_kind"); if (!subject_kind)   return subject_kind.error();
    auto subject_id     = get_str("subject_id");   if (!subject_id)     return subject_id.error();
    auto statement      = get_str("statement");    if (!statement)      return statement.error();
    auto attested_at_s  = get_str("attested_at");  if (!attested_at_s)  return attested_at_s.error();
    auto sig_hex        = get_str("signature");    if (!sig_hex)        return sig_hex.error();
    auto pk_hex         = get_str("public_key");   if (!pk_hex)         return pk_hex.error();

    auto fields = canonical::parse_signature_fields(sig_hex.value(), pk_hex.value());
    if (!fields) return fields.error();

    HumanAttestation a;
    a.actor        = ActorId{actor_s.value()};
    a.subject_kind = std::move(subject_kind.value());
    a.subject_id   = std::move(subject_id.value());
    a.statement    = std::move(statement.value());
    a.attested_at  = Time::from_iso8601(attested_at_s.value());
    a.signature    = fields.value().signature;
    a.public_key   = fields.value().public_key;
    return a;
}

}  // namespace asclepius
