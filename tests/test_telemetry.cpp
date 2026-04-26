// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include <doctest/doctest.h>

#include "asclepius/telemetry.hpp"

#include <random>

using namespace asclepius;

TEST_CASE("Histogram tracks observation counts") {
    Histogram h{0.0, 1.0, 10};
    for (int i = 0; i < 100; ++i) h.observe(0.05);
    CHECK(h.total() == 100);
    CHECK(h.normalized()[0] == doctest::Approx(1.0));
}

TEST_CASE("Histogram::psi is ~0 for identical distributions") {
    Histogram a{0.0, 1.0, 10};
    Histogram b{0.0, 1.0, 10};
    std::mt19937 rng{1};
    std::uniform_real_distribution<double> d{0.0, 1.0};
    for (int i = 0; i < 1000; ++i) {
        auto v = d(rng);
        a.observe(v);
        b.observe(v);
    }
    CHECK(Histogram::psi(a, b) < 0.05);
}

TEST_CASE("Histogram::psi is large for shifted distributions") {
    Histogram a{0.0, 1.0, 10};
    Histogram b{0.0, 1.0, 10};
    std::mt19937 rng{2};
    std::normal_distribution<double> da{0.2, 0.05};
    std::normal_distribution<double> db{0.8, 0.05};
    for (int i = 0; i < 1000; ++i) {
        a.observe(std::clamp(da(rng), 0.0, 1.0));
        b.observe(std::clamp(db(rng), 0.0, 1.0));
    }
    CHECK(Histogram::psi(a, b) > 1.0);
}

TEST_CASE("Histogram::ks ranges in [0, 1]") {
    Histogram a{0.0, 1.0, 10};
    Histogram b{0.0, 1.0, 10};
    std::mt19937 rng{3};
    std::uniform_real_distribution<double> da{0.0, 0.5};
    std::uniform_real_distribution<double> db{0.5, 1.0};
    for (int i = 0; i < 500; ++i) {
        a.observe(da(rng));
        b.observe(db(rng));
    }
    auto ks = Histogram::ks(a, b);
    CHECK(ks >= 0.0);
    CHECK(ks <= 1.0);
    CHECK(ks > 0.5);
}

TEST_CASE("DriftMonitor::classify thresholds") {
    CHECK(DriftMonitor::classify(0.05) == DriftSeverity::none);
    CHECK(DriftMonitor::classify(0.15) == DriftSeverity::minor);
    CHECK(DriftMonitor::classify(0.30) == DriftSeverity::moder);
    CHECK(DriftMonitor::classify(0.80) == DriftSeverity::severe);
}

TEST_CASE("DriftMonitor reports per-feature severity") {
    DriftMonitor dm;
    std::vector<double> baseline(500, 0.2);
    REQUIRE(dm.register_feature("score", baseline));

    for (int i = 0; i < 500; ++i) {
        REQUIRE(dm.observe("score", 0.85));
    }

    auto rep = dm.report();
    REQUIRE(rep.size() == 1);
    CHECK(rep[0].feature == "score");
    CHECK(rep[0].severity == DriftSeverity::severe);
    CHECK(rep[0].psi > 0.5);
}

TEST_CASE("MetricRegistry counters") {
    MetricRegistry m;
    m.inc("ok");
    m.inc("ok");
    m.inc("err");
    CHECK(m.count("ok")  == 2);
    CHECK(m.count("err") == 1);
    CHECK(m.snapshot_json().find("\"ok\":2") != std::string::npos);
}
