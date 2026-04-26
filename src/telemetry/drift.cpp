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

double Histogram::quantile(double q) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (total_ == 0) return 0.0;
    const double qc = std::clamp(q, 0.0, 1.0);
    if (qc <= 0.0) return lo_;
    if (qc >= 1.0) return hi_;

    const auto   n        = counts_.size();
    const double bin_w    = (hi_ - lo_) / static_cast<double>(n);
    const double target   = qc * static_cast<double>(total_);

    std::uint64_t cum = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint64_t prev = cum;
        cum += counts_[i];
        if (static_cast<double>(cum) >= target) {
            // Linearly interpolate within bin i: fraction of the bin's mass
            // we need to traverse beyond `prev` to reach `target`.
            const double bin_mass = static_cast<double>(counts_[i]);
            double frac = 0.0;
            if (bin_mass > 0.0) {
                frac = (target - static_cast<double>(prev)) / bin_mass;
                frac = std::clamp(frac, 0.0, 1.0);
            }
            return lo_ + bin_w * (static_cast<double>(i) + frac);
        }
    }
    // Floating-point edge: target rounds just past the last cum.
    return hi_;
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
    DriftReport report_to_emit;
    bool        should_emit = false;
    AlertSink   sink_copy;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = features_.find(std::string{feature});
        if (it == features_.end()) {
            return Error::not_found(std::string{"unregistered feature: "} + std::string{feature});
        }
        it->second->current->observe(value);

        // If an alert sink is registered, evaluate the new severity. We
        // fire only when the severity STRICTLY RISES above the previous
        // recorded severity AND meets the configured threshold. Falling
        // severity does not re-fire (a return-to-normal is a separate kind
        // of event we don't model yet).
        if (alert_sink_) {
            DriftReport r;
            r.feature      = std::string{feature};
            r.psi          = Histogram::psi(*it->second->reference, *it->second->current);
            r.ks_statistic = Histogram::ks (*it->second->reference, *it->second->current);
            r.emd          = Histogram::emd(*it->second->reference, *it->second->current);
            r.severity     = classify(r.psi);
            r.reference_n  = it->second->reference->total();
            r.current_n    = it->second->current->total();
            r.computed_at  = Time::now();

            auto last_it = last_severity_.find(r.feature);
            DriftSeverity prev = (last_it == last_severity_.end()
                                  ? DriftSeverity::none
                                  : last_it->second);
            if (static_cast<int>(r.severity) > static_cast<int>(prev)
             && static_cast<int>(r.severity) >= static_cast<int>(alert_threshold_)) {
                last_severity_[r.feature] = r.severity;
                report_to_emit = r;
                should_emit    = true;
                sink_copy      = alert_sink_;
            } else {
                last_severity_[r.feature] = r.severity;
            }
        }
    }
    // Call the sink without holding mu_ so it can do anything (including
    // appending to a Ledger which has its own mutex).
    if (should_emit) {
        try { sink_copy(report_to_emit); } catch (...) { /* swallow */ }
    }
    return Result<void>::ok();
}

void DriftMonitor::set_alert_sink(AlertSink sink, DriftSeverity threshold) {
    std::lock_guard<std::mutex> lk(mu_);
    alert_sink_      = std::move(sink);
    alert_threshold_ = threshold;
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

Result<void> DriftMonitor::reset(std::string_view feature) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = features_.find(std::string{feature});
    if (it == features_.end()) {
        return Error::not_found("unregistered feature");
    }
    auto& fs = it->second;
    fs->current = std::make_unique<Histogram>(
        fs->current->lo(), fs->current->hi(), fs->current->bin_count());
    last_severity_.erase(std::string{feature});
    return Result<void>::ok();
}

std::vector<std::string> DriftMonitor::list_features() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> out;
    out.reserve(features_.size());
    for (const auto& [name, _] : features_) out.push_back(name);
    return out;
}

std::size_t DriftMonitor::feature_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return features_.size();
}

Result<std::uint64_t> DriftMonitor::observation_count(std::string_view feature) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = features_.find(std::string{feature});
    if (it == features_.end()) {
        return Error::not_found(fmt::format("unregistered feature: {}", feature));
    }
    return it->second->current->total();
}

}  // namespace asclepius
