// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include "asclepius/telemetry.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>

namespace asclepius {

namespace {

// Default histogram bucket set, latency-shaped (seconds). Scales smoothly
// from sub-millisecond up to multi-second; +Inf catches the long tail.
const std::vector<double>& default_buckets() {
    static const std::vector<double> b{
        0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0,
        std::numeric_limits<double>::infinity(),
    };
    return b;
}

// Prometheus name rules: [a-zA-Z_:][a-zA-Z0-9_:]*. Anything else gets a '_'.
std::string sanitize_metric_name(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    bool first = true;
    for (char c : in) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':';
        if (!first) ok = ok || (c >= '0' && c <= '9');
        out.push_back(ok ? c : '_');
        first = false;
    }
    if (out.empty()) out = "_";
    return out;
}

// Format a double for Prometheus line output. +Inf gets the literal "+Inf"
// per spec; finite values use up to 6 decimals trimmed of trailing zeros.
std::string fmt_le(double v) {
    if (std::isinf(v)) return "+Inf";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", v);
    return std::string{buf};
}

}  // namespace

void MetricRegistry::inc(std::string_view name, std::uint64_t delta) {
    std::lock_guard<std::mutex> lk(mu_);
    counters_[std::string{name}] += delta;
}

void MetricRegistry::add(std::string_view name, std::uint64_t delta) {
    // Alias for inc(); prefer the explicit verb at call sites that are
    // recording a domain quantity rather than ticking an event.
    inc(name, delta);
}

void MetricRegistry::increment_or_create(std::string_view name, std::uint64_t delta) {
    // Verbose alias for inc(). Forwards verbatim — inc() already has
    // create-on-first-use semantics via operator[] on counters_.
    inc(name, delta);
}

void MetricRegistry::observe(std::string_view name, double value) {
    std::lock_guard<std::mutex> lk(mu_);
    auto& h = histograms_[std::string{name}];
    if (h.buckets.empty()) {
        h.buckets       = default_buckets();
        h.bucket_counts.assign(h.buckets.size(), 0);
    }
    // Cumulative buckets per Prometheus convention: count_of(value <= le).
    for (std::size_t i = 0; i < h.buckets.size(); ++i) {
        if (value <= h.buckets[i]) ++h.bucket_counts[i];
    }
    ++h.count;
    h.sum += value;
}

std::uint64_t MetricRegistry::count(std::string_view name) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = counters_.find(std::string{name});
    if (it != counters_.end()) return it->second;
    auto hit = histograms_.find(std::string{name});
    if (hit != histograms_.end()) return hit->second.count;
    return 0;
}

Result<std::uint64_t> MetricRegistry::counter_value(std::string_view name) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = counters_.find(std::string{name});
    if (it == counters_.end()) {
        return Error::not_found("unknown counter");
    }
    return it->second;
}

std::uint64_t MetricRegistry::counter_with_default(std::string_view name,
                                                   std::uint64_t    default_value) const {
    // Mirrors count()'s name resolution (counters first, then histograms),
    // but substitutes default_value for the "neither found" branch instead
    // of count()'s silent 0. Lets callers distinguish "missing" from "0"
    // without paying for a Result allocation.
    std::lock_guard<std::mutex> lk(mu_);
    auto it = counters_.find(std::string{name});
    if (it != counters_.end()) return it->second;
    auto hit = histograms_.find(std::string{name});
    if (hit != histograms_.end()) return hit->second.count;
    return default_value;
}

double MetricRegistry::histogram_quantile(std::string_view name, double q) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = histograms_.find(std::string{name});
    if (it == histograms_.end()) return 0.0;
    const auto& h = it->second;
    if (h.count == 0) return 0.0;

    const double qc     = std::clamp(q, 0.0, 1.0);
    const double target = qc * static_cast<double>(h.count);
    const auto   n      = h.buckets.size();
    if (n == 0) return 0.0;

    // bucket_counts is cumulative (count of value <= buckets[i]). Walk
    // until the cumulative count reaches target, then linearly interpolate
    // within the bucket using its lower and upper edges. The lower edge
    // of bucket i is buckets[i-1] (or 0.0 for i==0); the upper edge is
    // buckets[i]. For the +Inf upper edge, fall back to the prior bucket's
    // upper edge to keep the result finite.
    std::uint64_t prev_cum = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint64_t cum = h.bucket_counts[i];
        if (static_cast<double>(cum) >= target) {
            const double lower = (i == 0 ? 0.0 : h.buckets[i - 1]);
            double upper = h.buckets[i];
            if (std::isinf(upper)) {
                // No finite upper edge; clamp to the lower edge so we
                // return a finite value for the long-tail bucket.
                return lower;
            }
            const double bucket_mass = static_cast<double>(cum - prev_cum);
            double frac = 0.0;
            if (bucket_mass > 0.0) {
                frac = (target - static_cast<double>(prev_cum)) / bucket_mass;
                frac = std::clamp(frac, 0.0, 1.0);
            }
            return lower + (upper - lower) * frac;
        }
        prev_cum = cum;
    }
    // Floating-point edge: target rounded just past the final cumulative.
    // Walk back to the last finite bucket edge.
    for (std::size_t i = n; i-- > 0;) {
        if (!std::isinf(h.buckets[i])) return h.buckets[i];
    }
    return 0.0;
}

