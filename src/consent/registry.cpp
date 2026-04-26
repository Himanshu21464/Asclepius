// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include "asclepius/consent.hpp"

#include <fmt/core.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <unordered_set>

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

std::vector<ConsentToken>
ConsentRegistry::active_tokens_for_patient(const PatientId& patient) const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto now = Time::now();
    std::vector<ConsentToken> out;
    for (const auto& [_, t] : by_id_) {
        if (t.revoked)            continue;
        if (t.expires_at <= now)  continue;
        if (t.patient != patient) continue;
        out.push_back(t);
    }
    return out;
}

std::vector<ConsentToken>
ConsentRegistry::tokens_for_purpose(Purpose purpose) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<ConsentToken> out;
    for (const auto& [_, t] : by_id_) {
        if (std::find(t.purposes.begin(), t.purposes.end(), purpose) != t.purposes.end()) {
            out.push_back(t);
        }
    }
    return out;
}

Result<ConsentToken> ConsentRegistry::longest_active() const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto now = Time::now();
    const ConsentToken* best = nullptr;
    for (const auto& [_, t] : by_id_) {
        if (t.revoked)           continue;
        if (t.expires_at <= now) continue;
        if (best == nullptr || t.expires_at > best->expires_at) {
            best = &t;
        }
    }
    if (best == nullptr) {
        return Error::not_found("no active consent tokens");
    }
    return *best;
}

Result<ConsentToken> ConsentRegistry::oldest_active() const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto now = Time::now();
    const ConsentToken* best = nullptr;
    for (const auto& [_, t] : by_id_) {
        if (t.revoked)           continue;
        if (t.expires_at <= now) continue;
        if (best == nullptr || t.issued_at < best->issued_at) {
            best = &t;
        }
    }
    if (best == nullptr) {
        return Error::not_found("no active consent tokens");
    }
    return *best;
}

Result<ConsentToken> ConsentRegistry::soonest_to_expire() const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto now = Time::now();
    const ConsentToken* best = nullptr;
    for (const auto& [_, t] : by_id_) {
        if (t.revoked)           continue;
        if (t.expires_at <= now) continue;
        if (best == nullptr || t.expires_at < best->expires_at) {
            best = &t;
        }
    }
    if (best == nullptr) {
        return Error::not_found("no active consent tokens");
    }
    return *best;
}

bool ConsentRegistry::has_purpose_for_patient(const PatientId& patient,
                                              Purpose          purpose) const noexcept {
    try {
        std::lock_guard<std::mutex> lk(mu_);
        const auto now = Time::now();
        for (const auto& [_, t] : by_id_) {
            if (t.revoked)            continue;
            if (t.expires_at <= now)  continue;
            if (t.patient != patient) continue;
            if (std::find(t.purposes.begin(), t.purposes.end(), purpose)
                != t.purposes.end()) {
                return true;
            }
        }
        return false;
    } catch (...) {
        return false;
    }
}

void ConsentRegistry::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    by_id_.clear();
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

std::size_t ConsentRegistry::patient_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::unordered_set<std::string> seen;
    seen.reserve(by_id_.size());
    for (const auto& [_, t] : by_id_) {
        seen.insert(std::string{t.patient.str()});
    }
    return seen.size();
}

std::vector<PatientId> ConsentRegistry::patients() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::unordered_set<std::string> seen;
    seen.reserve(by_id_.size());
    for (const auto& [_, t] : by_id_) {
        seen.insert(std::string{t.patient.str()});
    }
    std::vector<PatientId> out;
    out.reserve(seen.size());
    for (auto& s : seen) {
        out.emplace_back(std::move(s));
    }
    std::sort(out.begin(), out.end(),
              [](const PatientId& a, const PatientId& b) {
                  return a.str() < b.str();
              });
    return out;
}

