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

Histogram::Histogram(Histogram&& other) noexcept
    : lo_(0.0), hi_(1.0), counts_(1, 0), total_(0) {
    // The mutex is intentionally NEVER moved (mutexes have no move
    // operations). Default-construct ours; lock the source to snapshot
    // its state under a single critical section so callers that move
    // out of a Histogram concurrently visible to other threads see a
    // consistent post-move state.
    std::lock_guard<std::mutex> lk(other.mu_);
    lo_           = other.lo_;
    hi_           = other.hi_;
    counts_       = std::move(other.counts_);
    total_        = other.total_;
    // Leave the moved-from instance in a usable, empty state. Its
    // counts_ vector has already been moved-from (which leaves it in a
    // valid-but-unspecified state per std::move spec); replace it with
    // a single zeroed bin so subsequent observe()/clear()/etc. still
    // satisfy invariants. total_ resets to 0 so total() reads sane.
    other.counts_.assign(1, 0);
    other.total_ = 0;
}

Histogram& Histogram::operator=(Histogram&& other) noexcept {
    if (this == &other) {
        // Self-assign is a no-op. We don't even take the lock; the
        // pointer comparison is enough and avoids the std::scoped_lock
        // double-lock-on-self UB scenario we'd otherwise hit.
        return *this;
    }
    // Acquire both mutexes simultaneously to avoid the AB/BA deadlock
    // a naive sequential lock would expose under concurrent moves of
    // two histograms (h1 = std::move(h2) in one thread,
    // h2 = std::move(h1) in another). std::scoped_lock invokes
    // std::lock under the hood with the same deadlock-avoidance.
    std::scoped_lock lk(this->mu_, other.mu_);
    lo_     = other.lo_;
    hi_     = other.hi_;
    counts_ = std::move(other.counts_);
    total_  = other.total_;
    // Leave moved-from in the same usable-empty state as the move
    // constructor.
    other.counts_.assign(1, 0);
    other.total_ = 0;
    return *this;
}

Result<void> Histogram::scale_by(double factor) {
    if (factor < 0.0) {
        return Error::invalid("Histogram::scale_by: factor must be >= 0");
    }
    std::lock_guard<std::mutex> lk(mu_);
    // factor == 0 effectively clears every bin (and total_).
    // factor != 0 multiplies each bin by factor and floors to the
    // nearest integer count. We recompute total_ from the post-scale
    // bin sums rather than scaling total_ separately, because the per-bin
    // floor operation can reduce the sum below floor(total_ * factor)
    // when the fractional remainders accumulate — so using the bin sum
    // is the only way to keep total_ == sum(counts_), which downstream
    // statistics (mean, normalized, psi) all assume.
    std::uint64_t new_total = 0;
    for (auto& c : counts_) {
        const double scaled = static_cast<double>(c) * factor;
        // std::floor on a non-negative scaled value is equivalent to
        // truncation toward zero; cast directly. Guard against NaN /
        // overflow by clamping to 0 on non-finite results.
        if (!(scaled >= 0.0)) {
            c = 0;
            continue;
        }
        c = static_cast<std::uint64_t>(scaled);
        new_total += c;
    }
    total_ = new_total;
    return Result<void>::ok();
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

double Histogram::median() const {
    // Convenience wrapper. quantile() takes its own lock and handles
    // the empty case (returns 0.0).
    return quantile(0.5);
}

void Histogram::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    std::fill(counts_.begin(), counts_.end(), 0);
    total_ = 0;
}

std::size_t Histogram::nonzero_bin_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::size_t n = 0;
    for (auto c : counts_) {
        if (c > 0) ++n;
    }
    return n;
}

Result<std::uint64_t> Histogram::bin_at(std::size_t i) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (i >= counts_.size()) {
        return Error::invalid("Histogram::bin_at: index out of range");
    }
    return counts_[i];
}