std::string MetricRegistry::snapshot_json() const {
    std::lock_guard<std::mutex> lk(mu_);
    nlohmann::json j = nlohmann::json::object();
    for (const auto& [k, v] : counters_) j[k] = v;
    for (const auto& [k, h] : histograms_) {
        nlohmann::json o;
        o["count"]   = h.count;
        o["sum"]     = h.sum;
        nlohmann::json buckets = nlohmann::json::object();
        for (std::size_t i = 0; i < h.buckets.size(); ++i) {
            buckets[fmt_le(h.buckets[i])] = h.bucket_counts[i];
        }
        o["buckets"] = std::move(buckets);
        j[k]         = std::move(o);
    }
    return j.dump();
}

std::string MetricRegistry::snapshot_prometheus() const {
    std::lock_guard<std::mutex> lk(mu_);

    // Sort keys for deterministic output (helps cache + diff tests).
    std::vector<std::string> ckeys, hkeys;
    ckeys.reserve(counters_.size());
    hkeys.reserve(histograms_.size());
    for (const auto& [k, _] : counters_)   ckeys.push_back(k);
    for (const auto& [k, _] : histograms_) hkeys.push_back(k);
    std::sort(ckeys.begin(), ckeys.end());
    std::sort(hkeys.begin(), hkeys.end());

    std::string out;
    out.reserve(64 * (ckeys.size() + 16 * hkeys.size()));

    // Counters first.
    for (const auto& k : ckeys) {
        std::string name = "asclepius_" + sanitize_metric_name(k);
        out += "# HELP " + name + " counter (auto-emitted by MetricRegistry)\n";
        out += "# TYPE " + name + " counter\n";
        out += name + " " + std::to_string(counters_.at(k)) + "\n";
    }
    // Histograms.
    for (const auto& k : hkeys) {
        const auto& h = histograms_.at(k);
        std::string name = "asclepius_" + sanitize_metric_name(k);
        out += "# HELP " + name + " histogram (auto-emitted by MetricRegistry)\n";
        out += "# TYPE " + name + " histogram\n";
        for (std::size_t i = 0; i < h.buckets.size(); ++i) {
            out += name + "_bucket{le=\"" + fmt_le(h.buckets[i]) + "\"} "
                 + std::to_string(h.bucket_counts[i]) + "\n";
        }
        char sumbuf[64];
        std::snprintf(sumbuf, sizeof(sumbuf), "%g", h.sum);
        out += name + "_sum "   + sumbuf + "\n";
        out += name + "_count " + std::to_string(h.count) + "\n";
    }
    return out;
}

Result<void> MetricRegistry::reset(std::string_view name) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = counters_.find(std::string{name});
    if (it == counters_.end()) {
        return Error::not_found("unknown counter");
    }
    it->second = 0;
    return Result<void>::ok();
}

void MetricRegistry::reset_all_counters() {
    // Zero every counter while keeping the names registered. Distinct
    // from clear() (drops everything) and reset_histograms() (drops
    // every histogram). Mirrors DriftMonitor::reset_all() semantics for
    // counters: data zeroed, names preserved, so subsequent
    // counter_value() probes still succeed (returning 0 instead of
    // not_found). Histograms are NOT touched.
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [_, v] : counters_) {
        v = 0;
    }
}

std::unordered_map<std::string, std::uint64_t>
MetricRegistry::counter_snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    return counters_;
}

