// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include "asclepius/consent.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <string>
#include <string_view>
#include <utility>

namespace asclepius {

// ---- ConsentArtefact::Status <-> string ---------------------------------

const char* to_string(ConsentArtefact::Status s) noexcept {
    switch (s) {
        case ConsentArtefact::Status::granted: return "granted";
        case ConsentArtefact::Status::revoked: return "revoked";
        case ConsentArtefact::Status::expired: return "expired";
    }
    return "unknown";
}

Result<ConsentArtefact::Status>
artefact_status_from_string(std::string_view s) noexcept {
    if (s == "granted") return ConsentArtefact::Status::granted;
    if (s == "revoked") return ConsentArtefact::Status::revoked;
    if (s == "expired") return ConsentArtefact::Status::expired;
    return Error::invalid("unknown artefact status");
}

// ---- ABDM JSON shape ----------------------------------------------------

std::string to_abdm_json(const ConsentArtefact& a) {
    nlohmann::json purposes = nlohmann::json::array();
    for (auto p : a.purposes) {
        purposes.push_back(to_string(p));
    }
    nlohmann::json out;
    out["artefact_id"]    = a.artefact_id;
    out["patient"]        = std::string{a.patient.str()};
    out["requester_id"]   = a.requester_id;
    out["fetcher_id"]     = a.fetcher_id;
    out["purposes"]       = std::move(purposes);
    out["issued_at"]      = a.issued_at.iso8601();
    out["expires_at"]     = a.expires_at.iso8601();
    out["status"]         = to_string(a.status);
    out["schema_version"] = a.schema_version.empty()
                                ? std::string{"1.0"}
                                : a.schema_version;
    return out.dump();
}

Result<ConsentArtefact> from_abdm_json(std::string_view s) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(s);
    } catch (const std::exception& e) {
        return Error::invalid(std::string{"malformed json: "} + e.what());
    }
    if (!j.is_object()) {
        return Error::invalid("malformed json: expected object");
    }

    // Validate required fields up front so the error message names which one
    // is missing.
    static constexpr const char* required[] = {
        "artefact_id", "patient", "requester_id", "fetcher_id",
        "purposes",    "issued_at", "expires_at", "status",
    };
    for (const char* name : required) {
        if (!j.contains(name)) {
            return Error::invalid(std::string{"missing field: "} + name);
        }
    }

    if (!j["artefact_id"].is_string() ||
        !j["patient"].is_string() ||
        !j["requester_id"].is_string() ||
        !j["fetcher_id"].is_string() ||
        !j["status"].is_string() ||
        !j["issued_at"].is_string() ||
        !j["expires_at"].is_string()) {
        return Error::invalid("malformed json: field has wrong type");
    }
    if (!j["purposes"].is_array()) {
        return Error::invalid("malformed json: purposes is not an array");
    }

    ConsentArtefact a;
    a.artefact_id  = j["artefact_id"].get<std::string>();
    a.patient      = PatientId{j["patient"].get<std::string>()};
    a.requester_id = j["requester_id"].get<std::string>();
    a.fetcher_id   = j["fetcher_id"].get<std::string>();

    for (const auto& pj : j["purposes"]) {
        if (!pj.is_string()) {
            return Error::invalid("malformed json: purpose is not a string");
        }
        auto pr = purpose_from_string(pj.get<std::string>());
        if (!pr) {
            return Error::invalid(std::string{"unknown purpose: "} +
                                  pj.get<std::string>());
        }
        a.purposes.push_back(pr.value());
    }

    try {
        a.issued_at  = Time::from_iso8601(j["issued_at"].get<std::string>());
        a.expires_at = Time::from_iso8601(j["expires_at"].get<std::string>());
    } catch (const std::exception& e) {
        return Error::invalid(std::string{"malformed iso8601: "} + e.what());
    }

    auto sr = artefact_status_from_string(j["status"].get<std::string>());
    if (!sr) {
        return Error::invalid(std::string{"unknown artefact status: "} +
                              j["status"].get<std::string>());
    }
    a.status = sr.value();

    if (j.contains("schema_version") && j["schema_version"].is_string()) {
        a.schema_version = j["schema_version"].get<std::string>();
    } else {
        a.schema_version = "1.0";
    }

    return a;
}

// ---- Token <-> Artefact -------------------------------------------------

ConsentArtefact artefact_from_token(const ConsentToken& token,
                                    std::string         requester_id,
                                    std::string         fetcher_id,
                                    std::string         artefact_id) {
    ConsentArtefact a;
    a.artefact_id    = std::move(artefact_id);
    a.patient        = token.patient;
    a.requester_id   = std::move(requester_id);
    a.fetcher_id     = std::move(fetcher_id);
    a.purposes       = token.purposes;
    a.issued_at      = token.issued_at;
    a.expires_at     = token.expires_at;
    a.schema_version = "1.0";

    if (token.revoked) {
        a.status = ConsentArtefact::Status::revoked;
    } else if (Time::now() >= token.expires_at) {
        a.status = ConsentArtefact::Status::expired;
    } else {
        a.status = ConsentArtefact::Status::granted;
    }
    return a;
}

ConsentToken token_from_artefact(const ConsentArtefact& artefact) {
    ConsentToken t;
    t.token_id   = artefact.artefact_id;
    t.patient    = artefact.patient;
    t.purposes   = artefact.purposes;
    t.issued_at  = artefact.issued_at;
    t.expires_at = artefact.expires_at;
    t.revoked    = (artefact.status == ConsentArtefact::Status::revoked);
    return t;
}

}  // namespace asclepius
