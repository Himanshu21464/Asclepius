// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
//
// Round 93 — CohortLedger: longitudinal observation chain.
//
// Append-only; the kernel does not mutate observations. Storage is a
// plain vector — for the cohorts the products in our list operate over
// (single-patient longitudinal, small-cohort outcome tracking) this is
// the right complexity. Callers needing massive cohort scale should
// shard at the operator level, not in the kernel.

#include "asclepius/telemetry.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_set>

namespace asclepius {

void CohortLedger::append(Observation obs) {
    std::lock_guard<std::mutex> lk(mu_);
    observations_.push_back(std::move(obs));
}

void CohortLedger::append_n(std::span<const Observation> obs) {
    if (obs.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    observations_.insert(observations_.end(), obs.begin(), obs.end());
}

std::size_t CohortLedger::total_observations() const noexcept {
    try {
        std::lock_guard<std::mutex> lk(mu_);
        return observations_.size();
    } catch (...) {
        return 0;
    }
}

std::size_t CohortLedger::patient_count() const noexcept {
    try {
        std::lock_guard<std::mutex> lk(mu_);
        std::unordered_set<std::string> seen;
        for (const auto& o : observations_) seen.insert(std::string{o.patient.str()});
        return seen.size();
    } catch (...) {
        return 0;
    }
}

std::size_t CohortLedger::distinct_metrics() const noexcept {
    try {
        std::lock_guard<std::mutex> lk(mu_);
        std::unordered_set<std::string> seen;
        for (const auto& o : observations_) seen.insert(o.metric);
        return seen.size();
    } catch (...) {
        return 0;
    }
}

std::vector<CohortLedger::Observation>
CohortLedger::for_patient(const PatientId& p) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<Observation> out;
    for (const auto& o : observations_) {
        if (o.patient == p) out.push_back(o);
    }
    std::sort(out.begin(), out.end(),
              [](const Observation& a, const Observation& b) {
                  return a.observed_at < b.observed_at;
              });
    return out;
}

std::vector<CohortLedger::Observation>
CohortLedger::for_metric(std::string_view metric) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<Observation> out;
    for (const auto& o : observations_) {
        if (o.metric == metric) out.push_back(o);
    }
    std::sort(out.begin(), out.end(),
              [](const Observation& a, const Observation& b) {
                  return a.observed_at < b.observed_at;
              });
    return out;
}

std::vector<CohortLedger::Observation>
CohortLedger::in_window(Time start, Time end) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<Observation> out;
    for (const auto& o : observations_) {
        if (o.observed_at >= start && o.observed_at < end) out.push_back(o);
    }
    std::sort(out.begin(), out.end(),
              [](const Observation& a, const Observation& b) {
                  return a.observed_at < b.observed_at;
              });
    return out;
}

Result<CohortLedger::Observation>
CohortLedger::latest(const PatientId& p, std::string_view metric) const {
    std::lock_guard<std::mutex> lk(mu_);
    const Observation* best = nullptr;
    for (const auto& o : observations_) {
        if (o.patient == p && o.metric == metric) {
            if (best == nullptr || o.observed_at > best->observed_at) {
                best = &o;
            }
        }
    }
    if (best == nullptr) return Error::not_found("no observation for patient/metric");
    return *best;
}

namespace {

CohortLedger::AggregateStats stats_from_values(const std::vector<double>& values) {
    CohortLedger::AggregateStats s{values.size(), 0.0, 0.0, 0.0, 0.0};
    if (values.empty()) return s;
    double sum = 0.0;
    s.min_value = values.front();
    s.max_value = values.front();
    for (double v : values) {
        sum += v;
        if (v < s.min_value) s.min_value = v;
        if (v > s.max_value) s.max_value = v;
    }
    s.mean = sum / static_cast<double>(values.size());
    if (values.size() > 1) {
        double sq = 0.0;
        for (double v : values) sq += (v - s.mean) * (v - s.mean);
        s.stddev = std::sqrt(sq / static_cast<double>(values.size()));
    }
    return s;
}

}  // namespace

Result<CohortLedger::AggregateStats>
CohortLedger::stats_for_metric(std::string_view metric) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<double> values;
    for (const auto& o : observations_) {
        if (o.metric == metric) values.push_back(o.value);
    }
    if (values.empty()) return Error::not_found("metric has no observations");
    return stats_from_values(values);
}

Result<CohortLedger::AggregateStats>
CohortLedger::stats_for_patient_metric(const PatientId& p,
                                       std::string_view metric) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<double> values;
    for (const auto& o : observations_) {
        if (o.patient == p && o.metric == metric) values.push_back(o.value);
    }
    if (values.empty()) return Error::not_found("no observation for patient/metric");
    return stats_from_values(values);
}

std::vector<std::string> CohortLedger::metrics() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::set<std::string> seen;
    for (const auto& o : observations_) seen.insert(o.metric);
    return std::vector<std::string>{seen.begin(), seen.end()};
}

std::vector<PatientId> CohortLedger::patients() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::set<std::string> seen;
    for (const auto& o : observations_) seen.insert(std::string{o.patient.str()});
    std::vector<PatientId> out;
    out.reserve(seen.size());
    for (const auto& s : seen) out.emplace_back(PatientId{s});
    return out;
}

std::vector<CohortLedger::Observation> CohortLedger::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    auto out = observations_;
    std::sort(out.begin(), out.end(),
              [](const Observation& a, const Observation& b) {
                  if (a.observed_at != b.observed_at) {
                      return a.observed_at < b.observed_at;
                  }
                  return a.patient.str() < b.patient.str();
              });
    return out;
}

void CohortLedger::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    observations_.clear();
}

}  // namespace asclepius