Result<std::uint64_t> Histogram::cumulative_at(std::size_t i) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (i >= counts_.size()) {
        return Error::invalid("Histogram::cumulative_at: index out of range");
    }
    // Sum bins [0, i] inclusive. Composing this from bin_at() would
    // re-acquire the mutex per bin and risk torn reads under
    // concurrent observe(); doing the sum directly under one lock keeps
    // the result a consistent snapshot.
    std::uint64_t cum = 0;
    for (std::size_t k = 0; k <= i; ++k) {
        cum += counts_[k];
    }
    return cum;
}

bool Histogram::is_empty() const noexcept {
    try {
        std::lock_guard<std::mutex> lk(mu_);
        return total_ == 0;
    } catch (...) {
        // Lock acquisition can theoretically throw. Contract is
        // noexcept; degrade to "treat as empty" rather than propagate.
        return true;
    }
}

double Histogram::percentile(double p) const {
    // p in [0, 100]; quantile() expects [0, 1] and clamps internally,
    // but we clamp here too so the conversion is well-defined for any
    // input (e.g. negative or > 100).
    const double pc = std::clamp(p, 0.0, 100.0);
    return quantile(pc / 100.0);
}

double Histogram::p99() const {
    // Sugar over percentile(99.0). percentile() takes its own lock and
    // handles the empty case (returns 0.0 via quantile()). The 99th
    // percentile is common enough on operator dashboards that giving
    // it its own name keeps call sites self-documenting — `h.p99()`
    // reads as the tail-latency cutoff rather than a magic number.
    return percentile(99.0);
}

double Histogram::p50() const {
    // Sugar over percentile(50.0). Distinct from median() in name only —
    // both call quantile(0.5) under the hood and are bit-for-bit
    // identical; this exists so call sites that lay out the canonical
    // p50/p95/p99 triple read uniformly without mixing
    // median()+p99() naming. percentile() takes its own lock and
    // handles the empty case (returns 0.0 via quantile()).
    return percentile(50.0);
}

double Histogram::p90() const {
    // Sugar over percentile(90.0). The "warning band" companion to
    // p99 — operators reach for p90 when they want a less-noisy
    // dashboard signal that still excludes the slowest 10% of
    // traffic. percentile() takes its own lock and handles empty.
    return percentile(90.0);
}

double Histogram::p95() const {
    // Sugar over percentile(95.0). The industry-default warning
    // ceiling between p90 and p99; operators typically dashboard
    // p50/p95/p99 as the canonical triple. percentile() takes its
    // own lock and handles the empty case.
    return percentile(95.0);
}

double Histogram::bin_midpoint(std::size_t i) const {
    // Locked because counts_.size() is held under mu_ semantically (the
    // vector is never resized post-construction in practice, but we
    // honor the documented locking pattern of the rest of the API).
    // Saturation: indices >= bin_count() collapse to the last bin's
    // midpoint, which equals hi() - bin_width/2. We deliberately do NOT
    // return Result<double> with an out-of-range error — callers reach
    // for bin_midpoint() to label dashboard rows or chart x-axes, where
    // the saturation is a more useful contract than a hard error.
    std::lock_guard<std::mutex> lk(mu_);
    const auto   n     = counts_.size();
    const double bin_w = (hi_ - lo_) / static_cast<double>(n);
    // Saturate i to the last valid bin index (n - 1). Constructor
    // guarantees n >= 1 (zero-bin histograms are padded), so n - 1 is
    // always a valid index here.
    const std::size_t idx = (i >= n ? n - 1 : i);
    return lo_ + bin_w * (static_cast<double>(idx) + 0.5);
}

