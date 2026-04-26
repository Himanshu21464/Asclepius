// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include "asclepius/telemetry.hpp"

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

}  // namespace asclepius
