// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
//
// EmergencyOverride — DPDP Act § 7 break-glass primitive.
//
// Records emergency clinical access events alongside a mandatory backfill
// window (default 72 hours). Within that window the operator MUST file an
// evidence_id (signed note, scanned consent, ABDM consent-artefact id —
// the kernel is policy-neutral about the concrete artefact). Past the
// deadline a token surfaces in overdue_backfills() so dashboards and
// audit alerts can flag the breach.
//
// Lock pattern matches ConsentRegistry: state mutations happen under
// `mu_`, the observer fires AFTER the lock is released so callbacks may
// safely re-enter the override.

#include "asclepius/consent.hpp"

#include <fmt/core.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <set>
#include <sstream>
#include <utility>

namespace asclepius {

namespace {

std::string fresh_eo_token_id() {
    static std::atomic<std::uint64_t> counter{0};
    auto                              n = counter.fetch_add(1);
    auto t = std::chrono::duration_cast<std::chrono::nanoseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
                 .count();
    return fmt::format("eo_{:016x}_{:08x}", static_cast<std::uint64_t>(t), n);
}

}  // namespace

EmergencyOverride::EmergencyOverride(std::chrono::seconds backfill_window)
    : window_(backfill_window) {}

Result<EmergencyOverrideToken>
EmergencyOverride::activate(ActorId     actor,
                            PatientId   patient,
                            std::string reason) {
    if (reason.empty()) {
        return Error::invalid("emergency override requires a non-empty reason");
    }

    EmergencyOverrideToken t;
    t.token_id             = fresh_eo_token_id();
    t.actor                = std::move(actor);
    t.patient              = std::move(patient);
    t.reason               = std::move(reason);
    t.activated_at         = Time::now();
    t.backfilled           = false;
    t.backfill_evidence_id = "";

    EmergencyOverrideToken snapshot;
    Observer               obs_copy;
    {
        std::lock_guard<std::mutex> lk(mu_);
        // Compute the deadline under the lock so the active window_ is
        // the one in force at the moment of insertion.
        t.backfill_deadline = t.activated_at +
            std::chrono::nanoseconds{window_};
        auto [it, inserted] = by_id_.emplace(t.token_id, std::move(t));
        if (!inserted) {
            return Error::internal("emergency override token id collision");
        }
        snapshot = it->second;
        obs_copy = observer_;
    }
    if (obs_copy) obs_copy(Event::activated, snapshot);
    return snapshot;
}

Result<void> EmergencyOverride::backfill(std::string_view token_id,
                                         std::string      evidence_id) {
    if (evidence_id.empty()) {
        return Error::invalid("backfill requires a non-empty evidence_id");
    }

    EmergencyOverrideToken snapshot;
    Observer               obs_copy;
    bool                   fire = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = by_id_.find(std::string{token_id});
        if (it == by_id_.end()) {
            return Error::not_found("emergency override token not found");
        }
        if (it->second.backfilled) {
            // Idempotent: same evidence_id is a silent no-op; a different
            // evidence_id is a hard conflict (the operator is trying to
            // overwrite an already-filed justification).
            if (it->second.backfill_evidence_id == evidence_id) {
                return Result<void>::ok();
            }
            return Error{ErrorCode::conflict,
                         "emergency override already backfilled with a "
                         "different evidence_id"};
        }
        it->second.backfilled           = true;
        it->second.backfill_evidence_id = std::move(evidence_id);
        snapshot = it->second;
        obs_copy = observer_;
        fire     = true;
    }
    if (fire && obs_copy) obs_copy(Event::backfilled, snapshot);
    return Result<void>::ok();
}

Result<EmergencyOverrideToken>
EmergencyOverride::get(std::string_view token_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = by_id_.find(std::string{token_id});
    if (it == by_id_.end()) {
        return Error::not_found("emergency override token not found");
    }
    return it->second;
}

bool EmergencyOverride::is_pending_backfill(std::string_view token_id) const noexcept {
    try {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = by_id_.find(std::string{token_id});
        if (it == by_id_.end()) return false;
        return !it->second.backfilled;
    } catch (...) {
        return false;
    }
}

bool EmergencyOverride::is_overdue(std::string_view token_id) const noexcept {
    try {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = by_id_.find(std::string{token_id});
        if (it == by_id_.end()) return false;
        if (it->second.backfilled) return false;
        return Time::now() >= it->second.backfill_deadline;
    } catch (...) {
        return false;
    }
}

std::vector<EmergencyOverrideToken>
EmergencyOverride::pending_backfills() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<EmergencyOverrideToken> out;
    for (const auto& [_, t] : by_id_) {
        if (!t.backfilled) out.push_back(t);
    }
    std::sort(out.begin(), out.end(),
              [](const EmergencyOverrideToken& a,
                 const EmergencyOverrideToken& b) {
                  return a.activated_at < b.activated_at;
              });
    return out;
}

std::vector<EmergencyOverrideToken>
EmergencyOverride::overdue_backfills() const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto now = Time::now();
    std::vector<EmergencyOverrideToken> out;
    for (const auto& [_, t] : by_id_) {
        if (t.backfilled)              continue;
        if (t.backfill_deadline > now) continue;
        out.push_back(t);
    }
    std::sort(out.begin(), out.end(),
              [](const EmergencyOverrideToken& a,
                 const EmergencyOverrideToken& b) {
                  return a.backfill_deadline < b.backfill_deadline;
              });
    return out;
}

