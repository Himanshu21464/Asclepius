// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include "asclepius/interop/fhir.hpp"
#include "asclepius/consent.hpp"

#include <nlohmann/json.hpp>

namespace asclepius::fhir {

Result<ResourceRef> parse_reference(std::string_view ref) {
    auto slash = ref.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash + 1 >= ref.size()) {
        return Error::invalid(std::string{"malformed FHIR reference: "} + std::string{ref});
    }
    return ResourceRef{
        std::string{ref.substr(0, slash)},
        std::string{ref.substr(slash + 1)},
    };
}

Result<EncounterScope> extract_scope(const nlohmann::json& bundle) {
    if (!bundle.is_object()) return Error::invalid("expected FHIR bundle object");
    EncounterScope scope;

    auto handle_resource = [&](const nlohmann::json& res) {
        if (!res.is_object() || !res.contains("resourceType")) return;
        const auto rt = res["resourceType"].get<std::string>();
        if (!res.contains("id") || !res["id"].is_string()) return;
        const auto id = res["id"].get<std::string>();
        if (rt == "Patient") {
            scope.patient = PatientId::fhir(id);
        } else if (rt == "Encounter") {
            scope.encounter = EncounterId::fhir(id);
        }
    };

    if (bundle.contains("resourceType") && bundle["resourceType"] == "Bundle" &&
        bundle.contains("entry") && bundle["entry"].is_array()) {
        for (const auto& e : bundle["entry"]) {
            if (e.contains("resource")) handle_resource(e["resource"]);
        }
    } else {
        // Treat as a single resource.
        handle_resource(bundle);
    }
    return scope;
}

Result<asclepius::Purpose> purpose_from_v3_code(std::string_view code) {
    if (code == "TREAT")    return Purpose::ambient_documentation;
    if (code == "DIAGNOST") return Purpose::diagnostic_suggestion;
    if (code == "TRIAGE")   return Purpose::triage;
    if (code == "RXMG")     return Purpose::medication_review;
    if (code == "HRESCH")   return Purpose::research;
    if (code == "QI")       return Purpose::quality_improvement;
    if (code == "HOPERAT")  return Purpose::operations;
    return Error::invalid(std::string{"unknown v3 ActReason: "} + std::string{code});
}

}  // namespace asclepius::fhir
