// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include "asclepius/telemetry.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace asclepius {

void MetricRegistry::inc(std::string_view name, std::uint64_t delta) {
    std::lock_guard<std::mutex> lk(mu_);
    counters_[std::string{name}] += delta;
}

void MetricRegistry::observe(std::string_view name, double /*value*/) {
    // Hooked in for future histogram support.
    std::lock_guard<std::mutex> lk(mu_);
    ++counters_[std::string{name} + ".observed"];
}

std::uint64_t MetricRegistry::count(std::string_view name) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = counters_.find(std::string{name});
    return it == counters_.end() ? 0 : it->second;
}

std::string MetricRegistry::snapshot_json() const {
    std::lock_guard<std::mutex> lk(mu_);
    nlohmann::json j = nlohmann::json::object();
    for (const auto& [k, v] : counters_) j[k] = v;
    return j.dump();
}

namespace {

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

}  // namespace

std::string MetricRegistry::snapshot_prometheus() const {
    std::lock_guard<std::mutex> lk(mu_);
    // Sort keys for deterministic output (helps cache + diff tests).
    std::vector<std::string> keys;
    keys.reserve(counters_.size());
    for (const auto& [k, _] : counters_) keys.push_back(k);
    std::sort(keys.begin(), keys.end());

    std::string out;
    out.reserve(64 * keys.size());
    for (const auto& k : keys) {
        std::string name = "asclepius_" + sanitize_metric_name(k);
        out += "# HELP " + name + " counter (auto-emitted by MetricRegistry)\n";
        out += "# TYPE " + name + " counter\n";
        out += name + " " + std::to_string(counters_.at(k)) + "\n";
    }
    return out;
}

}  // namespace asclepius
