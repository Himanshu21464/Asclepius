// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
//
// FamilyGraph — multi-party consent edges for elder/minor relations.
//
// The graph stores a flat vector of (proxy, subject, relation, recorded_at)
// edges. Each (proxy, subject) pair is unique: re-recording the same
// proxy/subject with the same relation is a conflict, while a different
// relation updates the edge in place. Mirrors the ConsentRegistry's
// "fire observer outside the lock" idiom so observers may call back into
// the graph without deadlocking.

#include "asclepius/consent.hpp"

#include <fmt/core.h>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <utility>

namespace asclepius {

const char* to_string(FamilyRelation r) noexcept {
    switch (r) {
        case FamilyRelation::adult_child_for_elder_parent: return "adult_child_for_elder_parent";
        case FamilyRelation::parent_for_minor:             return "parent_for_minor";
        case FamilyRelation::legal_guardian_for_ward:      return "legal_guardian_for_ward";
        case FamilyRelation::spouse_for_spouse:            return "spouse_for_spouse";
    }
    return "unknown";
}

Result<FamilyRelation> family_relation_from_string(std::string_view s) noexcept {
    if (s == "adult_child_for_elder_parent") return FamilyRelation::adult_child_for_elder_parent;
    if (s == "parent_for_minor")             return FamilyRelation::parent_for_minor;
    if (s == "legal_guardian_for_ward")      return FamilyRelation::legal_guardian_for_ward;
    if (s == "spouse_for_spouse")            return FamilyRelation::spouse_for_spouse;
    return Error::invalid("unknown family relation");
}

namespace {

// Linear scan for an edge with the given (proxy, subject). The graph is
// expected to stay small (caregivers per patient typically single digits)
// so a vector + linear search beats a map's overhead at this size and
// preserves a stable physical ordering for ingest/snapshot round-trips.
auto find_edge(std::vector<FamilyGraph::Edge>& edges,
               const PatientId&                proxy,
               const PatientId&                subject)
    -> std::vector<FamilyGraph::Edge>::iterator {
    return std::find_if(
        edges.begin(), edges.end(),
        [&](const FamilyGraph::Edge& e) {
            return e.proxy == proxy && e.subject == subject;
        });
}

auto find_edge(const std::vector<FamilyGraph::Edge>& edges,
               const PatientId&                       proxy,
               const PatientId&                       subject)
    -> std::vector<FamilyGraph::Edge>::const_iterator {
    return std::find_if(
        edges.begin(), edges.end(),
        [&](const FamilyGraph::Edge& e) {
            return e.proxy == proxy && e.subject == subject;
        });
}

}  // namespace

Result<void> FamilyGraph::record_relation(PatientId      proxy,
                                          PatientId      subject,
                                          FamilyRelation relation) {
    Edge     snapshot;
    Observer obs_copy;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = find_edge(edges_, proxy, subject);
        if (it != edges_.end()) {
            // Same (proxy, subject, relation) is a no-op conflict: we do
            // NOT touch recorded_at and we do NOT fire the observer. A
            // different relation updates the edge in place and fires
            // `recorded`, matching the documented contract.
            if (it->relation == relation) {
                return Error{ErrorCode::conflict, "relation already recorded"};
            }
            it->relation    = relation;
            it->recorded_at = Time::now();
            snapshot        = *it;
        } else {
            Edge e;
            e.proxy       = std::move(proxy);
            e.subject     = std::move(subject);
            e.relation    = relation;
            e.recorded_at = Time::now();
            edges_.push_back(std::move(e));
            snapshot = edges_.back();
        }
        obs_copy = observer_;
    }
    if (obs_copy) obs_copy(Event::recorded, snapshot);
    return Result<void>::ok();
}

Result<void> FamilyGraph::remove_relation(const PatientId& proxy,
                                          const PatientId& subject) {
    Edge     snapshot;
    Observer obs_copy;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = find_edge(edges_, proxy, subject);
        if (it == edges_.end()) {
            return Error::not_found("relation not found");
        }
        snapshot = *it;
        edges_.erase(it);
        obs_copy = observer_;
    }
    if (obs_copy) obs_copy(Event::removed, snapshot);
    return Result<void>::ok();
}

bool FamilyGraph::can_consent_for(const PatientId& proxy,
                                  const PatientId& subject) const noexcept {
    try {
        std::lock_guard<std::mutex> lk(mu_);
        return find_edge(edges_, proxy, subject) != edges_.end();
    } catch (...) {
        return false;
    }
}

