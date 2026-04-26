// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#ifndef ASCLEPIUS_CONSENT_HPP
#define ASCLEPIUS_CONSENT_HPP

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "asclepius/core.hpp"
#include "asclepius/identity.hpp"

namespace asclepius {

// Allowed purposes for AI use on a patient's data. Codified deliberately:
// adding a new purpose is a governance event, not a string change.
enum class Purpose : std::uint8_t {
    ambient_documentation = 1,
    diagnostic_suggestion = 2,
    triage                = 3,
    medication_review     = 4,
    risk_stratification   = 5,
    quality_improvement   = 6,
    research              = 7,
    operations            = 8,
};

const char* to_string(Purpose) noexcept;
Result<Purpose> purpose_from_string(std::string_view) noexcept;

// A consent token grants a set of purposes for a patient until expiry.
struct ConsentToken {
    std::string          token_id;
    PatientId            patient;
    std::vector<Purpose> purposes;
    Time                 issued_at;
    Time                 expires_at;
    bool                 revoked = false;
};

// ---- Registry ------------------------------------------------------------
//
// Tracks which patient has consented to which purposes. The registry is
// authoritative for runtime checks but does not persist by itself — the
// Runtime mirrors grants and revocations to the audit ledger so consent
// history is reconstructable from the chain alone.

class ConsentRegistry {
public:
    ConsentRegistry() = default;

    Result<ConsentToken> grant(PatientId            patient,
                               std::vector<Purpose> purposes,
                               std::chrono::seconds ttl);

    Result<void> revoke(std::string_view token_id);

    Result<bool> permits(const PatientId& patient, Purpose purpose) const;

    Result<ConsentToken> get(std::string_view token_id) const;

    // Snapshot for serialization / replay.
    std::vector<ConsentToken> snapshot() const;

private:
    mutable std::mutex                                mu_;
    std::unordered_map<std::string, ConsentToken>    by_id_;
};

}  // namespace asclepius

#endif  // ASCLEPIUS_CONSENT_HPP
