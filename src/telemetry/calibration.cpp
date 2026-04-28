// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
//
// Round 92 — CalibrationMonitor: empirical sensitivity / specificity tracking.
//
// Vendor-neutral: the kernel does not know what diagnostic the model is
// performing; it just keeps four counters (TP/FP/TN/FN) and reports
// derived rates against a configured floor. Callers feed Outcome events
// from their own ground-truth pipeline.

#include "asclepius/telemetry.hpp"

#include <fmt/core.h>

#include <cmath>

namespace asclepius {

const char* to_string(CalibrationMonitor::Outcome o) noexcept {
    switch (o) {
        case CalibrationMonitor::Outcome::true_positive:  return "true_positive";
        case CalibrationMonitor::Outcome::false_positive: return "false_positive";
        case CalibrationMonitor::Outcome::true_negative:  return "true_negative";
        case CalibrationMonitor::Outcome::false_negative: return "false_negative";
    }
    return "unknown";
}

CalibrationMonitor::CalibrationMonitor() : targets_{} {}
CalibrationMonitor::CalibrationMonitor(Targets t) : targets_(t) {}

void CalibrationMonitor::record(Outcome o) {
    record_n(o, 1);
}

void CalibrationMonitor::record_n(Outcome o, std::size_t n) {
    if (n == 0) return;
    std::lock_guard<std::mutex> lk(mu_);
    switch (o) {
        case Outcome::true_positive:  tp_ += n; break;
        case Outcome::false_positive: fp_ += n; break;
        case Outcome::true_negative:  tn_ += n; break;
        case Outcome::false_negative: fn_ += n; break;
    }
}

std::size_t CalibrationMonitor::tp() const noexcept {
    std::lock_guard<std::mutex> lk(mu_); return tp_;
}
std::size_t CalibrationMonitor::fp() const noexcept {
    std::lock_guard<std::mutex> lk(mu_); return fp_;
}
std::size_t CalibrationMonitor::tn() const noexcept {
    std::lock_guard<std::mutex> lk(mu_); return tn_;
}
std::size_t CalibrationMonitor::fn() const noexcept {
    std::lock_guard<std::mutex> lk(mu_); return fn_;
}
std::size_t CalibrationMonitor::total() const noexcept {
    std::lock_guard<std::mutex> lk(mu_); return tp_ + fp_ + tn_ + fn_;
}

namespace {

double safe_ratio(std::size_t numerator, std::size_t denominator) noexcept {
    if (denominator == 0) return std::nan("");
    return static_cast<double>(numerator) / static_cast<double>(denominator);
}

}  // namespace

double CalibrationMonitor::sensitivity() const noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    return safe_ratio(tp_, tp_ + fn_);
}

double CalibrationMonitor::specificity() const noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    return safe_ratio(tn_, tn_ + fp_);
}

double CalibrationMonitor::ppv() const noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    return safe_ratio(tp_, tp_ + fp_);
}

double CalibrationMonitor::npv() const noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    return safe_ratio(tn_, tn_ + fn_);
}

double CalibrationMonitor::accuracy() const noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    return safe_ratio(tp_ + tn_, tp_ + fp_ + tn_ + fn_);
}

namespace {

bool below_floor_check(double empirical, double floor, double tolerance) noexcept {
    if (std::isnan(empirical)) return false;
    return empirical < (floor - tolerance);
}

}  // namespace

bool CalibrationMonitor::sensitivity_below_floor(std::size_t min_samples) const noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    const auto t = tp_ + fp_ + tn_ + fn_;
    if (t < min_samples) return false;
    return below_floor_check(safe_ratio(tp_, tp_ + fn_),
                             targets_.sensitivity_floor,
                             targets_.tolerance);
}

bool CalibrationMonitor::specificity_below_floor(std::size_t min_samples) const noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    const auto t = tp_ + fp_ + tn_ + fn_;
    if (t < min_samples) return false;
    return below_floor_check(safe_ratio(tn_, tn_ + fp_),
                             targets_.specificity_floor,
                             targets_.tolerance);
}

bool CalibrationMonitor::is_below_floor(std::size_t min_samples) const noexcept {
    return sensitivity_below_floor(min_samples) ||
           specificity_below_floor(min_samples);
}

CalibrationMonitor::Targets CalibrationMonitor::targets() const noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    return targets_;
}

void CalibrationMonitor::set_targets(Targets t) {
    std::lock_guard<std::mutex> lk(mu_);
    targets_ = t;
}

void CalibrationMonitor::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    tp_ = fp_ = tn_ = fn_ = 0;
}

CalibrationMonitor::Snapshot
CalibrationMonitor::snapshot(std::size_t min_samples) const noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    const auto t = tp_ + fp_ + tn_ + fn_;
    Snapshot s{tp_, fp_, tn_, fn_, t,
               safe_ratio(tp_, tp_ + fn_),
               safe_ratio(tn_, tn_ + fp_),
               safe_ratio(tp_, tp_ + fp_),
               safe_ratio(tn_, tn_ + fn_),
               safe_ratio(tp_ + tn_, t),
               false};
    if (t >= min_samples) {
        s.below_floor =
            below_floor_check(s.sensitivity, targets_.sensitivity_floor, targets_.tolerance) ||
            below_floor_check(s.specificity, targets_.specificity_floor, targets_.tolerance);
    }
    return s;
}

std::string CalibrationMonitor::summary_string() const {
    auto s = snapshot();
    return fmt::format("tp={} fp={} tn={} fn={} sens={:.4f} spec={:.4f}",
                       s.tp, s.fp, s.tn, s.fn,
                       std::isnan(s.sensitivity) ? 0.0 : s.sensitivity,
                       std::isnan(s.specificity) ? 0.0 : s.specificity);
}

}  // namespace asclepius
