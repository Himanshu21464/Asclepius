// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#ifndef ASCLEPIUS_RUNTIME_HPP
#define ASCLEPIUS_RUNTIME_HPP

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "asclepius/audit.hpp"
#include "asclepius/consent.hpp"
#include "asclepius/core.hpp"
#include "asclepius/evaluation.hpp"
#include "asclepius/hashing.hpp"
#include "asclepius/identity.hpp"
#include "asclepius/policy.hpp"
#include "asclepius/telemetry.hpp"

namespace asclepius {

// ---- InferenceContext ---------------------------------------------------
//
// Read-only descriptor of an in-flight inference. Passed to policies so they
// can make purpose-aware decisions without being tightly coupled to Runtime.

class InferenceContext {
public:
    InferenceContext(std::string  inference_id,
                     ModelId      model,
                     ActorId      actor,
                     PatientId    patient,
                     EncounterId  encounter,
                     Purpose      purpose,
                     TenantId     tenant,
                     Time         started_at);

    std::string_view  id()         const noexcept;
    const ModelId&    model()      const noexcept;
    const ActorId&    actor()      const noexcept;
    const PatientId&  patient()    const noexcept;
    const EncounterId& encounter() const noexcept;
    Purpose           purpose()    const noexcept;
    const TenantId&   tenant()     const noexcept;
    Time              started_at() const noexcept;

private:
    std::string  inference_id_;
    ModelId      model_;
    ActorId      actor_;
    PatientId    patient_;
    EncounterId  encounter_;
    Purpose      purpose_;
    TenantId     tenant_;
    Time         started_at_;
};

// ---- InferenceSpec ------------------------------------------------------
//
// Caller-supplied request parameters for begin_inference().

struct InferenceSpec {
    ModelId                         model;
    ActorId                         actor;
    PatientId                       patient;
    EncounterId                     encounter;
    Purpose                         purpose;
    TenantId                        tenant{};
    std::optional<std::string>      consent_token_id{};
};

// ---- Inference (RAII handle) -------------------------------------------
//
// Returned by Runtime::begin_inference. Either commit() or destruct without
// calling commit() to abort. Single-use.

class Runtime;

using ModelCallback = std::function<Result<std::string>(std::string)>;

class Inference {
public:
    Inference(Inference&&) noexcept;
    Inference& operator=(Inference&&) noexcept;
    ~Inference();
    Inference(const Inference&)            = delete;
    Inference& operator=(const Inference&) = delete;

    // Run input through the input policy chain, invoke model_call with the
    // resulting payload, run output through the output policy chain, append
    // the inference event to the ledger. Returns the (possibly modified)
    // model output, or an error if any policy blocked or the model failed.
    Result<std::string> run(std::string input, const ModelCallback& model_call);

    // Same as run() but bounds the model_call duration. If model_call
    // exceeds `timeout`, this method returns Error::timeout and marks
    // the inference as `status: timeout` in its ledger body. The model
    // thread may still be running (no cooperative cancellation primitive
    // is required of the callback) — it's detached and its result is
    // discarded. Subsequent commit() will record the timeout, not the
    // late result.
    Result<std::string> run_with_timeout(
        std::string input,
        const ModelCallback& model_call,
        std::chrono::milliseconds timeout);

    // Caller-driven cancellation. Spawns the model call on a worker thread
    // and polls the supplied CancelToken at `poll_interval`. If the token
    // is cancelled before the model returns, the worker is detached, this
    // method returns Error::cancelled, and the inference's ledger body is
    // marked `status: cancelled`. The detached worker may still be running
    // — its result is discarded. If the model finishes first, the normal
    // output policy chain applies. Combine with run_with_timeout when you
    // need both deadline-based and caller-driven abort: prefer this method
    // for graceful shutdown / user-requested abort, and run_with_timeout
    // for runaway-model protection.
    Result<std::string> run_cancellable(
        std::string input,
        const ModelCallback& model_call,
        CancelToken token,
        std::chrono::milliseconds poll_interval = std::chrono::milliseconds{5});

    // Commit the inference to the ledger. Idempotent within the lifetime
    // of this Inference handle (calling commit() twice on the same handle
    // is a no-op). After commit, drift observations are flushed and
    // ground-truth/override hooks remain valid.
    Result<void> commit();

    // Cross-handle idempotent commit. Scans the last `lookback` ledger
    // entries for one with a matching inference_id; if found, marks this
    // handle as committed WITHOUT inserting a duplicate and returns ok.
    // Use this when the caller cannot guarantee a single Inference handle
    // per logical inference (e.g. stateless retry layer, process restart).
    // Cost: one Ledger::tail(lookback) call per commit; bounded.
    Result<void> commit_idempotent(std::size_t lookback = 200);

