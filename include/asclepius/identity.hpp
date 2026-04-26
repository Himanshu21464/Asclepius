// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#ifndef ASCLEPIUS_IDENTITY_HPP
#define ASCLEPIUS_IDENTITY_HPP

#include <string>
#include <string_view>

#include "asclepius/core.hpp"

namespace asclepius {

// Strong typedefs for the identifier kinds Asclepius reasons about.
//
// These are intentionally not interconvertible. Mixing a PatientId where an
// ActorId is expected should not compile. Each holds an opaque string body so
// callers can use whatever scheme their EHR or HIS already uses (FHIR
// resource ids, MRNs, pseudonymous tokens, OAuth subject claims).

namespace detail {

template <class Tag>
class StrongId {
public:
    StrongId() = default;
    explicit StrongId(std::string id) : id_(std::move(id)) {}

    std::string_view str() const noexcept { return id_; }
    bool             empty() const noexcept { return id_.empty(); }

    bool operator==(const StrongId& other) const = default;
    auto operator<=>(const StrongId& other) const = default;

protected:
    std::string id_;
};

struct ActorTag     {};
struct PatientTag   {};
struct EncounterTag {};
struct ModelTag     {};
struct TenantTag    {};

}  // namespace detail

// ---- Actor ---------------------------------------------------------------

class ActorId : public detail::StrongId<detail::ActorTag> {
public:
    using StrongId::StrongId;

    static ActorId clinician(std::string id) { return ActorId{"clinician:" + std::move(id)}; }
    static ActorId service  (std::string id) { return ActorId{"service:"   + std::move(id)}; }
    static ActorId system   (std::string id) { return ActorId{"system:"    + std::move(id)}; }
    static ActorId patient  (std::string id) { return ActorId{"patient:"   + std::move(id)}; }
};

// ---- Patient -------------------------------------------------------------

class PatientId : public detail::StrongId<detail::PatientTag> {
public:
    using StrongId::StrongId;

    // Pseudonymous: a token derived from the MRN by a one-way function on
    // the EHR side. Asclepius itself never holds the mapping.
    static PatientId pseudonymous(std::string token) { return PatientId{"pat:" + std::move(token)}; }

    // FHIR Patient/<id> reference.
    static PatientId fhir(std::string id) { return PatientId{"fhir:Patient/" + std::move(id)}; }
};

// ---- Encounter -----------------------------------------------------------

class EncounterId : public detail::StrongId<detail::EncounterTag> {
public:
    using StrongId::StrongId;

    // Generates a fresh time-sortable encounter id (ULID-shaped).
    static EncounterId make();

    static EncounterId fhir(std::string id) { return EncounterId{"fhir:Encounter/" + std::move(id)}; }
};

// ---- Model ---------------------------------------------------------------

class ModelId : public detail::StrongId<detail::ModelTag> {
public:
    using StrongId::StrongId;

    // A composite "<name>@<version>" id. Both fields are required.
    ModelId(std::string name, std::string version)
        : StrongId(std::move(name) + "@" + std::move(version)) {}
};

// ---- Tenant --------------------------------------------------------------

class TenantId : public detail::StrongId<detail::TenantTag> {
public:
    using StrongId::StrongId;
};

}  // namespace asclepius

namespace std {
template <class Tag>
struct hash<asclepius::detail::StrongId<Tag>> {
    std::size_t operator()(const asclepius::detail::StrongId<Tag>& id) const noexcept {
        return std::hash<std::string_view>{}(id.str());
    }
};
template <> struct hash<asclepius::ActorId>     : hash<asclepius::detail::StrongId<asclepius::detail::ActorTag>>     {};
template <> struct hash<asclepius::PatientId>   : hash<asclepius::detail::StrongId<asclepius::detail::PatientTag>>   {};
template <> struct hash<asclepius::EncounterId> : hash<asclepius::detail::StrongId<asclepius::detail::EncounterTag>> {};
template <> struct hash<asclepius::ModelId>     : hash<asclepius::detail::StrongId<asclepius::detail::ModelTag>>     {};
template <> struct hash<asclepius::TenantId>    : hash<asclepius::detail::StrongId<asclepius::detail::TenantTag>>    {};
}  // namespace std

#endif  // ASCLEPIUS_IDENTITY_HPP
