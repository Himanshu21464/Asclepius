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

std::vector<std::string> MetricRegistry::list_counters() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> out;
    out.reserve(counters_.size());
    for (const auto& [name, _] : counters_) out.push_back(name);
    return out;
}

}  // namespace asclepius