    // Attach an arbitrary key/value pair to the inference's ledger body.
    // Stored under a "metadata" sub-object so it can't collide with the
    // runtime's reserved fields (status, input_hash, output_hash, etc.).
    // Useful for cross-system correlation: trace_id, span_id, request_id,
    // sidecar_version, ab_cohort, retry_count.
    //
    // Reserved keys and empty keys are rejected. Values may be any
    // canonicalizable JSON. Calling twice with the same key replaces the
    // prior value. Must be called before commit() — after commit it
    // returns invalid_argument with "metadata after commit".
    Result<void> add_metadata(std::string_view key, nlohmann::json value);

    // True iff add_metadata() was previously called for `key` on this
    // handle. Reads from the same pending body add_metadata writes to
    // (impl_->ledger_body["metadata"]); after commit, returns whatever
    // was set at commit time. Empty key is rejected with
    // invalid_argument — silently treating it as "absent" would mask
    // caller bugs.
    Result<bool> has_metadata(std::string_view key) const;

    // Return the value previously written via add_metadata(key). Returns
    // Error::not_found if no such key has been set on this handle.
    // Empty key is rejected with invalid_argument. Mirrors
    // has_metadata() — works pre-commit, and post-commit returns
    // whatever was set at commit time.
    Result<nlohmann::json> get_metadata(std::string_view key) const;

    // Remove a single metadata entry. No-op if the key is not present,
    // is empty, or the inference has already been committed (the
    // ledger entry is immutable; we don't reach back into the chain).
    // Use before commit() when the caller wants to retract a value
    // they staged earlier in the same handle's lifetime.
    void clear_metadata(std::string_view key) noexcept;

    // Capture a clinician override of the model's output. Stored with the
    // inference id so prospective evaluation can join later.
    Result<void> capture_override(std::string rationale, nlohmann::json corrected);

    // Attach a ground-truth label for prospective evaluation. The truth
    // value (any canonicalizable JSON), the source (registry/follow-up
    // encounter/adjudicator), and the captured-at timestamp are stored
    // against this inference's id. If the inference has not been
    // committed yet this method auto-commits — matches the
    // capture_override pattern. The ground truth is held on the
    // EvaluationHarness, not the ledger, so it can be mutated until the
    // evidence window closes.
    Result<void> attach_ground_truth(nlohmann::json truth,
                                     std::string    source);

    // Attach a numeric drift observation under a registered feature.
    Result<void> observe_drift(std::string_view feature, double value);

    // Return the seq number of the inference.committed entry in the
    // ledger. Only valid AFTER a successful commit() / commit_idempotent()
    // / capture_override() / attach_ground_truth(). Before commit returns
    // invalid_argument; the seq is captured at commit time so subsequent
    // ledger growth doesn't affect it. Used by sidecars to publish a
    // stable cross-system reference for the inference (e.g. as part of a
    // response header, queue metadata, or webhook payload).
    Result<std::uint64_t> seq() const noexcept;

    // Sugar over ctx().tenant(). Saves sidecars one indirection when
    // emitting per-tenant metrics or routing decisions in the hot path.
    const TenantId& tenant() const noexcept;

    // Sugar over ctx().id(). Same motivation as tenant() — used heavily
    // by sidecars that thread the inference id into log lines and trace
    // attributes.
    std::string_view id() const noexcept;

    // True iff commit() has been called successfully. Cheap accessor
    // for sidecars that need to know whether the destructor will
    // record an aborted-inference event.
    bool is_committed() const noexcept;

    // Owning string copy of the actor id (i.e. the human or service
    // that initiated this inference). Sugar over
    // `std::string{ctx().actor().str()}` — useful when the caller
    // needs to capture the actor by value into a logger sink, queue
    // payload, or async task that outlives this handle's stack frame.
    // The underlying `actor().str()` returns a string_view tied to
    // the InferenceContext; this accessor lets callers escape that
    // lifetime without manual conversion at every call site.
    std::string actor_str() const;

    // True iff the inference is committed AND its started_at time is
    // strictly later than `t`. Useful as a "did this inference happen
    // after our last checkpoint?" guard for sidecars walking forward
    // from a saved bookmark. Returns false on uncommitted handles
    // (no terminal record exists yet) and on null impl. The
    // comparison uses the same Time type the ledger header records,
    // so callers can pass a Time captured from a prior ledger entry
    // without conversion.
    bool was_committed_after(Time t) const noexcept;

