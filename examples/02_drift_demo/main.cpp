// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
//
// Worked example: detect drift on a model's confidence score.
//
// We register a baseline (held-out validation distribution centered ~0.7),
// then simulate production observations slowly drifting downward — the
// kind of decay that happens when the underlying patient population
// shifts (post-COVID, new specialty mix, etc.). The example prints PSI,
// KS, EMD over time and flags severity transitions.

#include <fmt/core.h>

#include "asclepius/telemetry.hpp"

#include <random>

using namespace asclepius;

int main() {
    DriftMonitor dm;

    // Baseline: 1000 samples around 0.7.
    std::mt19937 rng{42};
    std::vector<double> baseline;
    {
        std::normal_distribution<double> nd{0.7, 0.05};
        baseline.reserve(1000);
        for (int i = 0; i < 1000; ++i) baseline.push_back(std::clamp(nd(rng), 0.0, 1.0));
    }
    if (auto r = dm.register_feature("confidence", baseline); !r) {
        fmt::print(stderr, "register: {}\n", r.error().what());
        return 1;
    }

    // Production: drift the mean from 0.7 down to 0.4 over 5 windows.
    for (int window = 0; window < 5; ++window) {
        const double mean = 0.7 - 0.075 * window;
        std::normal_distribution<double> nd{mean, 0.05};
        for (int i = 0; i < 500; ++i) {
            (void)dm.observe("confidence", std::clamp(nd(rng), 0.0, 1.0));
        }
        const auto rep = dm.report();
        for (const auto& r : rep) {
            fmt::print("window={}  feat={:14} psi={:6.3f}  ks={:5.3f}  emd={:6.3f}  "
                       "severity={}  n_ref={} n_cur={}\n",
                       window, r.feature, r.psi, r.ks_statistic, r.emd,
                       to_string(r.severity), r.reference_n, r.current_n);
        }
        dm.rotate();
    }
    return 0;
}
