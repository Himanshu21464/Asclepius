// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include "asclepius/interop/fhir.hpp"
#include "asclepius/consent.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>

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
    if (code == "TREAT")      return Purpose::ambient_documentation;
    if (code == "DIAGNOST")   return Purpose::diagnostic_suggestion;
    if (code == "TRIAGE")     return Purpose::triage;
    if (code == "RXMG")       return Purpose::medication_review;
    if (code == "HRESCH")     return Purpose::research;
    if (code == "QI")         return Purpose::quality_improvement;
    if (code == "HOPERAT")    return Purpose::operations;
    if (code == "RX-RESOLVE") return Purpose::prescription_resolution;
    if (code == "2NDOPN")     return Purpose::second_opinion;
    if (code == "REFERRAL")   return Purpose::specialist_referral;
    if (code == "BILLAUDIT")  return Purpose::billing_audit;
    if (code == "OUTCOMES")   return Purpose::longitudinal_outcomes_research;
    if (code == "ETREAT")     return Purpose::emergency_clinical_access;
    return Error::invalid(std::string{"unknown v3 ActReason: "} + std::string{code});
}

const char* purpose_to_v3_code(asclepius::Purpose p) noexcept {
    switch (p) {
        case asclepius::Purpose::ambient_documentation:           return "TREAT";
        case asclepius::Purpose::diagnostic_suggestion:           return "DIAGNOST";
        case asclepius::Purpose::triage:                          return "TRIAGE";
        case asclepius::Purpose::medication_review:               return "RXMG";
        case asclepius::Purpose::risk_stratification:             return "RISKMG";
        case asclepius::Purpose::quality_improvement:             return "QI";
        case asclepius::Purpose::research:                        return "HRESCH";
        case asclepius::Purpose::operations:                      return "HOPERAT";
        case asclepius::Purpose::prescription_resolution:         return "RX-RESOLVE";
        case asclepius::Purpose::second_opinion:                  return "2NDOPN";
        case asclepius::Purpose::specialist_referral:             return "REFERRAL";
        case asclepius::Purpose::billing_audit:                   return "BILLAUDIT";
        case asclepius::Purpose::longitudinal_outcomes_research:  return "OUTCOMES";
        case asclepius::Purpose::emergency_clinical_access:       return "ETREAT";
    }
    return "unknown";
}

const char* family_relation_to_role_code(asclepius::FamilyRelation r) noexcept {
    switch (r) {
        case asclepius::FamilyRelation::adult_child_for_elder_parent: return "CHILD";
        case asclepius::FamilyRelation::parent_for_minor:             return "PRN";
        case asclepius::FamilyRelation::legal_guardian_for_ward:      return "GUARD";
        case asclepius::FamilyRelation::spouse_for_spouse:            return "SPS";
    }
    return "unknown";
}

Result<asclepius::FamilyRelation>
family_relation_from_role_code(std::string_view code) {
    if (code == "CHILD") return asclepius::FamilyRelation::adult_child_for_elder_parent;
    if (code == "PRN")   return asclepius::FamilyRelation::parent_for_minor;
    if (code == "GUARD") return asclepius::FamilyRelation::legal_guardian_for_ward;
    if (code == "SPS")   return asclepius::FamilyRelation::spouse_for_spouse;
    return Error::invalid(std::string{"unknown role code: "} + std::string{code});
}

namespace {

const char* artefact_status_to_consent_status(
    asclepius::ConsentArtefact::Status s) noexcept {
    switch (s) {
        case asclepius::ConsentArtefact::Status::granted: return "active";
        case asclepius::ConsentArtefact::Status::revoked: return "inactive";
        case asclepius::ConsentArtefact::Status::expired: return "inactive";
    }
    return "unknown";
}

std::optional<std::string>
extract_meta_tag(const nlohmann::json& bundle, std::string_view system) {
    if (!bundle.is_object()) return std::nullopt;
    if (!bundle.contains("meta") || !bundle["meta"].is_object()) return std::nullopt;
    const auto& meta = bundle["meta"];
    if (!meta.contains("tag") || !meta["tag"].is_array()) return std::nullopt;
    for (const auto& tag : meta["tag"]) {
        if (!tag.is_object()) continue;
        if (!tag.contains("system") || !tag["system"].is_string()) continue;
        if (tag["system"].get<std::string>() != system) continue;
        if (!tag.contains("code") || !tag["code"].is_string()) continue;
        return tag["code"].get<std::string>();
    }
    return std::nullopt;
}

}  // namespace

