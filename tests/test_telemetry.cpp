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

TEST_CASE("MetricRegistry Prometheus exposition format") {
    MetricRegistry m;
    m.inc("inferences_total", 42);
    m.inc("policy_violations_total");
    m.inc("policy_violations_total");
    auto out = m.snapshot_prometheus();

    // Prefix all metrics with "asclepius_" so they're identifiable.
    CHECK(out.find("asclepius_inferences_total") != std::string::npos);
    CHECK(out.find("asclepius_policy_violations_total") != std::string::npos);
    // HELP + TYPE per metric.
    CHECK(out.find("# HELP ") != std::string::npos);
    CHECK(out.find("# TYPE asclepius_inferences_total counter") != std::string::npos);
    // Sample line ending in the value.
    CHECK(out.find("asclepius_inferences_total 42") != std::string::npos);
    CHECK(out.find("asclepius_policy_violations_total 2") != std::string::npos);
}

TEST_CASE("MetricRegistry sanitizes invalid metric name characters") {
    MetricRegistry m;
    m.inc("invalid-name.with-stuff", 7);
    auto out = m.snapshot_prometheus();
    // Hyphens and dots get replaced with underscores per Prometheus rules.
    CHECK(out.find("asclepius_invalid_name_with_stuff 7") != std::string::npos);
}

TEST_CASE("MetricRegistry Prometheus output is deterministic (sorted by key)") {
    MetricRegistry m;
    m.inc("z_last");
    m.inc("a_first");
    m.inc("m_middle");
    auto out = m.snapshot_prometheus();
    auto a = out.find("a_first");
    auto mid = out.find("m_middle");
    auto z = out.find("z_last");
    REQUIRE(a != std::string::npos);
    REQUIRE(mid != std::string::npos);
    REQUIRE(z != std::string::npos);
    CHECK(a < mid);
    CHECK(mid < z);
}
