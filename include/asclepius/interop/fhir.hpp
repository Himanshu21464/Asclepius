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
Result<asclepius::Purpose> purpose_from_v3_code(std::string_view code);

}  // namespace asclepius::fhir

#endif  // ASCLEPIUS_INTEROP_FHIR_HPP