std::string bundle_from_artefact(const asclepius::ConsentArtefact& a) {
    nlohmann::json provisions = nlohmann::json::array();
    for (const auto& p : a.purposes) {
        provisions.push_back({
            {"system", "https://terminology.hl7.org/CodeSystem/v3-ActReason"},
            {"code",   purpose_to_v3_code(p)},
        });
    }

    nlohmann::json consent = {
        {"resourceType", "Consent"},
        {"id",           a.artefact_id},
        {"status",       artefact_status_to_consent_status(a.status)},
        {"scope", {
            {"coding", nlohmann::json::array({nlohmann::json{
                {"system", "http://terminology.hl7.org/CodeSystem/consentscope"},
                {"code",   "patient-privacy"},
            }})},
        }},
        {"category", nlohmann::json::array({nlohmann::json{
            {"coding", nlohmann::json::array({nlohmann::json{
                {"system",  "http://loinc.org"},
                {"code",    "57016-8"},
                {"display", "Privacy policy acknowledgment"},
            }})},
        }})},
        {"patient", {
            {"reference", std::string{"Patient/"} + std::string{a.patient.str()}},
        }},
        {"organization", nlohmann::json::array({nlohmann::json{
            {"reference", std::string{"Organization/"} + a.fetcher_id},
        }})},
        {"performer", nlohmann::json::array({nlohmann::json{
            {"reference", std::string{"Organization/"} + a.requester_id},
        }})},
        {"provision", {
            {"type",   "permit"},
            {"period", {
                {"start", a.issued_at.iso8601()},
                {"end",   a.expires_at.iso8601()},
            }},
            {"purpose", provisions},
        }},
    };

    nlohmann::json patient_stub = {
        {"resourceType", "Patient"},
        {"id",           std::string{a.patient.str()}},
    };

    nlohmann::json bundle = {
        {"resourceType", "Bundle"},
        {"type",         "collection"},
        {"meta", {
            {"tag", nlohmann::json::array({
                nlohmann::json{
                    {"system", "https://abdm.gov.in/CodeSystem/hiu"},
                    {"code",   a.requester_id},
                },
                nlohmann::json{
                    {"system", "https://abdm.gov.in/CodeSystem/hip"},
                    {"code",   a.fetcher_id},
                },
                nlohmann::json{
                    {"system", "https://asclepius.health/CodeSystem/schema-version"},
                    {"code",   a.schema_version.empty() ? std::string{"1.0"} : a.schema_version},
                },
            })},
        }},
        {"entry", nlohmann::json::array({
            nlohmann::json{{"resource", consent}},
            nlohmann::json{{"resource", patient_stub}},
        })},
    };

    return bundle.dump();
}

Result<std::size_t> well_formed_bundle(const nlohmann::json& bundle) {
    if (!bundle.is_object()) {
        return Error::invalid("bundle must be a JSON object");
    }
    if (!bundle.contains("resourceType") ||
        !bundle["resourceType"].is_string() ||
        bundle["resourceType"].get<std::string>() != "Bundle") {
        return Error::invalid("bundle.resourceType must be \"Bundle\"");
    }
    if (!bundle.contains("entry") || !bundle["entry"].is_array()) {
        return Error::invalid("bundle.entry must be an array");
    }
    std::size_t n = 0;
    for (const auto& e : bundle["entry"]) {
        if (!e.is_object() || !e.contains("resource") ||
            !e["resource"].is_object() ||
            !e["resource"].contains("resourceType") ||
            !e["resource"]["resourceType"].is_string()) {
            return Error::invalid(
                "bundle.entry has malformed entry without resource.resourceType");
        }
        ++n;
    }
    return n;
}

