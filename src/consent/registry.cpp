// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include "asclepius/consent.hpp"

#include <fmt/core.h>

#include <algorithm>
#include <atomic>
#include <chrono>

namespace asclepius {

const char* to_string(Purpose p) noexcept {
    switch (p) {
        case Purpose::ambient_documentation: return "ambient_documentation";
        case Purpose::diagnostic_suggestion: return "diagnostic_suggestion";
        case Purpose::triage:                return "triage";
        case Purpose::medication_review:     return "medication_review";
        case Purpose::risk_stratification:   return "risk_stratification";
        case Purpose::quality_improvement:   return "quality_improvement";
        case Purpose::research:              return "research";
        case Purpose::operations:            return "operations";
    }
    return "unknown";
}

Result<Purpose> purpose_from_string(std::string_view s) noexcept {
    if (s == "ambient_documentation") return Purpose::ambient_documentation;
    if (s == "diagnostic_suggestion") return Purpose::diagnostic_suggestion;
    if (s == "triage")                return Purpose::triage;
    if (s == "medication_review")     return Purpose::medication_review;
    if (s == "risk_stratification")   return Purpose::risk_stratification;
    if (s == "quality_improvement")   return Purpose::quality_improvement;
    if (s == "research")              return Purpose::research;
    if (s == "operations")            return Purpose::operations;
    return Error::invalid("unknown purpose");
}

namespace {

std::string fresh_token_id() {
    static std::atomic<std::uint64_t> counter{0};
    auto                              n = counter.fetch_add(1);
    auto t = std::chrono::duration_cast<std::chrono::nanoseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
                 .count();
    return fmt::format("ct_{:016x}_{:08x}", static_cast<std::uint64_t>(t), n);
}

}  // namespace

Result<ConsentToken> ConsentRegistry::grant(PatientId            patient,
                                            std::vector<Purpose> purposes,
                                            std::chrono::seconds ttl) {
    if (purposes.empty()) {
        return Error::invalid("consent grant requires at least one purpose");
    }
    ConsentToken t;
    t.token_id   = fresh_token_id();
    t.patient    = std::move(patient);
    t.purposes   = std::move(purposes);
    t.issued_at  = Time::now();
    t.expires_at = t.issued_at + std::chrono::nanoseconds{ttl};

    ConsentToken snapshot;
    Observer     obs_copy;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto [it, inserted] = by_id_.emplace(t.token_id, std::move(t));
        if (!inserted) {
            return Error::internal("token id collision");
        }
        snapshot = it->second;
        obs_copy = observer_;
    }
    if (obs_copy) obs_copy(Event::granted, snapshot);
    return snapshot;
}

Result<void> ConsentRegistry::revoke(std::string_view token_id) {
    ConsentToken snapshot;
    Observer     obs_copy;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = by_id_.find(std::string{token_id});
        if (it == by_id_.end()) {
            return Error::not_found("consent token not found");
        }
        it->second.revoked = true;
        snapshot = it->second;
        obs_copy = observer_;
    }
    if (obs_copy) obs_copy(Event::revoked, snapshot);
    return Result<void>::ok();
}

void ConsentRegistry::set_observer(Observer obs) {
    std::lock_guard<std::mutex> lk(mu_);
    observer_ = std::move(obs);
}

Result<void> ConsentRegistry::ingest(ConsentToken token) {
    std::lock_guard<std::mutex> lk(mu_);
    auto [it, inserted] = by_id_.emplace(token.token_id, std::move(token));
    if (!inserted) {
        return Error{ErrorCode::conflict, "consent token already present"};
    }
    return Result<void>::ok();
}

Result<bool> ConsentRegistry::permits(const PatientId& patient, Purpose purpose) const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto now = Time::now();
    for (const auto& [_, t] : by_id_) {
        if (t.revoked)            continue;
        if (t.expires_at <= now)  continue;
        if (t.patient != patient) continue;
        if (std::find(t.purposes.begin(), t.purposes.end(), purpose) != t.purposes.end()) {
            return true;
        }
    }
    return false;
}

Result<ConsentToken> ConsentRegistry::get(std::string_view token_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = by_id_.find(std::string{token_id});
    if (it == by_id_.end()) {
        return Error::not_found("consent token not found");
    }
    return it->second;
}

std::vector<ConsentToken> ConsentRegistry::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<ConsentToken> out;
    out.reserve(by_id_.size());
    for (const auto& [_, t] : by_id_) out.push_back(t);
    return out;
}

std::vector<ConsentToken>
ConsentRegistry::tokens_for_patient(const PatientId& patient) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<ConsentToken> out;
    for (const auto& [_, t] : by_id_) {
        if (t.patient == patient) out.push_back(t);
    }
    return out;
}

std::size_t ConsentRegistry::active_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto now = Time::now();
    std::size_t n = 0;
    for (const auto& [_, t] : by_id_) {
        if (!t.revoked && t.expires_at > now) n++;
    }
    return n;
}

std::size_t ConsentRegistry::total_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return by_id_.size();
}

Result<ConsentToken> ConsentRegistry::extend(std::string_view     token_id,
                                             std::chrono::seconds additional_ttl) {
    if (additional_ttl.count() <= 0) {
        return Error::invalid("extend requires positive additional_ttl");
    }
    ConsentToken snapshot;
    Observer     obs_copy;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = by_id_.find(std::string{token_id});
        if (it == by_id_.end()) {
            return Error::not_found("consent token not found");
        }
        if (it->second.revoked) {
            return Error::denied("cannot extend a revoked token");
        }
        it->second.expires_at = it->second.expires_at +
            std::chrono::nanoseconds{additional_ttl};
        snapshot  = it->second;
        obs_copy  = observer_;
    }
    if (obs_copy) obs_copy(Event::granted, snapshot);
    return snapshot;
}

}  // namespace asclepius
