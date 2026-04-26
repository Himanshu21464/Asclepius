// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#ifndef ASCLEPIUS_CONSENT_HPP
#define ASCLEPIUS_CONSENT_HPP

#include <chrono>
#include <cstdint>
#include <functional>
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
    // Observer fires after a successful grant() or revoke(). Used by the
    // Runtime to mirror consent state into the audit ledger so it can be
    // replayed on restart. The observer is called while no lock is held,
    // so it may safely call back into the registry (idempotent).
    enum class Event : std::uint8_t { granted, revoked };
    using Observer = std::function<void(Event, const ConsentToken&)>;

    ConsentRegistry() = default;

    Result<ConsentToken> grant(PatientId            patient,
                               std::vector<Purpose> purposes,
                               std::chrono::seconds ttl);

    Result<void> revoke(std::string_view token_id);

    Result<bool> permits(const PatientId& patient, Purpose purpose) const;

    Result<ConsentToken> get(std::string_view token_id) const;

    // Snapshot for serialization / replay.
    std::vector<ConsentToken> snapshot() const;

    // List all (non-expired and revoked alike) tokens for a given patient.
    // Useful for operational queries: "which tokens does this patient
    // currently have on file?" Order is unspecified.
    std::vector<ConsentToken> tokens_for_patient(const PatientId& patient) const;

    // Push the expiry of an existing token forward by `additional_ttl`.
    // Rejects revoked tokens (denied) and unknown tokens (not_found). The
    // observer fires as a "granted" event — the new expiry is the
    // material change, and downstream consumers can detect the extension
    // by comparing token_id against existing state.
    Result<ConsentToken> extend(std::string_view     token_id,
                                std::chrono::seconds additional_ttl);

    // Counts of currently-active (non-revoked, non-expired) tokens and
    // total tokens (active + revoked + expired). O(n) over the registry,
    // bounded by the number of tokens the runtime has seen.
    std::size_t active_count() const;
    std::size_t total_count() const;

    // Emergency revoke all active tokens for a patient. Returns the
    // number of tokens that were revoked (already-revoked and expired
    // tokens are not counted, but unchanged). Fires the observer once
    // per token revoked. Used on patient-side withdrawal of consent
    // ("revoke everything for this patient now"); each ledger entry
    // produced is independent so the audit chain shows the cascade.
    std::size_t expire_all_for_patient(const PatientId& patient);

    // Install (or clear, by passing {}) the observer fired on grant/revoke.
    // Idempotent: repeat calls replace the previous observer.
    void set_observer(Observer obs);

    // Restore a token verbatim — preserves token_id, issued_at, expires_at,
    // revoked. Used by Runtime restart to replay consent events from the
    // ledger. Does NOT fire the observer (the source of truth is already
    // the ledger). Returns conflict if a token with the same id exists.
    Result<void> ingest(ConsentToken token);

    // Cheap existence check: does a token with this id exist (regardless
    // of revoked / expired state)? Useful for idempotent ledger replay
    // and for callers that want to avoid a copy-out via get(). noexcept.
    bool token_exists(std::string_view token_id) const noexcept;

    // Count of tokens that are not revoked but whose expires_at has passed.
    // O(n) over the registry. Distinct from active_count() which excludes
    // both revoked and expired; this counts the "expired but not yet
    // swept" pool that cleanup_expired() would drain.
    std::size_t expired_count() const;

    // Sweep: for each token that is not revoked but whose expires_at has
    // passed, mark it revoked. Returns the number swept. Fires the
    // observer once per token swept (Event::revoked) so the runtime's
    // ledger mirror records each cleanup. Same observer-after-lock-release
    // pattern as expire_all_for_patient.
    std::size_t cleanup_expired();

private:
    mutable std::mutex                                mu_;
    std::unordered_map<std::string, ConsentToken>    by_id_;
    Observer                                         observer_;
};

}  // namespace asclepius

#endif  // ASCLEPIUS_CONSENT_HPP
