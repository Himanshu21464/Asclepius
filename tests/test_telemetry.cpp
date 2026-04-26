// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include <doctest/doctest.h>

#include "asclepius/telemetry.hpp"
#include "asclepius/asclepius.hpp"

#include <filesystem>
#include <random>
#include <set>

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

TEST_CASE("MetricRegistry observe() records into a real histogram") {
    MetricRegistry m;
    // Latency-shaped observations.
    m.observe("inference_latency_seconds", 0.0005);  // < 0.001
    m.observe("inference_latency_seconds", 0.003);   // 0.001 < x <= 0.005
    m.observe("inference_latency_seconds", 0.150);   // 0.1 < x <= 0.25
    m.observe("inference_latency_seconds", 1.2);     // 1.0 < x <= 2.5
    m.observe("inference_latency_seconds", 100.0);   // > 5.0, lands in +Inf

    CHECK(m.count("inference_latency_seconds") == 5);

    auto out = m.snapshot_prometheus();
    CHECK(out.find("# TYPE asclepius_inference_latency_seconds histogram") != std::string::npos);
    // Cumulative buckets: at le=0.001 we have 1, at le=0.005 we have 2, ...
    CHECK(out.find("asclepius_inference_latency_seconds_bucket{le=\"0.001\"} 1") != std::string::npos);
    CHECK(out.find("asclepius_inference_latency_seconds_bucket{le=\"0.005\"} 2") != std::string::npos);
    CHECK(out.find("asclepius_inference_latency_seconds_bucket{le=\"0.25\"} 3")  != std::string::npos);
    CHECK(out.find("asclepius_inference_latency_seconds_bucket{le=\"2.5\"} 4")   != std::string::npos);
    CHECK(out.find("asclepius_inference_latency_seconds_bucket{le=\"+Inf\"} 5")  != std::string::npos);
    // _sum and _count present.
    CHECK(out.find("asclepius_inference_latency_seconds_count 5")  != std::string::npos);
    CHECK(out.find("asclepius_inference_latency_seconds_sum ")     != std::string::npos);
}

TEST_CASE("DriftMonitor alert sink fires when severity rises past threshold") {
    DriftMonitor dm;
    std::vector<double> baseline(200, 0.5);
    REQUIRE(dm.register_feature("f", baseline, 0.0, 1.0, 10));

    int    fires       = 0;
    DriftSeverity last = DriftSeverity::none;
    dm.set_alert_sink([&](const DriftReport& r) { ++fires; last = r.severity; },
                      DriftSeverity::moder);

    // Observe a wildly different distribution: 200 values at 0.95.
    // PSI between baseline (clustered at 0.5) and current (clustered at 0.95)
    // should easily cross moder/severe.
    for (int i = 0; i < 200; ++i) REQUIRE(dm.observe("f", 0.95));

    CHECK(fires >= 1);
    CHECK(static_cast<int>(last) >= static_cast<int>(DriftSeverity::moder));
}

TEST_CASE("DriftMonitor alert fires only on rising edge, not every observation") {
    DriftMonitor dm;
    std::vector<double> baseline(200, 0.5);
    REQUIRE(dm.register_feature("f", baseline, 0.0, 1.0, 10));

    int fires = 0;
    dm.set_alert_sink([&](const DriftReport&) { ++fires; },
                      DriftSeverity::moder);

    // Push severity to moder/severe with one batch...
    for (int i = 0; i < 200; ++i) REQUIRE(dm.observe("f", 0.95));
    int after_first = fires;

    // ...continuing to observe at the same severity must not re-fire.
    for (int i = 0; i < 200; ++i) REQUIRE(dm.observe("f", 0.95));
    CHECK(fires == after_first);
}

TEST_CASE("Runtime::open wires drift.crossed events into the ledger") {
    auto p = std::filesystem::temp_directory_path()
           / ("asc_drift_bridge_" + std::to_string(std::random_device{}()) + ".db");
    std::filesystem::remove(p);
    std::filesystem::remove(std::filesystem::path{p}.replace_extension(".key"));

    auto rt = Runtime::open(p);
    REQUIRE(rt);
    std::vector<double> baseline(200, 0.5);
    REQUIRE(rt.value().drift().register_feature("f", baseline, 0.0, 1.0, 10));

    std::uint64_t pre = rt.value().ledger().length();
    for (int i = 0; i < 200; ++i) REQUIRE(rt.value().drift().observe("f", 0.95));

    // The bridge should have appended at least one drift.crossed event.
    auto after = rt.value().ledger().length();
    CHECK(after > pre);
    auto tail = rt.value().ledger().tail(after - pre);
    REQUIRE(tail);
    bool found = false;
    for (const auto& e : tail.value()) {
        if (e.header.event_type == "drift.crossed") { found = true; break; }
    }
    CHECK(found);
}

