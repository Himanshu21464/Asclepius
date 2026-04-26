// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#ifndef ASCLEPIUS_RUNTIME_HPP
#define ASCLEPIUS_RUNTIME_HPP

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

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

    // Capture a clinician override of the model's output. Stored with the
    // inference id so prospective evaluation can join later.
    Result<void> capture_override(std::string rationale, nlohmann::json corrected);

    // Attach a numeric drift observation under a registered feature.
    Result<void> observe_drift(std::string_view feature, double value);

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

    // Open or create a runtime backed by a SQLite file (default) or a
    // PostgreSQL database via "postgres://" URI. The runtime generates
    // a fresh signing key on first open and persists it next to the
    // SQLite file with 0600 permissions; for Postgres backends a
    // KeyStore must be supplied explicitly.
    static Result<Runtime> open(std::filesystem::path db_path);
    static Result<Runtime> open(std::filesystem::path db_path, KeyStore key);
    static Result<Runtime> open_uri(const std::string& uri);
    static Result<Runtime> open_uri(const std::string& uri, KeyStore key);

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

private:
    Runtime();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace asclepius

#endif  // ASCLEPIUS_RUNTIME_HPP