std::vector<EmergencyOverrideToken>
EmergencyOverride::completed_backfills() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<EmergencyOverrideToken> out;
    for (const auto& [_, t] : by_id_) {
        if (t.backfilled) out.push_back(t);
    }
    std::sort(out.begin(), out.end(),
              [](const EmergencyOverrideToken& a,
                 const EmergencyOverrideToken& b) {
                  return a.activated_at < b.activated_at;
              });
    return out;
}

std::size_t EmergencyOverride::pending_count() const noexcept {
    try {
        std::lock_guard<std::mutex> lk(mu_);
        std::size_t n = 0;
        for (const auto& [_, t] : by_id_) {
            if (!t.backfilled) n++;
        }
        return n;
    } catch (...) {
        return 0;
    }
}

std::size_t EmergencyOverride::overdue_count() const noexcept {
    try {
        std::lock_guard<std::mutex> lk(mu_);
        const auto now = Time::now();
        std::size_t n = 0;
        for (const auto& [_, t] : by_id_) {
            if (t.backfilled)              continue;
            if (t.backfill_deadline > now) continue;
            n++;
        }
        return n;
    } catch (...) {
        return 0;
    }
}

std::size_t EmergencyOverride::completed_count() const noexcept {
    try {
        std::lock_guard<std::mutex> lk(mu_);
        std::size_t n = 0;
        for (const auto& [_, t] : by_id_) {
            if (t.backfilled) n++;
        }
        return n;
    } catch (...) {
        return 0;
    }
}

std::size_t EmergencyOverride::total_count() const noexcept {
    try {
        std::lock_guard<std::mutex> lk(mu_);
        return by_id_.size();
    } catch (...) {
        return 0;
    }
}

std::vector<PatientId> EmergencyOverride::patients() const {
    // std::set gives sorted-deterministic dedup keyed on the underlying
    // body string; reconstruct PatientIds from the keys for the return.
    std::lock_guard<std::mutex> lk(mu_);
    std::set<std::string> seen;
    for (const auto& [_, t] : by_id_) {
        seen.insert(std::string{t.patient.str()});
    }
    std::vector<PatientId> out;
    out.reserve(seen.size());
    for (const auto& s : seen) {
        out.emplace_back(PatientId{s});
    }
    return out;
}

std::vector<ActorId> EmergencyOverride::actors() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::set<std::string> seen;
    for (const auto& [_, t] : by_id_) {
        seen.insert(std::string{t.actor.str()});
    }
    std::vector<ActorId> out;
    out.reserve(seen.size());
    for (const auto& s : seen) {
        out.emplace_back(ActorId{s});
    }
    return out;
}

void EmergencyOverride::set_backfill_window(std::chrono::seconds w) {
    // New activations only — existing tokens keep the deadline computed
    // at their activation time, so we touch only the cached window_.
    std::lock_guard<std::mutex> lk(mu_);
    window_ = w;
}

std::chrono::seconds EmergencyOverride::backfill_window() const noexcept {
    try {
        std::lock_guard<std::mutex> lk(mu_);
        return window_;
    } catch (...) {
        return std::chrono::seconds{0};
    }
}

Result<void> EmergencyOverride::ingest(EmergencyOverrideToken token) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = by_id_.find(token.token_id);
    if (it != by_id_.end()) {
        // Idempotent replay: identical fields → silent ok. Different fields
        // (anything but pure equality) → conflict. The ledger is the source
        // of truth and we never want to silently overwrite a previously
        // recorded activation/backfill state on replay.
        const auto& cur = it->second;
        const bool same =
            cur.token_id              == token.token_id              &&
            cur.actor                 == token.actor                 &&
            cur.patient               == token.patient               &&
            cur.reason                == token.reason                &&
            cur.activated_at          == token.activated_at          &&
            cur.backfill_deadline     == token.backfill_deadline     &&
            cur.backfilled            == token.backfilled            &&
            cur.backfill_evidence_id  == token.backfill_evidence_id;
        if (same) return Result<void>::ok();
        return Error{ErrorCode::conflict,
                     "emergency override token id collides with a "
                     "different stored token"};
    }
    by_id_.emplace(token.token_id, std::move(token));
    return Result<void>::ok();
}

std::vector<EmergencyOverrideToken> EmergencyOverride::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<EmergencyOverrideToken> out;
    out.reserve(by_id_.size());
    for (const auto& [_, t] : by_id_) out.push_back(t);
    std::sort(out.begin(), out.end(),
              [](const EmergencyOverrideToken& a,
                 const EmergencyOverrideToken& b) {
                  return a.activated_at < b.activated_at;
              });
    return out;
}

void EmergencyOverride::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    by_id_.clear();
}

void EmergencyOverride::set_observer(Observer obs) {
    std::lock_guard<std::mutex> lk(mu_);
    observer_ = std::move(obs);
}

EmergencyOverride::Summary EmergencyOverride::summary() const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto now = Time::now();
    Summary s{0, 0, 0, 0};
    for (const auto& [_, t] : by_id_) {
        s.total++;
        if (t.backfilled) {
            s.completed++;
        } else {
            s.pending++;
            if (t.backfill_deadline <= now) {
                s.overdue++;
            }
        }
    }
    return s;
}

std::string EmergencyOverride::summary_string() const {
    // Reuse summary() so the string and the Summary struct stay in
    // lockstep — one source of truth for the partition counts.
    auto s = summary();
    return fmt::format("total={} pending={} overdue={} completed={}",
                       s.total, s.pending, s.overdue, s.completed);
}

}  // namespace asclepius