std::unordered_map<std::string, std::int64_t>
MetricRegistry::diff(const std::unordered_map<std::string, std::uint64_t>& baseline) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::unordered_map<std::string, std::int64_t> out;
    out.reserve(counters_.size() + baseline.size());

    // Forward pass: every current counter, delta = current - baseline_or_0.
    for (const auto& [k, v] : counters_) {
        auto bit = baseline.find(k);
        const std::int64_t prev = (bit == baseline.end()
                                   ? 0
                                   : static_cast<std::int64_t>(bit->second));
        out[k] = static_cast<std::int64_t>(v) - prev;
    }
    // Reverse pass: counters that were in baseline but no longer present
    // → negative delta equal to -baseline (the counter "disappeared").
    for (const auto& [k, v] : baseline) {
        if (counters_.find(k) == counters_.end()) {
            out[k] = -static_cast<std::int64_t>(v);
        }
    }
    return out;
}

Result<std::uint64_t> MetricRegistry::histogram_count(std::string_view name) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = histograms_.find(std::string{name});
    if (it == histograms_.end()) {
        return Error::not_found("unknown histogram");
    }
    return it->second.count;
}

Result<double> MetricRegistry::histogram_sum(std::string_view name) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = histograms_.find(std::string{name});
    if (it == histograms_.end()) {
        return Error::not_found("unknown histogram");
    }
    return it->second.sum;
}

std::vector<std::string> MetricRegistry::list_counters() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> out;
    out.reserve(counters_.size());
    for (const auto& [name, _] : counters_) out.push_back(name);
    return out;
}

std::vector<std::string> MetricRegistry::all_counter_names() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> out;
    out.reserve(counters_.size());
    for (const auto& [name, _] : counters_) out.push_back(name);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string>