std::vector<PatientId>
FamilyGraph::subjects_for_proxy(const PatientId& proxy) const {
    std::lock_guard<std::mutex> lk(mu_);
    // Distinct subjects authorised by `proxy`. Dedup via a string set
    // keyed on patient.str() to mirror the ConsentRegistry::patients()
    // pattern; sort the output by patient.str() ascending so callers can
    // diff snapshots over time.
    std::unordered_set<std::string> seen;
    seen.reserve(edges_.size());
    for (const auto& e : edges_) {
        if (e.proxy != proxy) continue;
        seen.insert(std::string{e.subject.str()});
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

std::vector<PatientId>
FamilyGraph::proxies_for_subject(const PatientId& subject) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::unordered_set<std::string> seen;
    seen.reserve(edges_.size());
    for (const auto& e : edges_) {
        if (e.subject != subject) continue;
        seen.insert(std::string{e.proxy.str()});
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

Result<FamilyRelation>
FamilyGraph::relation_between(const PatientId& proxy,
                              const PatientId& subject) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = find_edge(edges_, proxy, subject);
    if (it == edges_.end()) {
        return Error::not_found("relation not found");
    }
    return it->relation;
}

std::size_t FamilyGraph::total_relations() const noexcept {
    try {
        std::lock_guard<std::mutex> lk(mu_);
        return edges_.size();
    } catch (...) {
        return 0;
    }
}

std::size_t FamilyGraph::distinct_proxies() const noexcept {
    try {
        std::lock_guard<std::mutex> lk(mu_);
        std::unordered_set<std::string> seen;
        seen.reserve(edges_.size());
        for (const auto& e : edges_) {
            seen.insert(std::string{e.proxy.str()});
        }
        return seen.size();
    } catch (...) {
        return 0;
    }
}

std::size_t FamilyGraph::distinct_subjects() const noexcept {
    try {
        std::lock_guard<std::mutex> lk(mu_);
        std::unordered_set<std::string> seen;
        seen.reserve(edges_.size());
        for (const auto& e : edges_) {
            seen.insert(std::string{e.subject.str()});
        }
        return seen.size();
    } catch (...) {
        return 0;
    }
}

std::unordered_map<FamilyRelation, std::size_t>
FamilyGraph::counts_by_relation() const {
    std::lock_guard<std::mutex> lk(mu_);
    // Relations with zero edges do not appear (per the header contract).
    // We accumulate from an empty map so the absence is automatic.
    std::unordered_map<FamilyRelation, std::size_t> out;
    for (const auto& e : edges_) {
        out[e.relation]++;
    }
    return out;
}

std::vector<FamilyGraph::Edge> FamilyGraph::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<Edge> out = edges_;
    std::sort(out.begin(), out.end(),
              [](const Edge& a, const Edge& b) {
                  if (a.proxy.str() != b.proxy.str()) {
                      return a.proxy.str() < b.proxy.str();
                  }
                  return a.subject.str() < b.subject.str();
              });
    return out;
}

Result<void> FamilyGraph::ingest(Edge edge) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = find_edge(edges_, edge.proxy, edge.subject);
    if (it != edges_.end()) {
        // An exact-duplicate (same relation) is a hard conflict; ledger
        // replay should never re-emit the same edge twice. A different
        // relation is silently updated — that case mirrors the
        // record_relation update path, but without the observer side
        // effect (the ledger is already source of truth).
        if (it->relation == edge.relation) {
            return Error{ErrorCode::conflict, "relation already recorded"};
        }
        it->relation    = edge.relation;
        it->recorded_at = edge.recorded_at;
        return Result<void>::ok();
    }
    edges_.push_back(std::move(edge));
    return Result<void>::ok();
}

void FamilyGraph::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    edges_.clear();
}

void FamilyGraph::set_observer(Observer obs) {
    std::lock_guard<std::mutex> lk(mu_);
    observer_ = std::move(obs);
}

std::string FamilyGraph::summary_string() const {
    std::lock_guard<std::mutex> lk(mu_);
    // Single-pass tally so the three counts are self-consistent — calling
    // distinct_proxies / distinct_subjects separately would re-acquire
    // the lock and could race against concurrent mutations.
    std::unordered_set<std::string> proxies;
    std::unordered_set<std::string> subjects;
    proxies.reserve(edges_.size());
    subjects.reserve(edges_.size());
    for (const auto& e : edges_) {
        proxies.insert(std::string{e.proxy.str()});
        subjects.insert(std::string{e.subject.str()});
    }
    return fmt::format("edges={} proxies={} subjects={}",
                       edges_.size(), proxies.size(), subjects.size());
}

}  // namespace asclepius
