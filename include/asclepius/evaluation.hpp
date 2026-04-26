// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#ifndef ASCLEPIUS_EVALUATION_HPP
#define ASCLEPIUS_EVALUATION_HPP

#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "asclepius/audit.hpp"
#include "asclepius/core.hpp"
#include "asclepius/hashing.hpp"
#include "asclepius/identity.hpp"

namespace asclepius {

class Ledger;          // fwd
class DriftMonitor;    // fwd

// ---- Inputs --------------------------------------------------------------

struct GroundTruth {
    std::string    inference_id;
    nlohmann::json truth;
    Time           captured_at;
    std::string    source;  // adjudicator/registry/follow-up encounter
};

struct OverrideEvent {
    std::string    inference_id;
    std::string    rationale;
    nlohmann::json corrected;
    ActorId        clinician;
    Time           occurred_at;
};

struct EvaluationWindow {
    Time start;
    Time end;
};

// ---- Outputs -------------------------------------------------------------

struct ModelMetrics {
    std::string model;
    std::uint64_t n_inferences   = 0;
    std::uint64_t n_overrides    = 0;
    std::uint64_t n_blocked      = 0;
    std::uint64_t n_with_truth   = 0;
    double        accuracy       = 0.0;
    double        sensitivity    = 0.0;
    double        specificity    = 0.0;
    double        override_rate  = 0.0;
};

struct EvidenceBundle {
    std::filesystem::path path;
    Hash                  root_hash;
    Time                  exported_at;
    EvaluationWindow      window;
    std::vector<ModelMetrics> per_model;
};

// ---- Harness -------------------------------------------------------------
//
// Captures ground truth and overrides, computes per-model metrics, and emits
// signed evidence bundles. The bundle is a tar archive containing:
//
//   manifest.json         high-level summary, hashes, signing key id
//   ledger_excerpt.jsonl  the relevant ledger entries in order
//   metrics.json          ModelMetrics for each model touched
//   drift.json            DriftMonitor snapshot at window close
//   overrides.jsonl       captured override events
//   ground_truth.jsonl    captured ground-truth events
//   public_key            ed25519 public key (32 bytes)
//   manifest.sig          ed25519 signature over manifest.json
//
// The bundle is verifiable offline given just the public key.

class EvaluationHarness {
public:
    explicit EvaluationHarness(Ledger& ledger, DriftMonitor& drift);

    Result<void> attach_ground_truth(GroundTruth truth);
    Result<void> capture_override(OverrideEvent override_event);

    // Compute per-model metrics for the window.
    Result<std::vector<ModelMetrics>> metrics(EvaluationWindow window) const;

    // Export a signed evidence bundle to a path. Returns a descriptor
    // including the bundle's root hash.
    Result<EvidenceBundle> export_bundle(EvaluationWindow      window,
                                         std::filesystem::path out_path) const;

    // Verify a bundle file produced by export_bundle. Pure function: returns
    // true iff the manifest signature matches and contained hashes resolve.
    static Result<bool> verify_bundle(std::filesystem::path bundle_path);

private:
    Ledger&        ledger_;
    DriftMonitor&  drift_;
    mutable std::mutex                                       mu_;
    std::unordered_map<std::string, GroundTruth>             ground_truth_;
    std::vector<OverrideEvent>                               overrides_;
};

}  // namespace asclepius

#endif  // ASCLEPIUS_EVALUATION_HPP