    // True iff status() begins with "blocked." (i.e. "blocked.input"
    // or "blocked.output"). Cheap accessor — sidecars on the hot path
    // would otherwise re-allocate / re-compare the status string each
    // call. Returns false on a handle that hasn't run, on null impl,
    // or on any non-blocked terminal status (ok / model_error /
    // timeout / cancelled / aborted).
    bool was_blocked() const noexcept;

    // True iff run() / run_with_timeout() / run_cancellable() reached
    // a terminal state on this handle, regardless of whether commit()
    // followed. Mirrors has_run() at the impl level but is named for
    // the post-condition rather than the call: callers reading
    // "did this inference finish?" pair it with is_committed() to
    // distinguish run-but-uncommitted from never-ran. noexcept.
    bool has_completed() const noexcept;

    // JSON snapshot of the pending ledger_body. Read-only debug
    // accessor — useful for sidecars that want to inspect what would
    // be committed before calling commit() (e.g. assert a metadata
    // key was attached, log the staged status without waiting for the
    // chain append). Returns an empty object if impl_ is null;
    // never errors. Independent of commit state — works pre- and
    // post-commit and reflects whatever is currently staged.
    nlohmann::json body_snapshot() const;

    // True iff run() / run_with_timeout() / run_cancellable() has been
    // called on this handle (regardless of outcome). Distinct from
    // is_committed(): a handle can have run==true and committed==false
    // (e.g. caller forgot to commit, or chose to abort). Useful for
    // sidecars that want to log "run-but-not-committed" anomalies
    // without inspecting status() string contents.
    bool has_run() const noexcept;

    // Current ledger-body status string ("ok", "blocked.input",
    // "blocked.output", "model_error", "timeout", "cancelled",
    // "aborted") or an empty string if no run has been attempted yet.
    // Reads from the Inference's pending body, not the ledger; matches
    // what would be written if commit() were called now.
    std::string status() const;

    // Post-policy hash of the input fed to the model, in hex. Empty if
    // no successful run has populated it yet (handle hasn't run, or the
    // input policy chain blocked before hashing). Reads from the pending
    // ledger body so the value is observable pre-commit and survives
    // commit unchanged. Never errors — sidecars use this on the hot path
    // and prefer an empty string over a Result wrapper.
    std::string input_hash() const;

    // Post-policy hash of the model output, in hex. Same semantics as
    // input_hash(): empty until the run reaches the post-output-policy
    // stage. timeout / cancelled / blocked.output runs leave it empty
    // by design — no canonical output was produced.
    std::string output_hash() const;

    // Wallclock milliseconds elapsed since started_at(). Convenience
    // sugar for sidecar dashboards that don't want to convert
    // chrono::nanoseconds manually each time.
    std::int64_t elapsed_ms() const noexcept;

    const InferenceContext& ctx() const noexcept;

private:
    friend class Runtime;
    struct Impl;
    explicit Inference(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};

// ---- Runtime -----------------------------------------------------------
//
// Top-level handle. Owns the ledger, policy chain, drift monitor, consent
// registry, and evaluation harness. Thread-safe — multiple threads may call
// begin_inference concurrently. Component sub-objects each manage their own
// internal locking.

class Runtime {
public:
    Runtime(Runtime&&) noexcept;
    Runtime& operator=(Runtime&&) noexcept;
    ~Runtime();
    Runtime(const Runtime&)            = delete;
    Runtime& operator=(const Runtime&) = delete;

    // Open or create a SQLite-backed runtime at the given filesystem path.
    // The runtime generates a fresh signing key on first open and persists
    // it next to the SQLite file with 0600 permissions.
    static Result<Runtime> open(std::filesystem::path db_path);
    static Result<Runtime> open(std::filesystem::path db_path, KeyStore key);

    // Begin an inference. Performs consent and scope checks before
    // returning the handle. The returned Inference must be commit()ed to
    // persist; otherwise it is treated as aborted.
    Result<Inference> begin_inference(InferenceSpec spec);

    // Component access.
    PolicyChain&        policies();
    Ledger&             ledger();
    DriftMonitor&       drift();
    ConsentRegistry&    consent();
    EvaluationHarness&  evaluation();
    MetricRegistry&     metrics();

    const PolicyChain&  policies() const;
    const Ledger&       ledger()   const;

    // ---- Health snapshot ------------------------------------------------
    //
    // A single-call summary of the runtime's overall liveness. Designed
    // for /healthz endpoints, deployment readiness probes, and operator
    // dashboards. The fields are O(1)/bounded — no full chain scan.