std::pair<double, double> Histogram::confidence_interval_95() const {
    // Compute mean AND stddev under a single lock so a concurrent
    // observe() can't tear the pair. Calling mean() then stddev() each
    // takes the lock separately, which would expose a (mean_at_t0,
    // stddev_at_t1) read where the two no longer correspond — that
    // would make the resulting interval mathematically inconsistent.
    // Instead we inline the same loops mean()/stddev() use under one
    // critical section.
    std::lock_guard<std::mutex> lk(mu_);
    if (total_ == 0) return {0.0, 0.0};
    const auto   n     = counts_.size();
    const double bin_w = (hi_ - lo_) / static_cast<double>(n);
    // Mean: sum of midpoint * count, divided by total.
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double mid = lo_ + bin_w * (static_cast<double>(i) + 0.5);
        sum += mid * static_cast<double>(counts_[i]);
    }
    const double m = sum / static_cast<double>(total_);
    // total_ == 1: stddev is 0.0 (matching stddev()'s contract). Return
    // a degenerate interval (m, m) rather than (m - 0, m + 0) — same
    // numerically, but the explicit branch avoids the unused variance
    // pass below for the single-sample case.
    if (total_ == 1) return {m, m};
    double var = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double mid = lo_ + bin_w * (static_cast<double>(i) + 0.5);
        const double d   = mid - m;
        var += d * d * static_cast<double>(counts_[i]);
    }
    var /= static_cast<double>(total_);
    const double sd = std::sqrt(var);
    // 1.96 is the canonical normal-approximation z-score for a 95%
    // two-sided CI. Documented as a "rough" interval — the
    // normal-approximation assumption only holds for near-normal
    // empirical distributions, which the kernel does not enforce.
    constexpr double z95 = 1.96;
    return {m - z95 * sd, m + z95 * sd};
}

std::size_t Histogram::nth_largest_bin() const {
    std::lock_guard<std::mutex> lk(mu_);
    // Empty histogram → every bin has count 0, smallest index wins
    // (which is index 0). Since constructor pads bins to at least 1,
    // the iteration below also returns 0 in that case; we just
    // short-circuit to make the contract obvious.
    if (counts_.empty()) return 0;
    std::size_t best_idx = 0;
    std::uint64_t best_count = counts_[0];
    for (std::size_t i = 1; i < counts_.size(); ++i) {
        // Strict greater-than: ties broken by smallest index, so we
        // only update when the new bin is strictly larger.
        if (counts_[i] > best_count) {
            best_count = counts_[i];
            best_idx   = i;
        }
    }
    return best_idx;
}

std::uint64_t Histogram::observed_max_bin_count() const {
    // Companion to nth_largest_bin() — that returns the modal bin's
    // INDEX, this returns the modal bin's COUNT. Both are O(n) in
    // bin_count() under a single lock; we don't compose this on top
    // of nth_largest_bin() because that would require two locks (or a
    // recursive mutex) and risk torn reads under concurrent observe().
    std::lock_guard<std::mutex> lk(mu_);
    std::uint64_t best = 0;
    for (auto c : counts_) {
        if (c > best) best = c;
    }
    return best;
}