ConsentRegistry::Summary ConsentRegistry::summary() const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto now = Time::now();
    Summary s{0, 0, 0, 0, 0};
    std::unordered_set<std::string> seen;
    seen.reserve(by_id_.size());
    for (const auto& [_, t] : by_id_) {
        s.total++;
        if (t.revoked) {
            s.revoked++;
        } else if (t.expires_at <= now) {
            s.expired++;
        } else {
            s.active++;
        }
        seen.insert(std::string{t.patient.str()});
    }
    s.patients = seen.size();
    return s;
}

std::vector<ConsentToken>
ConsentRegistry::tokens_expiring_within(std::chrono::seconds horizon) const {
    std::vector<ConsentToken> out;
    if (horizon.count() <= 0) {
        return out;
    }
    std::lock_guard<std::mutex> lk(mu_);
    const auto now      = Time::now();
    const auto deadline = now + std::chrono::nanoseconds{horizon};
    for (const auto& [_, t] : by_id_) {
        if (t.revoked)            continue;
        if (t.expires_at <= now)  continue;
        if (t.expires_at <= deadline) {
            out.push_back(t);
        }
    }
    return out;
}

std::size_t ConsentRegistry::expire_all_for_patient(const PatientId& patient) {
    std::vector<ConsentToken> revoked_now;
    Observer obs_copy;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [_, t] : by_id_) {
            if (t.patient != patient || t.revoked) continue;
            t.revoked = true;
            revoked_now.push_back(t);
        }
        obs_copy = observer_;
    }
    if (obs_copy) {
        for (const auto& t : revoked_now) obs_copy(Event::revoked, t);
    }
    return revoked_now.size();
}

bool ConsentRegistry::token_exists(std::string_view token_id) const noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    return by_id_.find(std::string{token_id}) != by_id_.end();
}

std::size_t ConsentRegistry::expired_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto now = Time::now();
    std::size_t n = 0;
    for (const auto& [_, t] : by_id_) {
        if (!t.revoked && t.expires_at <= now) n++;
    }
    return n;
}

std::size_t ConsentRegistry::cleanup_expired() {
    std::vector<ConsentToken> revoked_now;
    Observer obs_copy;
    {
        std::lock_guard<std::mutex> lk(mu_);
        const auto now = Time::now();
        for (auto& [_, t] : by_id_) {
            if (t.revoked || t.expires_at > now) continue;
            t.revoked = true;
            revoked_now.push_back(t);
        }
        obs_copy = observer_;
    }
    if (obs_copy) {
        for (const auto& t : revoked_now) obs_copy(Event::revoked, t);
    }
    return revoked_now.size();
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

std::vector<Purpose>
ConsentRegistry::active_purposes_for_patient(const PatientId& patient) const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto now = Time::now();
    std::unordered_set<std::uint8_t> seen;
    std::vector<Purpose> out;
    for (const auto& [_, t] : by_id_) {
        if (t.revoked)            continue;
        if (t.expires_at <= now)  continue;
        if (t.patient != patient) continue;
        for (auto p : t.purposes) {
            auto code = static_cast<std::uint8_t>(p);
            if (seen.insert(code).second) {
                out.push_back(p);
            }
        }
    }
    std::sort(out.begin(), out.end(),
              [](Purpose a, Purpose b) {
                  return static_cast<std::uint8_t>(a) <
                         static_cast<std::uint8_t>(b);
              });
    return out;
}

Result<ConsentToken> ConsentRegistry::extend_to(std::string_view token_id,
                                                Time absolute_expires_at) {
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
        if (absolute_expires_at < it->second.expires_at) {
            return Error::invalid(
                "extend_to deadline is earlier than existing expiry");
        }
        it->second.expires_at = absolute_expires_at;
        snapshot  = it->second;
        obs_copy  = observer_;
    }
    if (obs_copy) obs_copy(Event::granted, snapshot);
    return snapshot;
}