    struct Health {
        bool          ok = true;        // false if any subsystem unhealthy
        std::uint64_t ledger_length = 0;
        std::string   ledger_head_hex;
        std::string   ledger_key_id;
        std::size_t   policy_count = 0;
        std::size_t   active_consent_tokens = 0;
        std::size_t   drift_features = 0;
        std::string   reason;           // populated when ok == false

        std::string to_json() const;
    };

    Health health() const;

    // True iff health().ok is true. Cheap, allocation-free wrapper
    // for /healthz endpoints and sidecar gates that only need the
    // boolean liveness verdict. Equivalent to health().ok but
    // saves callers the Health struct construction when they
    // don't care about its fields.
    bool is_healthy() const noexcept;

    // Single-line human-readable status string. Format:
    //   "OK · N entries · K active consent · F drift features · P policies"
    // or "EMPTY" when the runtime has never been written to. Used by
    // sidecar log lines and short health probes that want a one-line
    // summary without parsing JSON. Pulls from health() + ledger
    // length; never errors. The middle dot (·) separator is U+00B7
    // — the same character the website uses in eyebrow lines.
    std::string quick_status() const;

    // Multi-line, human-readable runtime summary. Distinct from
    // quick_status() — that's a single line for log eyebrows, this
    // is the verbose form used by sidecar startup banners and the
    // `asclepius runtime status` CLI surface. Roughly 6-8 lines,
    // includes: ledger length, head hex (truncated to 12 chars), key
    // id, signing fingerprint, active consent token count, drift
    // feature count, policy count, runtime version. Pulls from
    // health() + version() + keystore_fingerprint() — never errors.
    std::string summary_string() const;

    // Sugar over ledger().key_id(). The ledger's signing key id is
    // routinely surfaced in sidecar logs alongside the head hash, but
    // grabbing it requires the caller to drill through the Ledger&
    // accessor; this trivial wrapper saves the indirection at the
    // call site (and matches the framing used by other sugar
    // accessors here — head_hash(), keystore_fingerprint() etc.).
    std::string signing_key_id() const;

    // Cheap bool over consent().permits(patient, purpose). Sidecars
    // running pre-flight checks before begin_inference() typically
    // want a yes/no verdict and treat any registry error the same as
    // "not permitted" — this wrapper folds the Result<bool> into a
    // bool via .value_or(false), saving the boilerplate at every
    // gate. Use begin_inference() itself for the auth-grade check
    // (which surfaces the specific consent_missing / expired error).
    bool has_consent_for(const PatientId& patient, Purpose purpose) const;

    // Wallclock duration since the oldest entry in the chain.
    // Returns std::chrono::nanoseconds::zero() on an empty chain
    // (and on any read error — diagnostics belong on health(), not
    // here). Used by ops dashboards as a coarse "how long has this
    // runtime been recording?" gauge. Implementation reads ledger().
    // at(1); the call is bounded — one indexed lookup, not a scan.
    std::chrono::nanoseconds ledger_age() const;

    // Sugar over ledger().length(). Saves callers the indirection
    // of grabbing a Ledger& reference when they only want the count
    // (e.g. /healthz extras, sidecar dashboards, smoke tests).
    std::size_t ledger_length() const noexcept;

    // Sugar over ledger().head().hex(). Same motivation as
    // ledger_length() — used by sidecars that want to publish the
    // current chain head without juggling Ledger& and Hash types.
    std::string head_hash() const;

    // Sugar over ledger().checkpoint(). Produces a self-contained,
    // signed beacon of the current chain head (seq + head_hash + ts +
    // signature) without requiring callers to grab the Ledger&
    // reference. Used by sidecars publishing periodic external
    // attestations of the chain — the "self_attest" framing matches
    // operator vocabulary for "the runtime is signing its own head."
    LedgerCheckpoint self_attest() const;

    // Sugar over the ledger's signing-key fingerprint (an 8-byte
    // BLAKE2b hash of the public key, hex-encoded — same shape as
    // Ledger::Attestation::fingerprint and KeyStore::fingerprint).
    // Some operators prefer the "keystore" framing when surfacing
    // this in dashboards and audit reports; this accessor lets
    // sidecars use that vocabulary without grabbing Ledger&.
    std::string keystore_fingerprint() const;

    // Sugar over policies().names(). Trivial wrapper for callers that
    // want the in-order list of registered policy names without
    // grabbing the PolicyChain& reference (status endpoints, sidecar
    // dashboards, smoke tests). The returned vector is a copy.
    std::vector<std::string> policy_names() const;

