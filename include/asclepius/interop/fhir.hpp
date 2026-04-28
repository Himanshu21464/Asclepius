// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#ifndef ASCLEPIUS_INTEROP_FHIR_HPP
#define ASCLEPIUS_INTEROP_FHIR_HPP

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

#include "asclepius/consent.hpp"
#include "asclepius/core.hpp"
#include "asclepius/identity.hpp"

namespace asclepius::fhir {

// Asclepius does not ingest, validate, or normalize FHIR resources. It is
// FHIR-aware in the narrow sense that it can extract the few fields needed
// to scope an inference: Patient/<id>, Encounter/<id>, Practitioner/<id>,
// and a coded Purpose-of-use.
//
// This keeps the substrate vendor-neutral and avoids becoming a half-baked
// FHIR server. Real FHIR validation belongs in HAPI/Pathling/Aidbox.

struct ResourceRef {
    std::string resource_type;  // "Patient", "Encounter", ...
    std::string id;
};

Result<ResourceRef> parse_reference(std::string_view ref);

// Extract Patient and Encounter references from a FHIR Bundle JSON, if
// present. Returns nullopt for either if not found.
struct EncounterScope {
    std::optional<PatientId>    patient;
    std::optional<EncounterId>  encounter;
};

Result<EncounterScope> extract_scope(const nlohmann::json& bundle);

// Map a US Core / IPS coded purpose-of-use to an Asclepius Purpose.
//
//   "TREAT"     -> ambient_documentation (closest treatment-of-care fit)
//   "DIAGNOST"  -> diagnostic_suggestion
//   "TRIAGE"    -> triage
//   "RXMG"      -> medication_review
//   "HRESCH"    -> research
//   "QI"        -> quality_improvement
//   "HOPERAT"   -> operations
//
// India profile additions (round 91):
//   "RX-RESOLVE" -> prescription_resolution     (PMBJP / NPPA reconciliation)
//   "2NDOPN"     -> second_opinion              (ABDM HIU consult)
//   "REFERRAL"   -> specialist_referral         (tier-2/3 tele-referral)
//   "BILLAUDIT"  -> billing_audit               (CGHS / IRDAI line-item audit)
//   "OUTCOMES"   -> longitudinal_outcomes_research
//   "ETREAT"     -> emergency_clinical_access   (DPDP § 7 break-glass)
//
// "HOPERAT" / "ETREAT" follow HL7 v3 ActReason naming; the four India-
// specific codes are kernel-local until ABDM publishes formal coding.
Result<asclepius::Purpose> purpose_from_v3_code(std::string_view code);

// ---- Round 91: ABDM / India interop helpers -----------------------------
//
// Substrate-shaped: the kernel emits and extracts shapes; it does not
// validate against the ABDM consent profile (a real validator belongs in
// HAPI / Pathling / a vendor adapter). Output JSON is opinionated but
// vendor-neutral.

// Reverse of purpose_from_v3_code. noexcept; returns "unknown" for
// unmapped values rather than failing — useful in template / serializer
// paths that cannot fail.
const char* purpose_to_v3_code(asclepius::Purpose) noexcept;

// Emit a FHIR Bundle (type "collection") whose first entry is a Consent
// resource mirroring the artefact, with a Patient stub supplying the
// patient identity. The bundle is JSON-serialised. The kernel does NOT
// claim ABDM-profile conformance; it produces a conservative shape that
// downstream profiles can post-process.
std::string bundle_from_artefact(const asclepius::ConsentArtefact&);

// Inverse of bundle_from_artefact. Loose: any well-formed bundle with
// one Consent entry whose `provision.purpose` codes map to known
// Purposes is accepted. Returns Error::invalid on shape problems,
// missing fields, or unknown purpose / status strings.
Result<asclepius::ConsentArtefact>
artefact_from_bundle(const nlohmann::json& bundle);

// Extract an ABHA identifier from a FHIR Patient resource. ABDM
// convention: Patient.identifier[*].system ==
// "https://healthid.ndhm.gov.in/" with the value being the 14-digit
// ABHA number. Returns nullopt if not present. Never errors.
std::optional<std::string>
extract_abha_id(const nlohmann::json& patient_resource);

// Extract the HIP (Health Information Provider) identifier from a
// Bundle's meta.tag list. Convention: tag.system ==
// "https://abdm.gov.in/CodeSystem/hip"; tag.code is the HIP id.
// Returns nullopt if not present. Never errors.
std::optional<std::string>
extract_hip_id(const nlohmann::json& bundle);

// Extract the HIU (Health Information User) identifier. Same shape as
// HIP but tag.system "https://abdm.gov.in/CodeSystem/hiu".
std::optional<std::string>
extract_hiu_id(const nlohmann::json& bundle);

// Minimal shape-check on a FHIR Bundle: it has resourceType "Bundle",
// an "entry" array, every entry has a "resource" object with a
// "resourceType" string. NOT a validator; a precondition gate so
// callers can fail-fast on obvious shape issues. Returns the count of
// valid entries on success; Error::invalid otherwise with a description
// of the first problem encountered.
Result<std::size_t> well_formed_bundle(const nlohmann::json& bundle);

// Map a FamilyRelation to an HL7 v3 RoleCode used in ABDM consent
// bundles where the consent comes from a proxy (parent for minor,
// adult-child for elder, guardian, spouse). Codes:
//   adult_child_for_elder_parent -> "SONC" / "DAUC" generalised to "CHILD"
//   parent_for_minor             -> "PRN"
//   legal_guardian_for_ward      -> "GUARD"
//   spouse_for_spouse            -> "SPS"
const char* family_relation_to_role_code(asclepius::FamilyRelation) noexcept;

// Inverse of family_relation_to_role_code. Returns Error::invalid for
// unknown role codes.
Result<asclepius::FamilyRelation>
family_relation_from_role_code(std::string_view code);

}  // namespace asclepius::fhir

#endif  // ASCLEPIUS_INTEROP_FHIR_HPP