TEST_CASE("MetricRegistry counters and histograms coexist in Prometheus output") {
    MetricRegistry m;
    m.inc("appends_total", 10);
    m.observe("append_latency_seconds", 0.05);
    m.observe("append_latency_seconds", 0.07);

    auto out = m.snapshot_prometheus();
    CHECK(out.find("# TYPE asclepius_appends_total counter")             != std::string::npos);
    CHECK(out.find("# TYPE asclepius_append_latency_seconds histogram") != std::string::npos);
    CHECK(out.find("asclepius_appends_total 10")                          != std::string::npos);
    CHECK(out.find("asclepius_append_latency_seconds_count 2")            != std::string::npos);
}

// ============== list_features / list_counters ===========================

TEST_CASE("DriftMonitor::list_features enumerates registered features") {
    DriftMonitor dm;
    REQUIRE(dm.register_feature("a", {0.1, 0.2}, 0.0, 1.0, 4));
    REQUIRE(dm.register_feature("b", {0.3, 0.4}, 0.0, 1.0, 4));
    REQUIRE(dm.register_feature("c", {0.5}, 0.0, 1.0, 4));
    auto names = dm.list_features();
    CHECK(names.size() == 3);
    std::set<std::string> s(names.begin(), names.end());
    CHECK(s.count("a") == 1);
    CHECK(s.count("b") == 1);
    CHECK(s.count("c") == 1);
}

TEST_CASE("DriftMonitor::feature_count matches list_features().size()") {
    DriftMonitor dm;
    CHECK(dm.feature_count() == 0);
    REQUIRE(dm.register_feature("x", {0.1}, 0.0, 1.0, 4));
    CHECK(dm.feature_count() == 1);
    CHECK(dm.list_features().size() == 1);
}

TEST_CASE("MetricRegistry::list_counters enumerates incremented counters") {
    MetricRegistry m;
    m.inc("alpha");
    m.inc("beta", 3);
    m.inc("gamma", 7);
    auto names = m.list_counters();
    std::set<std::string> s(names.begin(), names.end());
    CHECK(s.count("alpha") == 1);
    CHECK(s.count("beta")  == 1);
    CHECK(s.count("gamma") == 1);
}

TEST_CASE("MetricRegistry::list_counters: empty registry returns empty vector") {
    MetricRegistry m;
    CHECK(m.list_counters().empty());
}

TEST_CASE("DriftMonitor::reset clears one feature's window") {
    DriftMonitor dm;
    REQUIRE(dm.register_feature("a", {0.1, 0.2}, 0.0, 1.0, 4));
    REQUIRE(dm.register_feature("b", {0.1, 0.2}, 0.0, 1.0, 4));
    for (int i = 0; i < 50; i++) {
        REQUIRE(dm.observe("a", 0.5));
        REQUIRE(dm.observe("b", 0.5));
    }
    REQUIRE(dm.reset("a"));
    auto rep = dm.report();
    for (const auto& r : rep) {
        if (r.feature == "a") CHECK(r.current_n == 0);
        if (r.feature == "b") CHECK(r.current_n == 50);
    }
}

TEST_CASE("DriftMonitor::reset on unknown feature returns not_found") {
    DriftMonitor dm;
    auto r = dm.reset("ghost");
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::not_found);
}

TEST_CASE("MetricRegistry::reset zeroes one counter only") {
    MetricRegistry m;
    m.inc("a", 5);
    m.inc("b", 10);
    REQUIRE(m.reset("a"));
    CHECK(m.count("a") == 0);
    CHECK(m.count("b") == 10);
}

TEST_CASE("MetricRegistry::reset on unknown counter returns not_found") {
    MetricRegistry m;
    auto r = m.reset("ghost");
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::not_found);
}

TEST_CASE("MetricRegistry::counter_snapshot returns full map") {
    MetricRegistry m;
    m.inc("a", 3);
    m.inc("b", 5);
    m.inc("a", 1);
    auto snap = m.counter_snapshot();
    CHECK(snap.at("a") == 4);
    CHECK(snap.at("b") == 5);
    CHECK(snap.size() == 2);
}