MetricRegistry::counter_names_with_prefix(std::string_view prefix) const {
    // Mirror sum_counters_with_prefix()'s "empty prefix matches every
    // counter" sentinel — std::string_view::starts_with(""sv) is
    // unconditionally true, so the branch falls out naturally and an
    // empty prefix degrades to all_counter_names(). Sort the result
    // for stable output (dashboards / diff-friendly snapshots).
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> out;
    out.reserve(counters_.size());
    for (const auto& [name, _] : counters_) {
        if (std::string_view{name}.starts_with(prefix)) {
            out.push_back(name);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string>
MetricRegistry::counters_above(std::uint64_t threshold) const {
    // Strictly greater than `threshold` — operators framing this as
    // "show me the noisy counters above N" expect threshold itself to
    // be the cutoff, not included. counters_above(0) returns every
    // non-zero counter, mirroring the canonical "non-default" probe.
    // Sorted alphabetically so dashboards / diff tests get stable
    // output. Single lock for the whole pass so the snapshot is
    // consistent under concurrent inc().
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> out;
    out.reserve(counters_.size());
    for (const auto& [name, v] : counters_) {
        if (v > threshold) {
            out.push_back(name);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

void MetricRegistry::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    counters_.clear();
    histograms_.clear();
}

void MetricRegistry::reset_histograms() {
    std::lock_guard<std::mutex> lk(mu_);
    histograms_.clear();
}

std::size_t MetricRegistry::counter_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return counters_.size();
}

std::uint64_t MetricRegistry::counter_total() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::uint64_t s = 0;
    for (const auto& [_, v] : counters_) s += v;
    return s;
}

std::uint64_t MetricRegistry::sum_counters_with_prefix(std::string_view prefix) const {
    std::lock_guard<std::mutex> lk(mu_);
    // Empty prefix is the "match every counter" sentinel — equivalent
    // to counter_total(). std::string_view::starts_with(""sv) is
    // unconditionally true, so the branch falls out naturally; we
    // don't need a special case here.
    std::uint64_t s = 0;
    for (const auto& [name, v] : counters_) {
        if (std::string_view{name}.starts_with(prefix)) {
            s += v;
        }
    }
    return s;
}

std::uint64_t MetricRegistry::counter_max() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::uint64_t best = 0;
    for (const auto& [_, v] : counters_) {
        if (v > best) best = v;
    }
    return best;
}

Result<std::uint64_t> MetricRegistry::counter_min() const {
    // Distinct from counter_max(): returns Error::not_found on an empty
    // registry instead of a silent 0, since 0 is a legal counter value
    // and callers gating on "is the smallest counter at least N?" need
    // to distinguish "no counters" from "all counters at 0."
    std::lock_guard<std::mutex> lk(mu_);
    if (counters_.empty()) {
        return Error::not_found("MetricRegistry::counter_min: no counters registered");
    }
    // Initialize from the first counter so the empty-then-iterate
    // pathway can't accidentally produce 0 from std::numeric_limits::max
    // collapsing — we require at least one counter to reach this branch.
    auto it = counters_.begin();
    std::uint64_t best = it->second;
    for (++it; it != counters_.end(); ++it) {
        if (it->second < best) best = it->second;
    }
    return best;
}

std::uint64_t MetricRegistry::histogram_count_total() const {
    // Sum of observation counts across every registered histogram.
    // Distinct from counter_total() (which sums COUNTER values) and
    // from histogram_count(name) (which returns one histogram's count).
    // The "_total" suffix mirrors counter_total()'s "global event
    // count" semantics — both answer "how much activity has the
    // registry recorded?" but on the histogram side rather than the
    // counter side. Locked once for the whole sweep so the returned
    // value is a consistent snapshot under concurrent observe().
    std::lock_guard<std::mutex> lk(mu_);
    std::uint64_t s = 0;
    for (const auto& [_, h] : histograms_) {
        s += h.count;
    }
    return s;
}

bool MetricRegistry::has_counter(std::string_view name) const noexcept {
    try {
        std::lock_guard<std::mutex> lk(mu_);
        return counters_.find(std::string{name}) != counters_.end();
    } catch (...) {
        // Lock acquisition or string construction can theoretically throw.
        // Contract is noexcept; degrade to "not present" rather than
        // propagate.
        return false;
    }
}

bool MetricRegistry::has_histogram(std::string_view name) const noexcept {
    try {
        std::lock_guard<std::mutex> lk(mu_);
        return histograms_.find(std::string{name}) != histograms_.end();
    } catch (...) {
        return false;
    }
}

bool MetricRegistry::has(std::string_view name) const noexcept {
    try {
        std::lock_guard<std::mutex> lk(mu_);
        const std::string key{name};
        return counters_.find(key)   != counters_.end()
            || histograms_.find(key) != histograms_.end();
    } catch (...) {
        // Lock acquisition or string construction can theoretically throw.
        // Contract is noexcept; degrade to "not present" rather than
        // propagate.
        return false;
    }
}

Result<double> MetricRegistry::ratio(std::string_view numerator,
                                     std::string_view denominator) const {
    // Hold the lock once for both lookups so the result is a consistent
    // snapshot — otherwise a concurrent inc() between the two reads could
    // produce ratios that no caller could observe directly.
    std::lock_guard<std::mutex> lk(mu_);
    auto nit = counters_.find(std::string{numerator});
    if (nit == counters_.end()) {
        return Error::not_found("unknown counter (numerator)");
    }
    auto dit = counters_.find(std::string{denominator});
    if (dit == counters_.end()) {
        return Error::not_found("unknown counter (denominator)");
    }
    if (dit->second == 0) {
        return Error::invalid("ratio denominator is zero");
    }
    return static_cast<double>(nit->second) / static_cast<double>(dit->second);
}

std::uint64_t MetricRegistry::counter_diff_total(
    const std::unordered_map<std::string, std::uint64_t>& baseline) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::uint64_t total = 0;
    // Forward pass: every current counter, |current - baseline_or_0|.
    for (const auto& [k, v] : counters_) {
        auto bit = baseline.find(k);
        const std::int64_t prev = (bit == baseline.end()
                                   ? 0
                                   : static_cast<std::int64_t>(bit->second));
        const std::int64_t delta = static_cast<std::int64_t>(v) - prev;
        total += static_cast<std::uint64_t>(delta < 0 ? -delta : delta);
    }
    // Reverse pass: counters in baseline but no longer present contribute
    // |0 - baseline| == baseline.
    for (const auto& [k, v] : baseline) {
        if (counters_.find(k) == counters_.end()) {
            total += v;
        }
    }
    return total;
}

bool MetricRegistry::is_empty() const noexcept {
    try {
        std::lock_guard<std::mutex> lk(mu_);
        return counters_.empty() && histograms_.empty();
    } catch (...) {
        // Lock acquisition can theoretically throw. Contract is
        // noexcept; degrade to "treat as empty" rather than propagate
        // (matches Histogram::is_empty()).
        return true;
    }
}

bool MetricRegistry::has_any() const noexcept {
    // Sugar wrapper over !is_empty(). Sharing the implementation rather
    // than duplicating the lock-and-empty-check keeps the two predicates
    // bitwise-consistent — important for tests that assert
    // `has_any() != is_empty()`. is_empty() is itself noexcept and
    // swallows lock-throw to "treat as empty"; the negation here
    // therefore degrades to "has_any == false" on the same failure,
    // matching the conservative reading.
    return !is_empty();
}

}  // namespace asclepius
