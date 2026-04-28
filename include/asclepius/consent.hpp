// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#ifndef ASCLEPIUS_CONSENT_HPP
#define ASCLEPIUS_CONSENT_HPP

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
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

    // Idempotent sibling of revoke(): mark the token revoked if it is not
    // already, otherwise return ok without firing the observer. Returns
    // Error::not_found if no token with this id exists. Useful for
    // shutdown sweeps and reconciliation paths that walk a list of
    // tokens to drain — callers want a "make this revoked, don't care if
    // it already was" primitive without papering over not_found.
    //
    // Distinct from revoke() in two ways: revoke() always re-flips the
    // bit (a no-op on the value but it always fires the observer);
    // force_revoke() short-circuits when already revoked and stays
    // silent on the observer side, so replay logs do not double-record
    // the cleanup. The not_found contract matches revoke().
    Result<void> force_revoke(std::string_view token_id);

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

    // True iff at least one active (non-revoked, non-expired) token
    // exists for this patient. Equivalent in observable behaviour to
    // permits_any_purpose() — kept as a separate name for callers that
    // express the question as "do we have an active token?" rather
    // than "does the patient permit anything?". noexcept; any internal
    // failure is swallowed → false.
    bool has_active_token(const PatientId& patient) const noexcept;

    // Registry-wide liveness probe: true iff at least one active
    // (non-revoked, non-expired) token exists anywhere in the registry,
    // regardless of patient. noexcept; any internal failure is
    // swallowed → false. Distinct from has_active_token (which is
    // per-patient) and has_revoked_tokens (which asks the inverse
    // question about the revoked subset). Useful for /healthz
    // dashboards that want a single yes/no on whether the registry
    // currently authorizes anything at all.
    bool has_any_active() const noexcept;

    // Bulk eligibility check: count of distinct PatientIds in `patients`
    // who have at least one active (non-revoked, non-expired) token in
    // the registry. Empty span returns 0. Duplicate patient ids in the
    // input are counted once each — the result is bounded above by the
    // number of distinct ids in the span. Used by routing logic that
    // wants to filter a candidate cohort to "those who currently
    // authorize anything at all" without N round-trips through
    // has_active_token.
    std::size_t
    count_active_for_patients(std::span<const PatientId> patients) const;

    // First active (non-revoked, non-expired) token for `patient` whose
    // purposes list contains `purpose`. Returns Error::not_found if no
    // such token exists. Tiebreak among multiple matches is unspecified —
    // callers that need ordering should use active_tokens_for_patient
    // and filter themselves.
    Result<ConsentToken> find_by_purpose(const PatientId& patient,
                                         Purpose          purpose) const;

    // Find an active (non-revoked, non-expired) token for `patient` whose
    // purposes-list contains EVERY purpose in `purposes` (a single token
    // covering the union). Used when a workflow needs one consent grant
    // to authorize multiple purposes simultaneously — e.g. "show me a
    // token good for both triage AND medication_review at once."
    // Returns Error::not_found if no single token covers them all.
    // Returns Error::invalid_argument if `purposes` is empty. Tiebreak
    // among multiple matches is unspecified.
    Result<ConsentToken>
    find_token_granting_all(const PatientId&         patient,
                            std::span<const Purpose> purposes) const;

    // Disjunctive sibling of find_token_granting_all: the first active
    // (non-revoked, non-expired) token for `patient` whose purposes list
    // contains AT LEAST ONE of the listed `purposes`. Used by routing
    // logic that wants any matching grant ("do we have consent for
    // triage OR diagnostic_suggestion?") without forcing a single
    // token to cover the full set. Returns Error::not_found if no such
    // token exists. Returns Error::invalid_argument if `purposes` is
    // empty. Tiebreak among multiple matches is unspecified.
    Result<ConsentToken>
    find_token_for_any_purpose(const PatientId&         patient,
                               std::span<const Purpose> purposes) const;

    Result<ConsentToken> get(std::string_view token_id) const;

    // Duration since the token was issued, computed as
    // Time::now() - token.issued_at. Returns Error::not_found if no
    // token with this id exists. Useful for ops dashboards and audit
    // surfaces that want to show "this consent grant is N minutes
    // old" without round-tripping a full token through get().
    Result<std::chrono::nanoseconds>
    token_age(std::string_view token_id) const;

    // Lifecycle snapshot of a single token: identity + issued/expires
    // timestamps + revoked flag + a computed State (revoked > expired
    // > active priority). Useful for "explain this token to me" UI
    // surfaces that want a single struct rather than the full
    // ConsentToken with its purposes vector.
    struct TokenLifecycle {
        std::string token_id;
        Time        issued_at;
        Time        expires_at;
        bool        revoked = false;
        enum class State : std::uint8_t { active, revoked, expired };
        State       state;  // computed: revoked > expired > active
    };

    // Returns the lifecycle snapshot for `token_id`, or Error::not_found
    // if no such token exists. State is computed at call time:
    // revoked tokens are reported as State::revoked even if they have
    // also passed their expires_at.
    Result<TokenLifecycle>
    token_lifecycle(std::string_view token_id) const;

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

    // Active-only mirror of tokens_for_purpose: list every non-revoked,
    // non-expired token whose purposes list contains `p`. Distinct from
    // tokens_for_purpose (which returns ALL states); this variant is the
    // one operational gate logic typically wants — "which currently-
    // effective grants cover this purpose?" Order is unspecified.
    std::vector<ConsentToken> tokens_with_purpose(Purpose p) const;

    // Distinct PatientIds that have at least one active (non-revoked,
    // non-expired) token granting `p`. Sorted by patient.str() for
    // deterministic output so callers can diff snapshots over time.
    // Empty registry returns empty. Distinct from tokens_with_purpose
    // (which returns full token rows): this is the "who has consented
    // to this purpose right now?" projection — useful for cohort
    // assembly that wants the patient list without N tokens of payload.
    std::vector<PatientId> patients_with_purpose(Purpose p) const;

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

    // Per-patient lifespan probe, unconstrained by state: the token for
    // `patient` (active OR revoked OR expired) with the largest
    // (expires_at - issued_at) lifespan — we are measuring the configured
    // duration of the grant, not its current liveness. Returns
    // Error::not_found if the patient has no tokens on file. Tiebreak
    // among equal lifespans is unspecified. Useful for audits that ask
    // "what is the longest grant we have ever held for this patient?"
    Result<ConsentToken>
    find_longest_lived_for_patient(const PatientId& patient) const;

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

    // Synonym of patient_count(): number of distinct patient ids across
    // all tokens (active + revoked + expired). Spelled with the
    // "distinct_patients" prefix to read naturally in callers asking
    // "how many patients do we hold consent records for?" — kept
    // separately from patient_count() for readability at call sites.
    std::size_t distinct_patients_count() const;

    // Distinct count of patients who have at least one active
    // (non-revoked, non-expired) token. Distinct from
    // distinct_patients_count (which counts patients with any token in
    // any state) and from active_count (which counts active tokens —
    // a patient with two active tokens contributes 2 to active_count
    // but 1 here). Useful for /healthz dashboards that want a
    // "currently authorized patient population" gauge.
    std::size_t patients_with_active_count() const;

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

    // Single-line ASCII summary for ops dashboards. Format:
    //   "total=<n> active=<n> revoked=<n> expired=<n> patients=<n>"
    // No trailing newline. Computed via summary() so the same
    // single-pass, self-consistent counts are reflected in the
    // string. Useful for plumbing a registry status pill into log
    // lines and /healthz endpoints without callers needing to format
    // the Summary struct themselves.
    std::string summary_string() const;

    // Per-token state breakdown for ops dashboards: a sorted vector of
    // {token_id, state_string} pairs covering every token in the
    // registry. The state_string is one of "active", "revoked", or
    // "expired", computed using TokenLifecycle's State priority
    // (revoked > expired > active) so a revoked token whose expiry has
    // also passed reports as "revoked". Sorted by token_id for
    // deterministic output suitable for diffing two snapshots. Useful
    // for tabular dashboards that want a flat row-per-token breakdown
    // rather than an aggregate Summary.
    std::vector<std::pair<std::string, std::string>>
    list_states_summary() const;

    // Aggregate count-by-state map: {"active": N, "revoked": N,
    // "expired": N}. Always contains all three keys — a state with
    // zero tokens still appears with count 0 so dashboards can render
    // a stable schema without conditional branches. State priority
    // matches TokenLifecycle: revoked > expired > active, so a
    // revoked-and-expired token counts as "revoked" only. Distinct
    // from list_states_summary (per-token rows) and from summary()
    // (which returns a typed struct including total/patients) — this
    // is the friction-free map ops dashboards want when plumbing a
    // pie chart or count strip.
    std::unordered_map<std::string, std::size_t>
    token_lifecycle_summary() const;

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

    // Per-patient mirror of most_recently_granted(), but scoped to the
    // revoked subset: the token belonging to `patient` that is revoked
    // and has the largest issued_at. We do not currently track a
    // distinct revoked_at timestamp, so issued_at is the proxy — newest
    // grant among revoked rows wins. Returns Error::not_found if the
    // patient has no revoked tokens on file. Tiebreak among equal
    // issued_at values is unspecified. Useful for ops probes: "what
    // was the most recent thing we walked back for this patient?"
    Result<ConsentToken>
    most_recently_revoked_for_patient(const PatientId& patient) const;

    // List form of most_recently_revoked_for_patient: every revoked
    // token belonging to `patient`, sorted by issued_at descending
    // (most recently issued first). Distinct from
    // most_recently_revoked_for_patient (which returns one) — this
    // returns the full historical revoked set for the patient.
    // Empty vector if the patient has no revoked tokens. Useful for
    // ops probes that want to walk the revocation history rather
    // than just the most recent row.
    std::vector<ConsentToken>
    tokens_revoked_for_patient(const PatientId& patient) const;

    // Per-patient mirror of oldest_active(), but unconstrained by state:
    // the token (active OR revoked OR expired) belonging to `patient`
    // with the smallest issued_at. Returns Error::not_found if the
    // patient has no tokens on file. Tiebreak among equal issued_at
    // values is unspecified. Useful for ops probes that want the
    // earliest grant on record for a patient regardless of whether
    // it is currently in force — e.g. "when did we first see consent
    // from this patient?"
    Result<ConsentToken>
    find_oldest_token_for_patient(const PatientId& patient) const;

    // Duration from the earliest active (non-revoked, non-expired)
    // token's issued_at to now() — i.e. the wall-clock age of our
    // longest-standing live grant. Returns Error::not_found when no
    // active tokens exist. Operator probe: "how old is the oldest
    // consent we are currently relying on?"
    Result<std::chrono::nanoseconds> age_of_oldest_active() const;

    // Mean (expires_at - issued_at) across every token in the registry
    // (active + revoked + expired) — i.e. the average configured
    // lifespan we have written, regardless of current state. Returns
    // Error::not_found when the registry is empty. Operator probe:
    // "how long are the grants we issue, on average?" — useful as a
    // sanity check that operators aren't drifting toward
    // indefinitely-long TTLs.
    Result<std::chrono::nanoseconds> estimated_avg_ttl() const;

    // Map of {patient.str() (raw id string): total token count across
    // all states (active + revoked + expired)}. Patients with zero
    // tokens do not appear. Used by ops dashboards that want a
    // per-patient breakdown of the registry's footprint without paying
    // for a copy of every token.
    std::unordered_map<std::string, std::size_t>
    token_count_by_patient() const;

    // Cheap count-only mirror of tokens_expiring_within: number of
    // active (non-revoked, non-expired) tokens whose expires_at falls
    // within [now, now + horizon). Sugar for callers that only need
    // the count, no token copies. horizon <= 0 returns 0 (a
    // non-positive horizon has no "soon" window).
    std::size_t
    tokens_expiring_soon(std::chrono::seconds horizon) const;

    // Cheap bool sugar over tokens_expiring_soon: true iff at least one
    // active (non-revoked, non-expired) token's expires_at falls within
    // [now, now + horizon). Equivalent in observable behaviour to
    // `tokens_expiring_soon(horizon) > 0`, but early-exits on the first
    // hit. horizon <= 0 returns false. Useful for ops dashboards
    // wanting a yes/no "do we need to nudge anyone right now?" without
    // counting every match.
    bool has_pending_expiry(std::chrono::seconds horizon) const;

    // True iff at least one token in the registry has the revoked flag
    // set. noexcept; any internal failure is swallowed -> false.
    // Operator probe: "have we ever walked back a grant in this
    // registry's lifetime?"
    bool has_revoked_tokens() const noexcept;

    // Per-patient mirror of has_revoked_tokens(): true iff `patient` has
    // ever had at least one token revoked (the token row may still be
    // in the registry, may have been hard-deleted via remove()... wait,
    // no — remove() drops the row, so hard-deleted revocations are
    // invisible here). Concretely: true iff at least one token currently
    // on file for `patient` has the revoked flag set. noexcept; any
    // internal failure is swallowed → false. Useful for ops dashboards
    // that want to flag patients who have walked back consent at any
    // point — distinct from "do they currently have any active token?"
    // which is the inverse.
    bool has_been_revoked(const PatientId& patient) const noexcept;

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

    // Cheap noexcept inverse of is_token_active for the revoked subset:
    // true iff a token with this id exists AND has its revoked flag
    // set. Returns false if no such token exists (missing != revoked)
    // and false if the token exists but is not revoked (expired-but-
    // not-revoked tokens still report false here). Any internal
    // failure is swallowed → false. Useful for ledger replay paths
    // that want a direct "did we walk this back?" probe without
    // copying the full token through get().
    bool is_revoked(std::string_view token_id) const noexcept;

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

    // Hard-delete every token whose `revoked` flag is set AND whose
    // `issued_at` is older than (now - older_than). Returns the count
    // compacted. Distinct from cleanup_expired() in two ways: this
    // method (a) deletes rows entirely (matching remove() semantics)
    // rather than marking them revoked, and (b) targets the already-
    // revoked pool rather than the not-yet-revoked-but-expired pool.
    // Does NOT fire the observer — like remove(), the operation is
    // intentionally invisible to the audit-ledger replay path; the
    // ledger is the source of truth for permanent history. older_than
    // <= 0 returns 0 (a non-positive window has no compaction
    // candidates). Used by ops to keep the in-memory registry small
    // after a long retention window has elapsed.
    std::size_t compact_state(std::chrono::seconds older_than);

    // Distinct list of purposes that have at least one active (non-revoked,
    // non-expired) token for `patient`. Sorted by underlying enum value so
    // callers can diff snapshots over time. Empty if no active grants.
    // Operator probe: "what can we currently do for this patient?"
    std::vector<Purpose> active_purposes_for_patient(const PatientId& patient) const;

    // Registry-wide mirror of active_purposes_for_patient: distinct list
    // of Purposes that have at least one active (non-revoked, non-
    // expired) token granting them, regardless of patient. Sorted by
    // underlying enum value so callers can diff snapshots over time.
    // Distinct from "every purpose ever granted" — revoked-only and
    // expired-only purposes do not appear. Operator probe: "what is
    // the registry currently authorizing across the entire patient
    // population?" Empty if the registry has no active grants.
    std::vector<Purpose> distinct_purposes_in_use() const;

    // Count of distinct Purposes for which `patient` has at least one
    // active (non-revoked, non-expired) token. Equivalent to
    // active_purposes_for_patient(patient).size() but cheaper — no Purpose
    // vector is materialized. O(n) over the registry, O(1) memory beyond
    // a small fixed-capacity set sized to the Purpose enum.
    std::size_t active_purpose_count_for_patient(const PatientId& patient) const;

    // Synonym of active_purpose_count_for_patient: number of distinct
    // Purposes covered (union) by `patient`'s currently-active
    // (non-revoked, non-expired) tokens. Spelled with the
    // "count_distinct_purposes" prefix to read naturally in callers
    // asking "how broadly has this patient consented?" — kept
    // separately from active_purpose_count_for_patient() for
    // readability at call sites. Equivalent in observable behaviour.
    // O(n) over the registry, O(1) extra memory beyond a small
    // fixed-capacity bitset sized to the Purpose enum.
    std::size_t
    count_distinct_purposes_for_patient(const PatientId& patient) const;

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

const char* to_string(ConsentRegistry::TokenLifecycle::State) noexcept;

}  // namespace asclepius

#endif  // ASCLEPIUS_CONSENT_HPP
