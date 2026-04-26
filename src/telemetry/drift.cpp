// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include "asclepius/telemetry.hpp"

#include <fmt/core.h>

#include <algorithm>
#include <cmath>

namespace asclepius {

const char* to_string(DriftSeverity s) noexcept {
    switch (s) {
        case DriftSeverity::none:   return "none";
        case DriftSeverity::minor:  return "minor";
        case DriftSeverity::moder:  return "moderate";
        case DriftSeverity::severe: return "severe";
    }
    return "unknown";
}

// ---- Histogram -----------------------------------------------------------

Histogram::Histogram(double lo, double hi, std::size_t bins) : lo_(lo), hi_(hi), counts_(bins, 0) {
    if (bins == 0) {
        // Defensive: a zero-bin histogram is useless. Pad to one.
        counts_.assign(1, 0);
    }
    if (!(hi_ > lo_)) {
        hi_ = lo_ + 1.0;
    }
}

void Histogram::observe(double value) {
    std::lock_guard<std::mutex> lk(mu_);
    const auto n  = counts_.size();
    auto       idx = static_cast<std::size_t>(
        std::clamp((value - lo_) / (hi_ - lo_), 0.0, 1.0 - 1e-12) * static_cast<double>(n));
    if (idx >= n) idx = n - 1;
    ++counts_[idx];
    ++total_;
}

std::size_t   Histogram::bin_count() const noexcept { return counts_.size(); }
std::uint64_t Histogram::total()     const noexcept { return total_; }
double        Histogram::lo()        const noexcept { return lo_; }
double        Histogram::hi()        const noexcept { return hi_; }

std::vector<double> Histogram::normalized() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<double> out(counts_.size(), 0.0);
    if (total_ == 0) return out;
    const double t = static_cast<double>(total_);
    for (std::size_t i = 0; i < counts_.size(); ++i) {
        out[i] = static_cast<double>(counts_[i]) / t;
    }
    return out;
}

double Histogram::psi(const Histogram& reference, const Histogram& current) {
    auto p = reference.normalized();
    auto q = current.normalized();
    const auto n = std::min(p.size(), q.size());
    constexpr double eps = 1e-6;
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double pi = std::max(p[i], eps);
        const double qi = std::max(q[i], eps);
        sum += (pi - qi) * std::log(pi / qi);
    }
    return sum;
}

double Histogram::ks(const Histogram& reference, const Histogram& current) {
    auto p = reference.normalized();
    auto q = current.normalized();
    const auto n = std::min(p.size(), q.size());
    double cp = 0.0, cq = 0.0, max_d = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        cp += p[i];
        cq += q[i];
        max_d = std::max(max_d, std::abs(cp - cq));
    }
    return max_d;
}

double Histogram::emd(const Histogram& reference, const Histogram& current) {
    auto p = reference.normalized();
    auto q = current.normalized();
    const auto n = std::min(p.size(), q.size());
    double work = 0.0;
    double flow = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        flow += p[i] - q[i];
        work += std::abs(flow);
    }
    return work;
}

// ---- DriftMonitor --------------------------------------------------------

struct DriftMonitor::FeatureState {
    std::unique_ptr<Histogram> reference;
    std::unique_ptr<Histogram> current;
};

DriftMonitor::DriftMonitor()  = default;
DriftMonitor::~DriftMonitor() = default;

DriftSeverity DriftMonitor::classify(double psi) noexcept {
    if (std::isnan(psi))   return DriftSeverity::severe;
    if (psi < 0.10)        return DriftSeverity::none;
    if (psi < 0.25)        return DriftSeverity::minor;
    if (psi < 0.50)        return DriftSeverity::moder;
    return DriftSeverity::severe;
}

Result<void> DriftMonitor::register_feature(std::string         name,
                                            std::vector<double> baseline,
                                            double              lo,
                                            double              hi,
                                            std::size_t         bins) {
    std::lock_guard<std::mutex> lk(mu_);
    auto fs = std::make_unique<FeatureState>();
    fs->reference = std::make_unique<Histogram>(lo, hi, bins);
    for (auto v : baseline) fs->reference->observe(v);
    fs->current = std::make_unique<Histogram>(lo, hi, bins);
    features_[name] = std::move(fs);
    return Result<void>::ok();
}

Result<void> DriftMonitor::observe(std::string_view feature, double value) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = features_.find(std::string{feature});
    if (it == features_.end()) {
        return Error::not_found(std::string{"unregistered feature: "} + std::string{feature});
    }
    it->second->current->observe(value);
    return Result<void>::ok();
}

std::vector<DriftReport> DriftMonitor::report() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<DriftReport> out;
    out.reserve(features_.size());
    const auto now = Time::now();
    for (const auto& [name, fs] : features_) {
        DriftReport r;
        r.feature      = name;
        r.psi          = Histogram::psi(*fs->reference, *fs->current);
        r.ks_statistic = Histogram::ks (*fs->reference, *fs->current);
        r.emd          = Histogram::emd(*fs->reference, *fs->current);
        r.severity     = classify(r.psi);
        r.reference_n  = fs->reference->total();
        r.current_n    = fs->current->total();
        r.computed_at  = now;
        out.push_back(std::move(r));
    }
    return out;
}

void DriftMonitor::rotate() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [_, fs] : features_) {
        fs->current = std::make_unique<Histogram>(
            fs->current->lo(), fs->current->hi(), fs->current->bin_count());
    }
}

}  // namespace asclepius
