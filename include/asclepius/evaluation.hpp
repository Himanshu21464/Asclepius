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
#include "asclepius/policy.hpp"

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

// ============================================================================
// Round 95 — TeleConsultEnvelope and BillAuditBundle
// ============================================================================
//
// Substrate primitives for two India-healthtech evaluation flows:
//   * tele-consult attestation (Itch 7 — SpecHub): both the patient and
//     the clinician sign an envelope binding their identities to the
//     consult metadata, the video recording hash, and the transcript hash.
//     Either signature alone is meaningful (proof of unilateral attestation),
//     but is_fully_signed() requires both.
//   * bill audit attestation (Itch 9 — BillBuster): an auditor signs a
//     structured audit of a bill against a named reference rate table
//     (CGHS-2025, PMJAY package rates, operator-private), with one
//     LineFinding per item flagged as over- / under- / consistent.

struct TeleConsultEnvelope {
    std::string                                   consult_id;
    PatientId                                     patient;
    ActorId                                       clinician;
    std::string                                   topic;
    Time                                          started_at;
    Time                                          ended_at;
    Hash                                          video_hash;
    Hash                                          transcript_hash;

    std::array<std::uint8_t, KeyStore::sig_bytes> patient_signature{};
    std::array<std::uint8_t, KeyStore::pk_bytes>  patient_public_key{};
    std::array<std::uint8_t, KeyStore::sig_bytes> clinician_signature{};
    std::array<std::uint8_t, KeyStore::pk_bytes>  clinician_public_key{};
};

// Build an unsigned envelope. video_hash and transcript_hash may be the
// zero Hash if the consult had no recording or transcript.
TeleConsultEnvelope
make_envelope(std::string consult_id,
              PatientId   patient,
              ActorId     clinician,
              std::string topic,
              Time        started_at,
              Time        ended_at,
              Hash        video_hash      = Hash{},
              Hash        transcript_hash = Hash{});

// Apply the clinician's / patient's signature in place. Replaces any
// previous signature on that side.
TeleConsultEnvelope& sign_as_clinician(TeleConsultEnvelope&, const KeyStore& clinician_key);
TeleConsultEnvelope& sign_as_patient  (TeleConsultEnvelope&, const KeyStore& patient_key);

// Verify each side's signature against the embedded public_key.
bool verify_clinician_signature(const TeleConsultEnvelope&) noexcept;
bool verify_patient_signature  (const TeleConsultEnvelope&) noexcept;

// True iff both signatures verify.
bool is_fully_signed(const TeleConsultEnvelope&) noexcept;

// Canonical JSON serialisation (round-trips through envelope_from_json).
std::string envelope_to_json(const TeleConsultEnvelope&);

// Inverse. Returns Error::invalid on malformed input.
Result<TeleConsultEnvelope> envelope_from_json(std::string_view);

// ---- BillAuditBundle ---------------------------------------------------

struct BillLineFinding {
    std::string item_code;
    std::string item_description;
    double      billed_amount    = 0.0;
    double      reference_amount = 0.0;
    enum class Severity : std::uint8_t {
        consistent   = 1,   // 0.5x <= billed/reference <= 1.5x
        over_billed  = 2,   // billed > 1.5x reference
        under_billed = 3,   // billed < 0.5x reference (unusual; flagged)
        unknown_item = 4,   // no reference amount (reference_amount == 0)
    };
    Severity    severity = Severity::consistent;
    std::string note;
};

// Operator-supplied band thresholds for `classify_line`. The substrate
// SHIPS DEFAULT values (over=1.5, under=0.5) for ergonomics, but those
// defaults are not a regulatory or clinical claim — they are an
// industry-customary starting point. Operators with different
// jurisdictional or institutional conventions pass their own
// `Thresholds` and own the policy. The kernel's job is to record the
// classification deterministically, not to mandate it.
struct ClassifyThresholds {
    double over_factor  = 1.5;   // billed > reference * over_factor  -> over_billed
    double under_factor = 0.5;   // billed < reference * under_factor -> under_billed
};

// Classify by billed vs reference using the supplied thresholds (or
// the documented defaults). reference_amount == 0 -> unknown_item
// regardless of billed.
BillLineFinding::Severity
classify_line(double billed_amount,
              double reference_amount,
              ClassifyThresholds = ClassifyThresholds{}) noexcept;

const char* to_string(BillLineFinding::Severity) noexcept;