bool Histogram::is_balanced(double max_share) const {
    // Heuristic shape probe: max_bin_count / total < max_share. We
    // clamp max_share to [0, 1] so callers passing negative or > 1
    // values get well-defined behavior — negative collapses to 0 (no
    // bin can hold less than 0% of mass, so every non-empty histogram
    // is "imbalanced" under that cutoff), 1.0 is the "everything is
    // balanced" sentinel (any histogram passes).
    //
    // Empty histogram is vacuously balanced — there's no bin holding
    // more than max_share of the (zero) mass. This matches is_empty()'s
    // / median()'s "no-data → trivially-OK" sentinel pattern and lets
    // dashboards gate on is_balanced() without first checking total().
    //
    // Strict less-than (not <=): max_share == 1.0 must accept a
    // histogram where every observation lands in one bin (share == 1.0
    // would otherwise fail). We use the multiplicative form
    // (max_count < max_share * total) instead of dividing to avoid
    // FP precision issues at the boundary and to skip the divide.
    const double share = std::clamp(max_share, 0.0, 1.0);
    std::lock_guard<std::mutex> lk(mu_);
    if (total_ == 0) return true;
    std::uint64_t best = 0;
    for (auto c : counts_) {
        if (c > best) best = c;
    }
    // Compare best / total < share without dividing. Special-case
    // share == 1.0 so we accept the "all in one bin" extreme (where
    // best == total exactly) — the strict-less-than would otherwise
    // miss it.
    if (share >= 1.0) return true;
    return static_cast<double>(best) < share * static_cast<double>(total_);
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

std::pair<double, double> Histogram::observed_range() const {
    // Wrap min() + max() under a single lock so the returned pair is a
    // consistent snapshot. min()/max() each take the mutex separately;
    // a concurrent observe() between two split calls could expose a
    // (hi_min, lo_max) pair that no single state of the histogram
    // produced. Compute both from one snapshot of counts_ instead.
    std::lock_guard<std::mutex> lk(mu_);
    if (total_ == 0) {
        return {lo_, hi_};
    }
    const auto   n     = counts_.size();
    const double bin_w = (hi_ - lo_) / static_cast<double>(n);
    double observed_min = lo_;
    double observed_max = hi_;
    // Forward scan for the first non-empty bin.
    for (std::size_t i = 0; i < n; ++i) {
        if (counts_[i] > 0) {
            observed_min = lo_ + bin_w * (static_cast<double>(i) + 0.5);
            break;
        }
    }
    // Backward scan for the last non-empty bin.
    for (std::size_t i = n; i-- > 0;) {
        if (counts_[i] > 0) {
            observed_max = lo_ + bin_w * (static_cast<double>(i) + 0.5);
            break;
        }
    }
    return {observed_min, observed_max};
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

Histogram Histogram::clone() const {
    // Explicit deep copy. The implicit copy constructor is deleted
    // because Histogram holds a mutex (mutexes are non-copyable), so
    // callers that want a snapshot need this verb. Lock the source
    // under mu_ so the read of lo_/hi_/counts_/total_ is a consistent
    // snapshot under concurrent observe(); the destination's mutex is
    // default-constructed (mutexes are intentionally never copied —
    // each Histogram owns its own).
    std::lock_guard<std::mutex> lk(mu_);
    Histogram out{lo_, hi_, counts_.size()};
    // out's constructor zero-initialized counts_ and total_; overwrite
    // them with the snapshotted state. We don't need to lock out.mu_
    // because it was default-constructed in this function and is not
    // visible to any other thread yet.
    out.counts_ = counts_;
    out.total_  = total_;
    return out;
}

Histogram Histogram::with_added(const Histogram& other) const {
    // Non-mutating mirror of merge(). Acquire both mutexes via
    // std::scoped_lock to avoid the AB/BA deadlock a sequential lock
    // would expose under concurrent with_added() between two
    // histograms (h1.with_added(h2) on one thread, h2.with_added(h1)
    // on another). std::scoped_lock invokes std::lock under the hood
    // with the same deadlock-avoidance.
    if (this == &other) {
        // Self with_added: equivalent to clone() then merge(*this) —
        // every bin doubled. Single lock, single pass.
        std::lock_guard<std::mutex> lk(mu_);
        Histogram out{lo_, hi_, counts_.size()};
        for (std::size_t i = 0; i < counts_.size(); ++i) {
            out.counts_[i] = counts_[i] + counts_[i];
        }
        out.total_ = total_ + total_;
        return out;
    }
    std::scoped_lock lk(mu_, other.mu_);
    if (counts_.size() != other.counts_.size()
     || lo_ != other.lo_
     || hi_ != other.hi_) {
        // Geometry mismatch: return an empty histogram with *this's
        // geometry rather than propagating an error. This matches
        // scale_by(0)'s "preserve geometry, drop counts" pattern and
        // keeps the call site exception-free for exploratory /
        // dashboard code that probes "what would the merged shape look
        // like?" without owning a transactional contract.
        return Histogram{lo_, hi_, counts_.size()};
    }
    Histogram out{lo_, hi_, counts_.size()};
    for (std::size_t i = 0; i < counts_.size(); ++i) {
        out.counts_[i] = counts_[i] + other.counts_[i];
    }
    out.total_ = total_ + other.total_;
    return out;
}

void Histogram::reset_to(const Histogram& other) {
    if (this == &other) {
        // Self-assignment is a content-preserving no-op.
        return;
    }
    std::lock(mu_, other.mu_);
    std::lock_guard<std::mutex> lk_self (mu_,       std::adopt_lock);
    std::lock_guard<std::mutex> lk_other(other.mu_, std::adopt_lock);
    lo_     = other.lo_;
    hi_     = other.hi_;
    counts_ = other.counts_;
    total_  = other.total_;
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

void DriftMonitor::set_alert_threshold(DriftSeverity severity) {
    // Distinct from set_alert_sink(): re-tunes only the threshold while
    // leaving the installed sink (and last_severity_ map) untouched.
    // Operators reach for this when they want to dial a noisy sink
    // up to "severe" during a rollout without re-wiring the sink. The
    // last_severity_ map is INTENTIONALLY preserved — features that
    // already crossed the prior threshold stay marked, so a threshold
    // change alone won't cause re-fires for already-fired features.
    // Pair with clear_alerts() if a threshold change should also reset
    // the per-feature alert ladder.
    std::lock_guard<std::mutex> lk(mu_);
    alert_threshold_ = severity;
}

bool DriftMonitor::has_alert_sink() const noexcept {
    try {
        std::lock_guard<std::mutex> lk(mu_);
        return static_cast<bool>(alert_sink_);
    } catch (...) {
        // Lock acquisition can theoretically throw. Contract is noexcept;
        // degrade to "not installed" rather than propagate.
        return false;
    }
}

Result<void> DriftMonitor::observe_batch(std::string_view        feature,
                                         std::span<const double> values) {
    DriftReport report_to_emit;
    bool        should_emit = false;
    AlertSink   sink_copy;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = features_.find(std::string{feature});
        if (it == features_.end()) {
            return Error::not_found(std::string{"unregistered feature: "} + std::string{feature});
        }
        // Empty span is a valid no-op: do nothing, don't fire the sink.
        if (values.empty()) {
            return Result<void>::ok();
        }
        for (double v : values) {
            it->second->current->observe(v);
        }

        // Single post-batch severity evaluation. Fire at most once per
        // call when severity strictly rises above the previous recorded
        // severity AND meets the configured threshold.
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
    if (should_emit) {
        try { sink_copy(report_to_emit); } catch (...) { /* swallow */ }
    }
    return Result<void>::ok();
}

Result<DriftReport> DriftMonitor::report_for_feature(std::string_view feature) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = features_.find(std::string{feature});
    if (it == features_.end()) {
        return Error::not_found(fmt::format("unregistered feature: {}", feature));
    }
    DriftReport r;
    r.feature      = std::string{feature};
    r.psi          = Histogram::psi(*it->second->reference, *it->second->current);
    r.ks_statistic = Histogram::ks (*it->second->reference, *it->second->current);
    r.emd          = Histogram::emd(*it->second->reference, *it->second->current);
    r.severity     = classify(r.psi);
    r.reference_n  = it->second->reference->total();
    r.current_n    = it->second->current->total();
    r.computed_at  = Time::now();
    return r;
}

void DriftMonitor::observe_uniform(std::string_view feature, double value, std::size_t n) {
    DriftReport report_to_emit;
    bool        should_emit = false;
    AlertSink   sink_copy;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = features_.find(std::string{feature});
        if (it == features_.end()) {
            // Silent no-op when the feature is unregistered — distinct
            // from observe() which returns not_found.
            return;
        }
        if (n == 0) {
            // Zero-count batch: nothing to fold in, no sink fire.
            return;
        }
        for (std::size_t i = 0; i < n; ++i) {
            it->second->current->observe(value);
        }

        // Single post-batch severity evaluation. Mirrors observe_batch().
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
    if (should_emit) {
        try { sink_copy(report_to_emit); } catch (...) { /* swallow */ }
    }
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

void DriftMonitor::reset_all() {
    // All-features analogue of reset(name). Like reset(), this clears
    // the per-feature current window AND the last_severity_ tracking
    // map — distinct from rotate(), which only rebuilds the current
    // histograms and leaves alert tracking intact. The combined
    // semantics let an operator declare a fresh start: data window
    // empty AND alert ladder reset, so the sink can re-fire from the
    // bottom on the next observation that crosses the threshold.
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [_, fs] : features_) {
        fs->current = std::make_unique<Histogram>(
            fs->current->lo(), fs->current->hi(), fs->current->bin_count());
    }
    last_severity_.clear();
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

std::size_t DriftMonitor::feature_count_observed() const {
    // Distinct from feature_count(): counts only features whose current
    // window has at least one observation. Operators use this to gate
    // dashboards on "are we receiving traffic on every feature we're
    // watching?" without enumerating list_features() and probing
    // observation_count() per name.
    std::lock_guard<std::mutex> lk(mu_);
    std::size_t n = 0;
    for (const auto& [_, fs] : features_) {
        if (fs->current->total() > 0) ++n;
    }
    return n;
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

bool DriftMonitor::is_registered(std::string_view feature) const noexcept {
    // Sugar wrapper: identical semantics to has_feature(), exposed under
    // a name that reads naturally as a registration predicate. Callers
    // that frame the question as "did register_feature() run for this
    // name?" reach for this; callers that frame it as "does the monitor
    // own this name?" reach for has_feature(). Both swallow internal
    // errors → false, matching the noexcept contract.
    return has_feature(feature);
}

Result<std::uint64_t> DriftMonitor::observation_count(std::string_view feature) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = features_.find(std::string{feature});
    if (it == features_.end()) {
        return Error::not_found(fmt::format("unregistered feature: {}", feature));
    }
    return it->second->current->total();
}

std::uint64_t DriftMonitor::observation_total() const {
    // Aggregate sweep across every registered feature's current window.
    // We don't compose this on top of summary() (which also computes
    // PSI per feature) because callers reach for observation_total()
    // when they only want a traffic-volume probe — paying for the PSI
    // pass would be wasted work. Locked once for the whole sum so the
    // result is a consistent snapshot under concurrent observe().
    std::lock_guard<std::mutex> lk(mu_);
    std::uint64_t s = 0;
    for (const auto& [_, fs] : features_) {
        s += fs->current->total();
    }
    return s;
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

std::vector<std::string> DriftMonitor::list_severe_features() const {
    // Sugar over report() filtered by severity == severe, returned in
    // alphabetical order so dashboards / tests get deterministic output.
    // We don't reuse report() directly because that would force two
    // passes (build the reports vector, then filter); instead we walk
    // features_ once under the lock and only allocate strings for the
    // matching set. The resulting names are then sorted in place.
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> out;
    out.reserve(features_.size());
    for (const auto& [name, fs] : features_) {
        const double psi = Histogram::psi(*fs->reference, *fs->current);
        if (classify(psi) == DriftSeverity::severe) {
            out.push_back(name);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool DriftMonitor::has_any_severe() const noexcept {
    // Cheap predicate that stops on the first severe feature, so it's
    // strictly cheaper than !list_severe_features().empty() (no
    // allocation, no full enumeration). Distinct from any_severe()
    // only in the noexcept contract: any_severe() can theoretically
    // propagate a lock or hash-map exception, while this swallows
    // them and degrades to "no severe features" — matching the
    // conservative degradation of has_alert_sink() / is_registered().
    try {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& [_, fs] : features_) {
            const double psi = Histogram::psi(*fs->reference, *fs->current);
            if (classify(psi) == DriftSeverity::severe) {
                return true;
            }
        }
        return false;
    } catch (...) {
        return false;
    }
}

Result<std::uint64_t> DriftMonitor::baseline_count(std::string_view feature) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = features_.find(std::string{feature});
    if (it == features_.end()) {
        return Error::not_found(fmt::format("unregistered feature: {}", feature));
    }
    return it->second->reference->total();
}

void DriftMonitor::clear_alerts() {
    std::lock_guard<std::mutex> lk(mu_);
    last_severity_.clear();
}

Result<DriftSeverity> DriftMonitor::feature_severity(std::string_view feature) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = features_.find(std::string{feature});
    if (it == features_.end()) {
        return Error::not_found(fmt::format("unregistered feature: {}", feature));
    }
    const double psi = Histogram::psi(*it->second->reference, *it->second->current);
    return classify(psi);
}

std::vector<DriftReport>
DriftMonitor::trend_for_feature(std::string_view feature, std::size_t /*n*/) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = features_.find(std::string{feature});
    if (it == features_.end()) {
        // "Show me a trend" call: a missing feature returns an empty
        // vector rather than an error. Future versions will buffer up
        // to `n` historical snapshots; for now we surface a single
        // current snapshot regardless of `n`.
        return {};
    }
    DriftReport r;
    r.feature      = std::string{feature};
    r.psi          = Histogram::psi(*it->second->reference, *it->second->current);
    r.ks_statistic = Histogram::ks (*it->second->reference, *it->second->current);
    r.emd          = Histogram::emd(*it->second->reference, *it->second->current);
    r.severity     = classify(r.psi);
    r.reference_n  = it->second->reference->total();
    r.current_n    = it->second->current->total();
    r.computed_at  = Time::now();
    return std::vector<DriftReport>{std::move(r)};
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

Result<std::pair<std::string, DriftReport>>
DriftMonitor::worst_psi_feature() const {
    // Single-pass companion to most_drifted_feature() that builds the
    // full DriftReport for the winning feature instead of just its
    // name. We can't compose this on top of most_drifted_feature() +
    // report_for_feature() because each one takes its own lock — under
    // concurrent observe(), the winning feature's PSI could shift
    // between the two reads, producing an inconsistent (name, report)
    // pair. Instead we walk features_ once under a single lock,
    // tracking both the best name AND the best PSI value, then
    // construct the full report from the same iterator's FeatureState
    // before releasing the lock. Tie-breaking matches
    // most_drifted_feature(): equal PSI broken by alphabetical name.
    std::lock_guard<std::mutex> lk(mu_);
    if (features_.empty()) {
        return Error::not_found("no features registered");
    }
    using FsIt = decltype(features_)::const_iterator;
    FsIt   best_it  = features_.end();
    double best_psi = 0.0;
    for (auto it = features_.begin(); it != features_.end(); ++it) {
        const double psi = Histogram::psi(*it->second->reference,
                                          *it->second->current);
        if (best_it == features_.end()
         || psi > best_psi
         || (psi == best_psi && it->first < best_it->first)) {
            best_it  = it;
            best_psi = psi;
        }
    }
    DriftReport r;
    r.feature      = best_it->first;
    r.psi          = best_psi;
    r.ks_statistic = Histogram::ks (*best_it->second->reference,
                                    *best_it->second->current);
    r.emd          = Histogram::emd(*best_it->second->reference,
                                    *best_it->second->current);
    r.severity     = classify(r.psi);
    r.reference_n  = best_it->second->reference->total();
    r.current_n    = best_it->second->current->total();
    r.computed_at  = Time::now();
    return std::pair<std::string, DriftReport>{best_it->first, std::move(r)};
}

}  // namespace asclepius
