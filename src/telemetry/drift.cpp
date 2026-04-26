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

double Histogram::percentile(double p) const {
    // p in [0, 100]; quantile() expects [0, 1] and clamps internally,
    // but we clamp here too so the conversion is well-defined for any
    // input (e.g. negative or > 100).
    const double pc = std::clamp(p, 0.0, 100.0);
    return quantile(pc / 100.0);
}

std::vector<double> Histogram::cdf() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<double> out(counts_.size(), 0.0);
    if (total_ == 0) return out;
    const double t = static_cast<double>(total_);
    std::uint64_t cum = 0;
    for (std::size_t i = 0; i < counts_.size(); ++i) {
        cum += counts_[i];
        out[i] = static_cast<double>(cum) / t;
    }
    return out;
}

double Histogram::mean() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (total_ == 0) return 0.0;
    const auto   n     = counts_.size();
    const double bin_w = (hi_ - lo_) / static_cast<double>(n);
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double mid = lo_ + bin_w * (static_cast<double>(i) + 0.5);
        sum += mid * static_cast<double>(counts_[i]);
    }
    return sum / static_cast<double>(total_);
}

double Histogram::stddev() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (total_ == 0 || total_ == 1) return 0.0;
    const auto   n     = counts_.size();
    const double bin_w = (hi_ - lo_) / static_cast<double>(n);
    // Compute mean inline to avoid double-locking mu_.
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double mid = lo_ + bin_w * (static_cast<double>(i) + 0.5);
        sum += mid * static_cast<double>(counts_[i]);
    }
    const double m = sum / static_cast<double>(total_);
    double var = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double mid = lo_ + bin_w * (static_cast<double>(i) + 0.5);
        const double d   = mid - m;
        var += d * d * static_cast<double>(counts_[i]);
    }
    var /= static_cast<double>(total_);
    return std::sqrt(var);
}

double Histogram::variance() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (total_ == 0 || total_ == 1) return 0.0;
    const auto   n     = counts_.size();
    const double bin_w = (hi_ - lo_) / static_cast<double>(n);
    // Compute mean inline to avoid double-locking mu_.
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double mid = lo_ + bin_w * (static_cast<double>(i) + 0.5);
        sum += mid * static_cast<double>(counts_[i]);
    }
    const double m = sum / static_cast<double>(total_);
    double var = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double mid = lo_ + bin_w * (static_cast<double>(i) + 0.5);
        const double d   = mid - m;
        var += d * d * static_cast<double>(counts_[i]);
    }
    return var / static_cast<double>(total_);
}

double Histogram::iqr() const {
    // quantile() takes its own lock; check emptiness first under our own
    // lock to avoid invoking quantile() on an empty histogram (which would
    // return 0.0 - 0.0 == 0.0 anyway, but the explicit check is cheaper
    // and matches the documented contract).
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (total_ == 0) return 0.0;
    }
    return quantile(0.75) - quantile(0.25);
}

double Histogram::skewness() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (total_ < 2) return 0.0;
    const auto   n     = counts_.size();
    const double bin_w = (hi_ - lo_) / static_cast<double>(n);
    // Compute mean inline.
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double mid = lo_ + bin_w * (static_cast<double>(i) + 0.5);
        sum += mid * static_cast<double>(counts_[i]);
    }
    const double m = sum / static_cast<double>(total_);
    // Variance and 3rd central moment in one pass.
    double m2 = 0.0;
    double m3 = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double mid = lo_ + bin_w * (static_cast<double>(i) + 0.5);
        const double d   = mid - m;
        const double w   = static_cast<double>(counts_[i]);
        m2 += d * d * w;
        m3 += d * d * d * w;
    }
    const double t = static_cast<double>(total_);
    const double var = m2 / t;
    if (var <= 0.0) return 0.0;  // stddev == 0 short-circuit
    const double sd  = std::sqrt(var);
    return (m3 / t) / (sd * sd * sd);
}

double Histogram::min() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (total_ == 0) return lo_;
    const auto   n     = counts_.size();
    const double bin_w = (hi_ - lo_) / static_cast<double>(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (counts_[i] > 0) {
            return lo_ + bin_w * (static_cast<double>(i) + 0.5);
        }
    }
    return lo_;
}