Result<asclepius::ConsentArtefact>
artefact_from_bundle(const nlohmann::json& bundle) {
    auto wf = well_formed_bundle(bundle);
    if (!wf) return wf.error();

    const nlohmann::json* consent = nullptr;
    for (const auto& e : bundle["entry"]) {
        const auto& r = e["resource"];
        if (r["resourceType"].get<std::string>() == "Consent") {
            consent = &r;
            break;
        }
    }
    if (!consent) {
        return Error::invalid("bundle has no Consent entry");
    }

    asclepius::ConsentArtefact a;

    if (!consent->contains("id") || !(*consent)["id"].is_string()) {
        return Error::invalid("Consent.id missing");
    }
    a.artefact_id = (*consent)["id"].get<std::string>();

    if (!consent->contains("patient") ||
        !(*consent)["patient"].is_object() ||
        !(*consent)["patient"].contains("reference") ||
        !(*consent)["patient"]["reference"].is_string()) {
        return Error::invalid("Consent.patient.reference missing");
    }
    {
        auto ref = (*consent)["patient"]["reference"].get<std::string>();
        const std::string prefix = "Patient/";
        if (ref.rfind(prefix, 0) == 0) ref.erase(0, prefix.size());
        a.patient = asclepius::PatientId{ref};
    }

    if (!consent->contains("status") || !(*consent)["status"].is_string()) {
        return Error::invalid("Consent.status missing");
    }
    {
        const auto s = (*consent)["status"].get<std::string>();
        if      (s == "active")   a.status = asclepius::ConsentArtefact::Status::granted;
        else if (s == "inactive") a.status = asclepius::ConsentArtefact::Status::revoked;
        else if (s == "expired")  a.status = asclepius::ConsentArtefact::Status::expired;
        else return Error::invalid(std::string{"unknown Consent.status: "} + s);
    }

    if (!consent->contains("provision") || !(*consent)["provision"].is_object()) {
        return Error::invalid("Consent.provision missing");
    }
    const auto& prov = (*consent)["provision"];

    if (!prov.contains("period") || !prov["period"].is_object() ||
        !prov["period"].contains("start") || !prov["period"]["start"].is_string() ||
        !prov["period"].contains("end")   || !prov["period"]["end"].is_string()) {
        return Error::invalid("Consent.provision.period.{start,end} missing");
    }
    a.issued_at  = asclepius::Time::from_iso8601(prov["period"]["start"].get<std::string>());
    a.expires_at = asclepius::Time::from_iso8601(prov["period"]["end"].get<std::string>());

    if (!prov.contains("purpose") || !prov["purpose"].is_array()) {
        return Error::invalid("Consent.provision.purpose missing or not array");
    }
    for (const auto& code_entry : prov["purpose"]) {
        if (!code_entry.is_object() ||
            !code_entry.contains("code") || !code_entry["code"].is_string()) {
            return Error::invalid("Consent.provision.purpose entry missing code");
        }
        auto p = purpose_from_v3_code(code_entry["code"].get<std::string>());
        if (!p) return p.error();
        a.purposes.push_back(p.value());
    }

    auto hip = extract_hip_id(bundle);
    auto hiu = extract_hiu_id(bundle);
    if (hiu) a.requester_id = *hiu;
    if (hip) a.fetcher_id   = *hip;

    a.schema_version = "1.0";
    if (auto v = extract_meta_tag(bundle,
                                  "https://asclepius.health/CodeSystem/schema-version")) {
        a.schema_version = *v;
    }

    return a;
}

std::optional<std::string>
extract_abha_id(const nlohmann::json& patient_resource) {
    if (!patient_resource.is_object()) return std::nullopt;
    if (!patient_resource.contains("identifier") ||
        !patient_resource["identifier"].is_array()) return std::nullopt;
    for (const auto& ident : patient_resource["identifier"]) {
        if (!ident.is_object()) continue;
        if (!ident.contains("system") || !ident["system"].is_string()) continue;
        if (ident["system"].get<std::string>() !=
            "https://healthid.ndhm.gov.in/") continue;
        if (!ident.contains("value") || !ident["value"].is_string()) continue;
        return ident["value"].get<std::string>();
    }
    return std::nullopt;
}

std::optional<std::string>
extract_hip_id(const nlohmann::json& bundle) {
    return extract_meta_tag(bundle, "https://abdm.gov.in/CodeSystem/hip");
}

std::optional<std::string>
extract_hiu_id(const nlohmann::json& bundle) {
    return extract_meta_tag(bundle, "https://abdm.gov.in/CodeSystem/hiu");
}

}  // namespace asclepius::fhir