std::vector<ConsentToken>
ConsentRegistry::expired_for_patient(const PatientId& patient) const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto now = Time::now();
    std::vector<ConsentToken> out;
    for (const auto& [_, t] : by_id_) {
        if (t.revoked)            continue;
        if (t.patient != patient) continue;
        if (t.expires_at <= now) {
            out.push_back(t);
        }
    }
    return out;
}

std::string ConsentRegistry::dump_state_json() const {
    std::lock_guard<std::mutex> lk(mu_);
    nlohmann::json tokens = nlohmann::json::array();
    for (const auto& [_, t] : by_id_) {
        nlohmann::json purposes = nlohmann::json::array();
        for (auto p : t.purposes) {
            purposes.push_back(to_string(p));
        }
        tokens.push_back({
            {"token_id",   t.token_id},
            {"patient",    std::string{t.patient.str()}},
            {"purposes",   std::move(purposes)},
            {"issued_at",  t.issued_at.iso8601()},
            {"expires_at", t.expires_at.iso8601()},
            {"revoked",    t.revoked},
        });
    }
    nlohmann::json out;
    out["tokens"] = std::move(tokens);
    return out.dump();
}

ConsentRegistry::Summary
ConsentRegistry::stats_for_patient(const PatientId& patient) const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto now = Time::now();
    Summary s{0, 0, 0, 0, 0};
    for (const auto& [_, t] : by_id_) {
        if (t.patient != patient) continue;
        s.total++;
        if (t.revoked) {
            s.revoked++;
        } else if (t.expires_at <= now) {
            s.expired++;
        } else {
            s.active++;
        }
    }
    s.patients = (s.total > 0) ? 1 : 0;
    return s;
}

bool ConsentRegistry::is_patient_known(const PatientId& patient) const noexcept {
    try {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& [_, t] : by_id_) {
            if (t.patient == patient) return true;
        }
        return false;
    } catch (...) {
        return false;
    }
}

std::vector<ConsentToken>
ConsentRegistry::recently_revoked(std::chrono::seconds window) const {
    std::vector<ConsentToken> out;
    if (window.count() <= 0) {
        return out;
    }
    std::lock_guard<std::mutex> lk(mu_);
    const auto now    = Time::now();
    const auto cutoff = now - std::chrono::nanoseconds{window};
    for (const auto& [_, t] : by_id_) {
        if (!t.revoked)            continue;
        if (t.issued_at < cutoff)  continue;
        out.push_back(t);
    }
    std::sort(out.begin(), out.end(),
              [](const ConsentToken& a, const ConsentToken& b) {
                  return a.issued_at > b.issued_at;
              });
    return out;
}

Result<ConsentToken> ConsentRegistry::most_recently_granted() const {
    std::lock_guard<std::mutex> lk(mu_);
    const ConsentToken* best = nullptr;
    for (const auto& [_, t] : by_id_) {
        if (best == nullptr || t.issued_at > best->issued_at) {
            best = &t;
        }
    }
    if (best == nullptr) {
        return Error::not_found("registry is empty");
    }
    return *best;
}

std::size_t ConsentRegistry::extend_all_for_patient(const PatientId&     patient,
                                                    std::chrono::seconds additional_ttl) {
    if (additional_ttl.count() <= 0) {
        return 0;
    }
    std::vector<ConsentToken> extended;
    Observer obs_copy;
    {
        std::lock_guard<std::mutex> lk(mu_);
        const auto now = Time::now();
        for (auto& [_, t] : by_id_) {
            if (t.revoked)            continue;
            if (t.expires_at <= now)  continue;
            if (t.patient != patient) continue;
            t.expires_at = t.expires_at +
                std::chrono::nanoseconds{additional_ttl};
            extended.push_back(t);
        }
        obs_copy = observer_;
    }
    if (obs_copy) {
        for (const auto& t : extended) obs_copy(Event::granted, t);
    }
    return extended.size();
}

}  // namespace asclepius
