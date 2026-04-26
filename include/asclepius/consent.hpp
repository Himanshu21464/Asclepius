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

    // HARD delete: erase the entry from the registry entirely. Distinct
    // from revoke(), which marks a token revoked but keeps the row so
    // the audit chain can still reconstruct the token's history. remove()
    // wipes the row — after this call get(), token_exists(), and the
    // various enumerators behave as if the token never existed.
    //
    // Returns Error::not_found if no token with this id exists.
    //
    // Does NOT fire the observer. The runtime mirrors consent state into
    // the audit ledger by replaying observer events; a removed token
    // would be invisible to that replay, which is intentionally
    // destructive — the opposite of the "ledger is source of truth"
    // pattern. Use revoke() for normal lifecycle, remove() only for
    // tests and explicit administrative cleanup.
    Result<void> remove(std::string_view token_id);

    Result<bool> permits(const PatientId& patient, Purpose purpose) const;

    // Cheap noexcept "is this patient consented to anything right now?"
    // True iff at least one active (non-revoked, non-expired) token
    // exists for the patient (any purpose). Useful for /healthz
    // dashboards and gate logic that just wants a yes/no on whether
    // any grant is currently in force for the patient.
    bool permits_any_purpose(const PatientId& patient) const noexcept;

    // First active (non-revoked, non-expired) token for `patient` whose
    // purposes list contains `purpose`. Returns Error::not_found if no
    // such token exists. Tiebreak among multiple matches is unspecified —
    // callers that need ordering should use active_tokens_for_patient
    // and filter themselves.
    Result<ConsentToken> find_by_purpose(const PatientId& patient,
                                         Purpose          purpose) const;

    Result<ConsentToken> get(std::string_view token_id) const;

    // Snapshot for serialization / replay.
    std::vector<ConsentToken> snapshot() const;

    // List all (non-expired and revoked alike) tokens for a given patient.
    // Useful for operational queries: "which tokens does this patient
    // currently have on file?" Order is unspecified.
    std::vector<ConsentToken> tokens_for_patient(const PatientId& patient) const;

    // Total number of tokens for a patient across all states (active +
    // revoked + expired). Equivalent to tokens_for_patient(patient).size()
    // but cheaper — no token copies are made. Useful for /healthz
    // breakdowns and "do we hold N tokens for this patient?" checks.
    std::size_t token_count_for_patient(const PatientId& patient) const;

    // Variant of tokens_for_patient that returns only non-revoked,
    // non-expired tokens. Used by operational checks that want the
    // currently-effective grants for a patient rather than the full
    // history. Order is unspecified.
    std::vector<ConsentToken> active_tokens_for_patient(const PatientId& patient) const;

    // List all tokens (active + revoked + expired) whose purposes list
    // contains `purpose`. Used for audits — e.g. "which tokens granted
    // research access?" — where revoked/expired tokens are still
    // historically interesting. Order is unspecified.
    std::vector<ConsentToken> tokens_for_purpose(Purpose purpose) const;

    // Return the active (non-revoked, non-expired) token with the
    // latest expires_at. Returns Error::not_found when no active
    // token exists. Tiebreak among equal expiries is unspecified.
    // Useful for operator probes: "when does our oldest grant lapse?"
    Result<ConsentToken> longest_active() const;

    // Per-patient mirror of longest_active(): the active (non-revoked,
    // non-expired) token for `patient` with the latest expires_at.
    // Returns Error::not_found when the patient has no active token.
    // Tiebreak among equal expiries is unspecified. Useful for "when
    // is this specific patient's longest grant going to lapse?"
    Result<ConsentToken>
    longest_lived_active_for_patient(const PatientId& patient) const;

    // Return the active (non-revoked, non-expired) token with the
    // smallest issued_at — i.e. the oldest grant still in force.
    // Returns Error::not_found when no active token exists. Tiebreak
    // among equal issued_at values is unspecified. Useful for operator
    // probes: "what's the longest-standing live consent we hold?"
    Result<ConsentToken> oldest_active() const;

    // Return the active (non-revoked, non-expired) token with the
    // smallest expires_at — i.e. the next active grant to lapse.
    // Mirror of longest_active(). Returns Error::not_found when no
    // active token exists. Tiebreak among equal expiries is
    // unspecified. Useful for "when does our next grant lapse?"
    Result<ConsentToken> soonest_to_expire() const;

    // Cheap noexcept variant of permits(): returns whether `patient`
    // has an active (non-revoked, non-expired) token granting
    // `purpose`. Any internal failure is swallowed → false. Used in
    // hot paths where the caller has already validated inputs and
    // just wants a yes/no answer without a Result wrapper.
    bool has_purpose_for_patient(const PatientId& patient,
                                 Purpose          purpose) const noexcept;

    // Drop all tokens. Does NOT fire the observer — the registry is in
    // a reset state, not a series of individual revocations. Used by
    // tests and admin-driven reset paths.
    void clear();

    // Push the expiry of an existing token forward by `additional_ttl`.
    // Rejects revoked tokens (denied) and unknown tokens (not_found). The
    // observer fires as a "granted" event — the new expiry is the
    // material change, and downstream consumers can detect the extension
    // by comparing token_id against existing state.
    Result<ConsentToken> extend(std::string_view     token_id,
                                std::chrono::seconds additional_ttl);

    // Bulk extend: for every active (non-revoked, non-expired) token
    // belonging to `patient`, push expires_at forward by
    // `additional_ttl`. Returns the number of tokens extended. Fires
    // the observer once per token extended (Event::granted), matching
    // the single extend() semantics. additional_ttl <= 0 is a no-op
    // and returns 0. Same observer-after-lock-release pattern as
    // expire_all_for_patient.
    std::size_t extend_all_for_patient(const PatientId&     patient,
                                       std::chrono::seconds additional_ttl);

    // Counts of currently-active (non-revoked, non-expired) tokens and
    // total tokens (active + revoked + expired). O(n) over the registry,
    // bounded by the number of tokens the runtime has seen.
    std::size_t active_count() const;
    std::size_t total_count() const;

    // Number of distinct PatientIds across all stored tokens (active +
    // revoked + expired). O(n) over the registry, bounded by the number
    // of tokens the runtime has seen. Used by /healthz dashboards that
    // want a "how many patients have we ever seen?" line.
    std::size_t patient_count() const;

    // Distinct PatientIds across all stored tokens (active + revoked +
    // expired), sorted by their underlying string body. Order is
    // deterministic so callers can diff snapshots over time. Used by
    // operator probes that want to enumerate the patient population the
    // runtime has on file.
    std::vector<PatientId> patients() const;

    // Aggregate snapshot for /healthz: total tokens, active (non-revoked,
    // non-expired) count, expired-but-not-revoked count, revoked count,
    // and distinct patient count. Computed in a single pass under one
    // lock for a self-consistent view — calling the individual counters
    // separately can drift if mutations interleave.
    struct Summary {
        std::size_t total;
        std::size_t active;
        std::size_t expired;
        std::size_t revoked;
        std::size_t patients;
    };
    Summary summary() const;

    // Per-patient version of summary(). Same Summary struct, but counts
    // are scoped to a single patient. The `patients` field is 1 if the
    // patient has any tokens on file (active, expired, or revoked), else
    // 0 — useful for callers that want a "do we know this patient?"
    // bit alongside the breakdown.
    Summary stats_for_patient(const PatientId& patient) const;

    // Cheap noexcept existence check at the patient level: true iff at
    // least one token (any state — active, expired, or revoked) exists
    // for this patient. Useful for /healthz dashboards and idempotent
    // ledger replay paths that want to avoid copying out tokens just to
    // ask "have we ever seen this patient?"
    bool is_patient_known(const PatientId& patient) const noexcept;

    // Tokens that are revoked AND whose issued_at falls within the past
    // `window`. Heuristic: we don't track revocation timestamps
    // explicitly, so a revoked token is "recently revoked" only if the
    // grant itself is recent — long-running grants that get revoked are
    // not surfaced. Order: by issued_at descending (newest first).
    // window <= 0 returns empty.
    std::vector<ConsentToken>
    recently_revoked(std::chrono::seconds window) const;

    // Mirror of recently_revoked, but for grants: every token (active OR
    // revoked) whose issued_at falls within the past `horizon`. Used by
    // ops dashboards that want a "recent grant velocity" view alongside
    // the revocation view. Order: by issued_at descending (newest first).
    // horizon <= 0 returns empty.
    std::vector<ConsentToken>
    tokens_granted_within(std::chrono::seconds horizon) const;

    // Token with the largest issued_at across the entire registry,
    // regardless of revoked / expired state. Returns Error::not_found
    // when the registry is empty. Tiebreak among equal issued_at values
    // is unspecified. Useful for operator probes: "what's the freshest
    // grant we've recorded?"
    Result<ConsentToken> most_recently_granted() const;

    // Active (non-revoked, non-expired) tokens whose expires_at - now()
    // is <= horizon — i.e. "what's about to lapse?" Used by ops
    // dashboards that want to nudge clinicians to re-consent before a
    // grant quietly drops. horizon <= 0 returns empty (a non-positive
    // horizon has no "soon" window). Order is unspecified.
    std::vector<ConsentToken>
    tokens_expiring_within(std::chrono::seconds horizon) const;

    // Emergency revoke all active tokens for a patient. Returns the
    // number of tokens that were revoked (already-revoked and expired
    // tokens are not counted, but unchanged). Fires the observer once
    // per token revoked. Used on patient-side withdrawal of consent
    // ("revoke everything for this patient now"); each ledger entry
    // produced is independent so the audit chain shows the cascade.
    std::size_t expire_all_for_patient(const PatientId& patient);

    // Narrower variant of expire_all_for_patient: revoke every active
    // (non-revoked, non-expired) token belonging to `patient` whose
    // purposes list contains `purpose`. Returns the number of tokens
    // revoked. Fires the observer once per token revoked
    // (Event::revoked), matching expire_all_for_patient. Same
    // observer-after-lock-release pattern. Used to walk back a single
    // purpose ("stop using this patient's data for research") without
    // touching grants for unrelated purposes.
    Result<std::size_t>
    expire_purpose_for_patient(const PatientId& patient, Purpose purpose);

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

    // Cheap noexcept liveness check on a single token id: true iff a
    // token with this id exists AND is not revoked AND has not expired.
    // Distinct from token_exists() (which is true even for revoked /
    // expired rows). Any internal failure is swallowed → false. Useful
    // for hot-path gates that already hold the token id and just want
    // a yes/no on whether it is currently in force.
    bool is_token_active(std::string_view token_id) const noexcept;

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

    // Distinct list of purposes that have at least one active (non-revoked,
    // non-expired) token for `patient`. Sorted by underlying enum value so
    // callers can diff snapshots over time. Empty if no active grants.
    // Operator probe: "what can we currently do for this patient?"
    std::vector<Purpose> active_purposes_for_patient(const PatientId& patient) const;

    // Count of distinct Purposes for which `patient` has at least one
    // active (non-revoked, non-expired) token. Equivalent to
    // active_purposes_for_patient(patient).size() but cheaper — no Purpose
    // vector is materialized. O(n) over the registry, O(1) memory beyond
    // a small fixed-capacity set sized to the Purpose enum.
    std::size_t active_purpose_count_for_patient(const PatientId& patient) const;

    // Set the expiry of an existing token to a specific absolute time.
    // Rejects revoked tokens (denied), unknown ids (not_found), and a
    // deadline strictly earlier than the existing expires_at
    // (invalid_argument — shrinking is what revoke is for). Fires the
    // observer as a granted event, mirroring extend(); the new expiry is
    // the material change.
    Result<ConsentToken> extend_to(std::string_view token_id,
                                   Time             absolute_expires_at);

    // Tokens for `patient` that are not revoked but whose expires_at has
    // passed. Distinct from active_tokens_for_patient (which excludes
    // expired) and from tokens_for_patient (which includes everything).
    // Useful for ops dashboards that want to surface "what just lapsed?"
    // before cleanup_expired() sweeps it. Order is unspecified.
    std::vector<ConsentToken> expired_for_patient(const PatientId& patient) const;

    // Full JSON dump of every token for ops debugging and diagnostic
    // export. Stable schema: {"tokens": [{"token_id", "patient",
    // "purposes", "issued_at", "expires_at", "revoked"} ...]}. Times are
    // ISO-8601 strings, purposes are stringified via to_string(Purpose).
    // Order is unspecified.
    std::string dump_state_json() const;

    // Map of {Purpose: count of active (non-revoked, non-expired) tokens
    // granting that Purpose}. A token granting multiple purposes counts
    // once per purpose it includes. Purposes with zero active tokens do
    // not appear in the map. Useful for /healthz dashboards that want a
    // per-purpose breakdown of currently-effective grants.
    std::unordered_map<Purpose, std::size_t> tokens_count_by_purpose() const;

    // PatientId with the largest token count (active + revoked + expired)
    // across the entire registry. Tiebreak among equal counts is
    // unspecified. Returns Error::not_found when the registry is empty.
    // Useful for operator probes: "which patient is the heaviest user
    // of this registry?"
    Result<PatientId> patient_with_most_tokens() const;

    // Cold-backup serialization of the entire registry: an array of
    // token objects, each {"token_id", "patient", "purposes",
    // "issued_at", "expires_at", "revoked"}. Times are ISO-8601 strings,
    // purposes are stringified via to_string(Purpose). Distinct from
    // dump_state_json (which wraps the array in a {"tokens": ...}
    // object) — this form is the bare array, suitable for
    // round-tripping through deserialize_from_json.
    std::string serialize_to_json() const;

    // Restore tokens from a JSON string produced by serialize_to_json
    // (or compatible shape — also accepts the {"tokens": [...]} envelope
    // emitted by dump_state_json). Returns the count of tokens ingested.
    // Does NOT clear existing state — appends. Skips tokens already
    // present (token_id collision) without erroring. Does NOT fire the
    // observer (matches ingest()). Returns Error::invalid on malformed
    // JSON or missing required fields.
    Result<std::size_t> deserialize_from_json(std::string_view s);

private:
    mutable std::mutex                                mu_;
    std::unordered_map<std::string, ConsentToken>    by_id_;
    Observer                                         observer_;
};

}  // namespace asclepius

#endif  // ASCLEPIUS_CONSENT_HPP