struct BillAuditBundle {
    std::string                                   bundle_id;
    PatientId                                     patient;
    ActorId                                       auditor;
    std::string                                   hospital_id;
    std::string                                   reference_table;   // e.g. "CGHS-2025"
    std::vector<BillLineFinding>                  findings;
    double                                        total_billed    = 0.0;
    double                                        total_reference = 0.0;
    Time                                          audited_at;

    std::array<std::uint8_t, KeyStore::sig_bytes> signature{};
    std::array<std::uint8_t, KeyStore::pk_bytes>  public_key{};
};

// Sum billed and reference amounts across findings into total_*.
BillAuditBundle& aggregate_totals(BillAuditBundle&);

// Sign the canonical bytes of the audit with the auditor's keystore.
BillAuditBundle& sign_bill_audit(BillAuditBundle&, const KeyStore& auditor_key);

// Verify the embedded signature.
bool verify_bill_audit(const BillAuditBundle&) noexcept;

// Count findings by severity.
struct BillAuditSummary {
    std::size_t total;
    std::size_t consistent;
    std::size_t over_billed;
    std::size_t under_billed;
    std::size_t unknown_item;
};
BillAuditSummary summarise_bill_audit(const BillAuditBundle&) noexcept;

// JSON round-trip.
std::string             bill_audit_to_json(const BillAuditBundle&);
Result<BillAuditBundle> bill_audit_from_json(std::string_view);

// ============================================================================
// Round 96 — SampleIntegrityBundle and CarePathAttestation
// ============================================================================
//
// SampleIntegrityBundle (Itch 5 — VanRoute) records the chain-of-custody
// for a diagnostic sample collected in the field: who collected it, when,
// the cold-chain checkpoint trail, and the final result hash. The whole
// bundle is signed by the collector's organisation, anchoring the
// integrity claim into the audit chain.
//
// CarePathAttestation (Itch 6 — Sakhi Health) is the operator-side
// signed proof that an inference call was evaluated against a configured
// access::Constraint and either allowed or denied. The kernel writes
// these into the audit chain so an external auditor can replay every
// care-path decision the deployment made.

struct SampleIntegrityCheckpoint {
    Time         at;
    std::string  location;
    double       temperature_c = 0.0;
    bool         within_spec   = true;
    std::string  note;
};

struct SampleIntegrityBundle {
    std::string                                   sample_id;
    PatientId                                     patient;
    std::string                                   collected_by;   // collector / van id (free-form)
    Time                                          collected_at;
    std::vector<SampleIntegrityCheckpoint>        checkpoints;
    Hash                                          result_hash;    // hash of final result doc
    std::string                                   resulted_by;    // lab id
    Time                                          resulted_at;

    std::array<std::uint8_t, KeyStore::sig_bytes> signature{};
    std::array<std::uint8_t, KeyStore::pk_bytes>  public_key{};
};

// Sign a bundle with the collector-org's keystore.
SampleIntegrityBundle&
sign_sample_integrity(SampleIntegrityBundle&, const KeyStore& signer);

// Verify the embedded signature.
bool verify_sample_integrity(const SampleIntegrityBundle&) noexcept;

// True iff every checkpoint reports within_spec == true.
bool cold_chain_intact(const SampleIntegrityBundle&) noexcept;

// JSON round-trip.
std::string                      sample_integrity_to_json(const SampleIntegrityBundle&);
Result<SampleIntegrityBundle>    sample_integrity_from_json(std::string_view);

// ---- CarePathAttestation -----------------------------------------------

struct CarePathAttestation {
    std::string                                   attestation_id;
    PatientId                                     patient;
    ActorId                                       attester;
    access::Constraint                            constraint;
    access::Context                               context;
    access::Decision                              decision;
    std::string                                   reason;          // empty when allow
    Time                                          attested_at;

    std::array<std::uint8_t, KeyStore::sig_bytes> signature{};
    std::array<std::uint8_t, KeyStore::pk_bytes>  public_key{};
};

// Evaluate the constraint against the context and build the unsigned
// attestation. attested_at is captured at call time.
CarePathAttestation
make_care_path_attestation(std::string                attestation_id,
                           PatientId                  patient,
                           ActorId                    attester,
                           const access::Constraint&  constraint,
                           const access::Context&     context);

// Sign the attestation in place.
CarePathAttestation&
sign_care_path(CarePathAttestation&, const KeyStore& signer);

bool verify_care_path(const CarePathAttestation&) noexcept;

// JSON round-trip.
std::string             care_path_to_json(const CarePathAttestation&);
Result<CarePathAttestation> care_path_from_json(std::string_view);

}  // namespace asclepius

#endif  // ASCLEPIUS_EVALUATION_HPP