double Histogram::max() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (total_ == 0) return hi_;
    const auto   n     = counts_.size();
    const double bin_w = (hi_ - lo_) / static_cast<double>(n);
    for (std::size_t i = n; i-- > 0;) {
        if (counts_[i] > 0) {
            return lo_ + bin_w * (static_cast<double>(i) + 0.5);
        }
    }
    return hi_;
}

double Histogram::sum() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (total_ == 0) return 0.0;
    const auto   n     = counts_.size();
    const double bin_w = (hi_ - lo_) / static_cast<double>(n);
    double s = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double mid = lo_ + bin_w * (static_cast<double>(i) + 0.5);
        s += mid * static_cast<double>(counts_[i]);
    }
    return s;
}

double Histogram::range() const {
    // min() and max() each take their own lock. Check emptiness up
    // front so we return the documented 0.0 sentinel (not hi - lo).
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (total_ == 0) return 0.0;
    }
    return max() - min();
}

Result<void> Histogram::merge(const Histogram& other) {
    if (this == &other) {
        // Self-merge: lock once and double our own counts.
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& c : counts_) c += c;
        total_ += total_;
        return Result<void>::ok();
    }
    std::lock(mu_, other.mu_);
    std::lock_guard<std::mutex> lk_self (mu_,       std::adopt_lock);
    std::lock_guard<std::mutex> lk_other(other.mu_, std::adopt_lock);
    if (counts_.size() != other.counts_.size()
     || lo_ != other.lo_
     || hi_ != other.hi_) {
        return Error::invalid("Histogram::merge: bins/lo/hi mismatch");
    }
    for (std::size_t i = 0; i < counts_.size(); ++i) {
        counts_[i] += other.counts_[i];
    }
    total_ += other.total_;
    return Result<void>::ok();
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

bool DriftMonitor::has_feature(std::string_view name) const noexcept {
    try {
        std::lock_guard<std::mutex> lk(mu_);
        return features_.find(std::string{name}) != features_.end();
    } catch (...) {
        // Locking or string construction can theoretically throw. The
        // contract is noexcept; degrade to "not present" rather than
        // propagate.
        return false;
    }
}

Result<std::uint64_t> DriftMonitor::observation_count(std::string_view feature) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = features_.find(std::string{feature});
    if (it == features_.end()) {
        return Error::not_found(fmt::format("unregistered feature: {}", feature));
    }
    return it->second->current->total();
}

DriftMonitor::Summary DriftMonitor::summary() const {
    std::lock_guard<std::mutex> lk(mu_);
    Summary s;
    s.feature_count       = features_.size();
    s.total_observations  = 0;
    s.max_severity        = DriftSeverity::none;
    for (const auto& [_, fs] : features_) {
        s.total_observations += fs->current->total();
        const double psi = Histogram::psi(*fs->reference, *fs->current);
        const auto   sev = classify(psi);
        if (static_cast<int>(sev) > static_cast<int>(s.max_severity)) {
            s.max_severity = sev;
        }
    }
    return s;
}

bool DriftMonitor::any_severe() const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& [_, fs] : features_) {
        const double psi = Histogram::psi(*fs->reference, *fs->current);
        if (classify(psi) == DriftSeverity::severe) {
            return true;
        }
    }
    return false;
}

Result<std::uint64_t> DriftMonitor::baseline_count(std::string_view feature) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = features_.find(std::string{feature});
    if (it == features_.end()) {
        return Error::not_found(fmt::format("unregistered feature: {}", feature));
    }
    return it->second->reference->total();
}

Result<std::string> DriftMonitor::most_drifted_feature() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (features_.empty()) {
        return Error::not_found("no features registered");
    }
    const std::string* best_name = nullptr;
    double             best_psi  = 0.0;
    for (const auto& [name, fs] : features_) {
        const double psi = Histogram::psi(*fs->reference, *fs->current);
        if (best_name == nullptr
         || psi > best_psi
         || (psi == best_psi && name < *best_name)) {
            best_name = &name;
            best_psi  = psi;
        }
    }
    return *best_name;
}

}  // namespace asclepius