    // True iff the ledger holds zero entries. Cheap accessor for
    // "is this a fresh runtime?" probes — sidecars use it to gate
    // first-boot setup (e.g. install_default_policies, seed initial
    // consent) without grabbing Ledger&. noexcept; never reads the
    // backend beyond the existing length() call.
    bool is_chain_empty() const noexcept;

    // Convenience: clear() the policy chain and push the standard
    // production set — make_phi_scrubber() and a reasonable
    // make_length_limit(64*1024, 64*1024). Used by sidecars that
    // want a sensible default without manually composing policies.
    // Returns ok unconditionally.
    Result<void> install_default_policies();

    // Asclepius runtime version string. Returns ASCLEPIUS_VERSION_STRING
    // when defined via target_compile_definitions, otherwise a stable
    // "0.0.0-dev" fallback. Used by sidecars and CLI clients reporting
    // runtime build info into logs and /healthz extras.
    std::string version() const;

    // Rough estimate of inferences started but not yet at a terminal
    // state, derived from counter deltas (begin/attempts vs the union of
    // ok / timeout / cancelled / model_error / blocked.input /
    // blocked.output / idempotent_dedupe). Underflow-clamped to 0; not
    // a strong real-time gauge — counters can be reset_metrics()'d out
    // from under it. Best-effort observability sugar for dashboards.
    std::size_t active_inference_count() const;

    // Size in bytes of the ledger backing storage — the on-disk size of
    // the .db file at the time of the call (subject to WAL checkpoint
    // timing). Used by capacity-planning probes and quota dashboards.
    Result<std::int64_t> ledger_size_bytes() const;

    // Snapshot of the names of all currently-registered drift
    // features. Convenience wrapper around drift().list_features()
    // for callers that want a stable accessor without grabbing the
    // DriftMonitor& reference (e.g. /healthz extras, sidecar
    // dashboards). The returned vector is a copy; mutating it has
    // no effect on the runtime.
    std::vector<std::string> list_loaded_features() const;

    // Reset every counter currently registered in the metric
    // registry to zero. Histograms are NOT reset. Useful for
    // per-deploy resets so a fresh deployment doesn't inherit the
    // previous build's traffic. Returns ok if every counter could
    // be reset; the first failure short-circuits and is returned.
    Result<void> reset_metrics();

    // Verify the last `lookback` entries of the chain. Sugar over
    // ledger().verify_range(length-lookback+1, length+1) with bounds
    // clamping when lookback exceeds the chain length. lookback == 0 is
    // rejected with invalid_argument — the caller almost certainly
    // wanted at least one entry, and a zero-width range is a no-op
    // worth catching. Returns ok() on the empty chain (nothing to check).
    // Used as a fast post-restart sanity probe before serving traffic:
    // a full verify() may scan millions of entries; spot-checking the
    // tail catches the common corruption modes (last-write torn, WAL
    // never replayed) in O(lookback).
    Result<void> audit_spot_check(std::size_t lookback) const;

    // ---- System summary -------------------------------------------------
    //
    // Single-call aggregate returned by value for status endpoints. Pulls
    // from health() plus version() plus metrics().counter_count() so a
    // /status responder doesn't need to grab three separate accessors and
    // assemble its own struct. Strictly observability sugar — never
    // errors, never blocks on I/O beyond what health() already does.

    struct SystemSummary {
        std::uint64_t ledger_length = 0;
        std::string   head_hash_hex;
        std::size_t   policy_count = 0;
        std::size_t   active_consent = 0;
        std::size_t   drift_features = 0;
        std::size_t   total_counters = 0;
        std::string   version;
    };

    SystemSummary system_summary() const;

    // Sugar over metrics().count("inference.attempts"). Wraps in a
    // Result so future expansions can report registry errors without
    // breaking callers; today it is infallible. Saves callers who only
    // want the dispatched-inference counter from grabbing the
    // MetricRegistry& reference.
    Result<std::size_t> dispatched_inferences() const;

    // Run a battery of internal invariants. Used at boot and by /healthz
    // deep-check endpoints to catch corruption early. Currently checks:
    //  - ledger.verify() succeeds on the full chain
    //  - consent state is internally consistent (no expired-but-active)
    //  - drift state has no NaN baselines
    // Returns the first failing invariant as an Error, or ok() if
    // everything passes. May be slow on large chains — gated for slow
    // probes, not p99 paths.
    Result<void> self_test() const;

private:
    Runtime();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace asclepius

#endif  // ASCLEPIUS_RUNTIME_HPP
