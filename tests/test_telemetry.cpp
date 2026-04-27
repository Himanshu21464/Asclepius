// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include <doctest/doctest.h>

#include "asclepius/telemetry.hpp"
#include "asclepius/asclepius.hpp"

#include <filesystem>
#include <random>
#include <set>
#include <utility>

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

// ============== Histogram::quantile =====================================

TEST_CASE("Histogram::quantile returns median for symmetric uniform fill") {
    Histogram h{0.0, 1.0, 10};
    // 100 evenly-spaced observations across [0, 1).
    for (int i = 0; i < 100; ++i) h.observe(static_cast<double>(i) / 100.0);
    // Median of a uniform distribution on [0, 1] is ~0.5.
    CHECK(h.quantile(0.5) == doctest::Approx(0.5).epsilon(0.05));
    // Tails return the configured edges.
    CHECK(h.quantile(0.0) == doctest::Approx(0.0));
    CHECK(h.quantile(1.0) == doctest::Approx(1.0));
}

TEST_CASE("Histogram::quantile clamps out-of-range q and handles empty") {
    Histogram empty{0.0, 1.0, 10};
    // total()==0 short-circuits to 0.0.
    CHECK(empty.quantile(0.5) == doctest::Approx(0.0));
    CHECK(empty.quantile(0.99) == doctest::Approx(0.0));

    Histogram h{0.0, 1.0, 10};
    for (int i = 0; i < 50; ++i) h.observe(0.45);
    // q < 0 clamps to 0 → lo; q > 1 clamps to 1 → hi.
    CHECK(h.quantile(-0.5) == doctest::Approx(0.0));
    CHECK(h.quantile(1.5)  == doctest::Approx(1.0));
}

TEST_CASE("Histogram::quantile interpolates inside the matching bin") {
    Histogram h{0.0, 10.0, 10};
    // 100 observations all in bin 4 ([4.0, 5.0)).
    for (int i = 0; i < 100; ++i) h.observe(4.5);
    // Every quantile in (0, 1) should land inside [4.0, 5.0].
    for (double q : {0.1, 0.25, 0.5, 0.75, 0.9}) {
        auto v = h.quantile(q);
        CHECK(v >= 4.0);
        CHECK(v <= 5.0);
    }
    // Multi-bin integration: 10/10/10/... per bin → q=0.3 should sit
    // around bin index 3.
    Histogram g{0.0, 10.0, 10};
    for (int b = 0; b < 10; ++b) {
        for (int i = 0; i < 10; ++i) g.observe(static_cast<double>(b) + 0.5);
    }
    auto v3 = g.quantile(0.3);
    CHECK(v3 >= 2.5);
    CHECK(v3 <= 3.5);
}

// ============== MetricRegistry::histogram_count / histogram_sum =========

TEST_CASE("MetricRegistry::histogram_count returns count for known histogram") {
    MetricRegistry m;
    m.observe("latency", 0.1);
    m.observe("latency", 0.2);
    m.observe("latency", 0.3);
    auto c = m.histogram_count("latency");
    REQUIRE(c);
    CHECK(c.value() == 3);
}

TEST_CASE("MetricRegistry::histogram_count and histogram_sum: not_found on unknown") {
    MetricRegistry m;
    auto c = m.histogram_count("ghost");
    CHECK(!c);
    CHECK(c.error().code() == ErrorCode::not_found);

    auto s = m.histogram_sum("ghost");
    CHECK(!s);
    CHECK(s.error().code() == ErrorCode::not_found);

    // Counters are not histograms — incrementing a counter must not make
    // histogram_count() find it.
    m.inc("counter_only", 5);
    auto c2 = m.histogram_count("counter_only");
    CHECK(!c2);
    CHECK(c2.error().code() == ErrorCode::not_found);
}

TEST_CASE("MetricRegistry::histogram_sum tracks running sum across calls") {
    MetricRegistry m;
    m.observe("svc.latency", 0.10);
    m.observe("svc.latency", 0.25);
    m.observe("svc.latency", 0.65);

    auto s = m.histogram_sum("svc.latency");
    REQUIRE(s);
    CHECK(s.value() == doctest::Approx(1.0).epsilon(1e-9));

    auto c = m.histogram_count("svc.latency");
    REQUIRE(c);
    CHECK(c.value() == 3);

    // Adding another observation updates both.
    m.observe("svc.latency", 0.5);
    auto s2 = m.histogram_sum("svc.latency");
    auto c2 = m.histogram_count("svc.latency");
    REQUIRE(s2);
    REQUIRE(c2);
    CHECK(s2.value() == doctest::Approx(1.5).epsilon(1e-9));
    CHECK(c2.value() == 4);
}

// ============== DriftMonitor::observation_count =========================

TEST_CASE("DriftMonitor::observation_count returns current-window count") {
    DriftMonitor dm;
    REQUIRE(dm.register_feature("score", {0.1, 0.2, 0.3}, 0.0, 1.0, 10));
    auto c0 = dm.observation_count("score");
    REQUIRE(c0);
    CHECK(c0.value() == 0);

    for (int i = 0; i < 17; ++i) REQUIRE(dm.observe("score", 0.4));
    auto c1 = dm.observation_count("score");
    REQUIRE(c1);
    CHECK(c1.value() == 17);
}

TEST_CASE("DriftMonitor::observation_count: not_found on unknown feature") {
    DriftMonitor dm;
    auto c = dm.observation_count("ghost");
    CHECK(!c);
    CHECK(c.error().code() == ErrorCode::not_found);
}

// ============== Histogram::mean / stddev ================================

TEST_CASE("Histogram::mean computes weighted bin-midpoint average") {
    Histogram h{0.0, 10.0, 10};
    // 100 observations all in bin 4 ([4.0, 5.0)) → midpoint 4.5.
    for (int i = 0; i < 100; ++i) h.observe(4.5);
    CHECK(h.mean() == doctest::Approx(4.5).epsilon(1e-9));

    // Symmetric uniform fill across [0, 10) → mean ~5.
    Histogram g{0.0, 10.0, 10};
    for (int b = 0; b < 10; ++b) {
        for (int i = 0; i < 10; ++i) g.observe(static_cast<double>(b) + 0.5);
    }
    CHECK(g.mean() == doctest::Approx(5.0).epsilon(1e-9));
}

TEST_CASE("Histogram::mean returns 0.0 on empty histogram") {
    Histogram empty{0.0, 1.0, 10};
    CHECK(empty.mean() == doctest::Approx(0.0));

    // Same for a non-zero range that doesn't include zero — empty must
    // still be 0.0 (the documented sentinel), not lo.
    Histogram off{5.0, 15.0, 10};
    CHECK(off.mean() == doctest::Approx(0.0));
}

TEST_CASE("Histogram::mean reflects skewed distributions") {
    Histogram h{0.0, 1.0, 10};
    // Heavy weight at low end, small tail at high end.
    for (int i = 0; i < 90; ++i) h.observe(0.05);   // bin 0, midpoint 0.05
    for (int i = 0; i < 10; ++i) h.observe(0.95);   // bin 9, midpoint 0.95
    // Expected mean = 0.9 * 0.05 + 0.1 * 0.95 = 0.045 + 0.095 = 0.14.
    CHECK(h.mean() == doctest::Approx(0.14).epsilon(1e-9));
}

TEST_CASE("Histogram::stddev returns 0.0 on empty or single-sample") {
    Histogram empty{0.0, 1.0, 10};
    CHECK(empty.stddev() == doctest::Approx(0.0));

    Histogram one{0.0, 1.0, 10};
    one.observe(0.42);
    CHECK(one.total() == 1);
    CHECK(one.stddev() == doctest::Approx(0.0));
}

TEST_CASE("Histogram::stddev is ~0 for a tightly-clustered distribution") {
    Histogram h{0.0, 10.0, 10};
    // All observations land in the same bin → stddev should be 0
    // (every sample's contribution is identical to the mean).
    for (int i = 0; i < 50; ++i) h.observe(4.5);
    CHECK(h.stddev() == doctest::Approx(0.0));
}

TEST_CASE("Histogram::stddev grows with spread") {
    Histogram tight{0.0, 10.0, 10};
    for (int i = 0; i < 50; ++i) tight.observe(4.5);
    for (int i = 0; i < 50; ++i) tight.observe(5.5);

    Histogram wide{0.0, 10.0, 10};
    for (int i = 0; i < 50; ++i) wide.observe(0.5);
    for (int i = 0; i < 50; ++i) wide.observe(9.5);

    CHECK(wide.stddev() > tight.stddev());
    // Wide is two atoms at midpoints 0.5 and 9.5, mean 5.0 → variance
    // = ((4.5)^2 + (4.5)^2) / 2 = 20.25, stddev = 4.5.
    CHECK(wide.stddev() == doctest::Approx(4.5).epsilon(1e-9));
}

// ============== DriftMonitor::baseline_count ============================

TEST_CASE("DriftMonitor::baseline_count returns reference window total") {
    DriftMonitor dm;
    std::vector<double> baseline = {0.1, 0.2, 0.3, 0.4, 0.5};
    REQUIRE(dm.register_feature("score", baseline, 0.0, 1.0, 10));
    auto c = dm.baseline_count("score");
    REQUIRE(c);
    CHECK(c.value() == 5);
}

TEST_CASE("DriftMonitor::baseline_count: not_found on unknown feature") {
    DriftMonitor dm;
    auto c = dm.baseline_count("ghost");
    CHECK(!c);
    CHECK(c.error().code() == ErrorCode::not_found);
}

TEST_CASE("DriftMonitor::baseline_count is unaffected by observe/reset/rotate") {
    DriftMonitor dm;
    std::vector<double> baseline(200, 0.5);
    REQUIRE(dm.register_feature("f", baseline, 0.0, 1.0, 10));

    auto initial = dm.baseline_count("f");
    REQUIRE(initial);
    CHECK(initial.value() == 200);

    // Observations on the current window must not change baseline_count.
    for (int i = 0; i < 50; ++i) REQUIRE(dm.observe("f", 0.7));
    auto after_obs = dm.baseline_count("f");
    REQUIRE(after_obs);
    CHECK(after_obs.value() == 200);

    // Per-feature reset clears current; baseline still untouched.
    REQUIRE(dm.reset("f"));
    auto after_reset = dm.baseline_count("f");
    REQUIRE(after_reset);
    CHECK(after_reset.value() == 200);

    // rotate() clears current window for all features; baseline unchanged.
    dm.rotate();
    auto after_rotate = dm.baseline_count("f");
    REQUIRE(after_rotate);
    CHECK(after_rotate.value() == 200);

    // And observation_count() correctly reports the current window as 0
    // — confirms baseline/current are independent counts.
    auto cur = dm.observation_count("f");
    REQUIRE(cur);
    CHECK(cur.value() == 0);
}

// ============== MetricRegistry::clear ===================================

TEST_CASE("MetricRegistry::clear drops all counters and histograms") {
    MetricRegistry m;
    m.inc("a", 5);
    m.inc("b", 7);
    m.observe("lat", 0.1);
    m.observe("lat", 0.2);

    REQUIRE(m.counter_count() == 2);
    // histogram_count_total = sum of observations across all histograms.
    // "lat" got 2 observe()s.
    REQUIRE(m.histogram_count_total() == 2);

    m.clear();

    CHECK(m.counter_count() == 0);
    CHECK(m.histogram_count_total() == 0);
    CHECK(m.count("a") == 0);
    CHECK(m.count("b") == 0);
    auto hc = m.histogram_count("lat");
    CHECK(!hc);
    CHECK(hc.error().code() == ErrorCode::not_found);
}

TEST_CASE("MetricRegistry::clear on empty registry is a no-op") {
    MetricRegistry m;
    m.clear();  // must not crash
    CHECK(m.counter_count() == 0);
    CHECK(m.histogram_count_total() == 0);
    // Subsequent operations still work normally after a clear-on-empty.
    m.inc("x");
    CHECK(m.count("x") == 1);
}

TEST_CASE("MetricRegistry::clear leaves the registry usable for new metrics") {
    MetricRegistry m;
    m.inc("old_counter", 9);
    m.observe("old_hist", 1.0);
    m.clear();

    // Re-register fresh metrics. They should not see any residual state.
    m.inc("new_counter", 3);
    m.observe("new_hist", 0.5);
    CHECK(m.count("old_counter") == 0);
    CHECK(m.count("new_counter") == 3);
    auto oh = m.histogram_count("old_hist");
    CHECK(!oh);
    auto nh = m.histogram_count("new_hist");
    REQUIRE(nh);
    CHECK(nh.value() == 1);

    auto out = m.snapshot_prometheus();
    CHECK(out.find("asclepius_old_counter") == std::string::npos);
    CHECK(out.find("asclepius_new_counter 3") != std::string::npos);
}

// ============== MetricRegistry::counter_count / histogram_count_total ===

TEST_CASE("MetricRegistry::counter_count tracks distinct counter names") {
    MetricRegistry m;
    CHECK(m.counter_count() == 0);
    m.inc("alpha");
    CHECK(m.counter_count() == 1);
    m.inc("beta", 4);
    CHECK(m.counter_count() == 2);
    // Re-incrementing an existing counter does NOT grow the registry.
    m.inc("alpha", 10);
    CHECK(m.counter_count() == 2);
}

TEST_CASE("MetricRegistry::histogram_count_total sums observations across histograms") {
    MetricRegistry m;
    CHECK(m.histogram_count_total() == 0);
    m.observe("svc.latency", 0.1);
    // One histogram, one observation → 1.
    CHECK(m.histogram_count_total() == 1);
    m.observe("svc.latency", 0.2);  // same histogram, second obs
    CHECK(m.histogram_count_total() == 2);
    m.observe("queue.depth", 7.0);  // different histogram, one obs
    // 2 ("svc.latency") + 1 ("queue.depth") == 3.
    CHECK(m.histogram_count_total() == 3);
}

TEST_CASE("MetricRegistry counter_count and histogram_count_total are independent") {
    MetricRegistry m;
    m.inc("c1");
    m.inc("c2");
    m.inc("c3");
    m.observe("h1", 1.0);
    m.observe("h2", 2.0);

    CHECK(m.counter_count() == 3);
    // h1 and h2 each got one observation → 2 total observations.
    CHECK(m.histogram_count_total() == 2);

    // After reset() of one counter, counter_count must remain unchanged
    // (reset zeroes the value, doesn't drop the entry). Histograms
    // also untouched, so their observation total stays at 2.
    REQUIRE(m.reset("c1"));
    CHECK(m.counter_count() == 3);
    CHECK(m.histogram_count_total() == 2);

    // After clear(), both go to zero.
    m.clear();
    CHECK(m.counter_count() == 0);
    CHECK(m.histogram_count_total() == 0);
}

// ============== Histogram::min / max ====================================

TEST_CASE("Histogram::min and max return lo/hi sentinels on empty") {
    Histogram h{0.0, 1.0, 10};
    CHECK(h.min() == doctest::Approx(0.0));
    CHECK(h.max() == doctest::Approx(1.0));

    // Off-zero range: empty min == lo, empty max == hi.
    Histogram off{5.0, 15.0, 10};
    CHECK(off.min() == doctest::Approx(5.0));
    CHECK(off.max() == doctest::Approx(15.0));
}

TEST_CASE("Histogram::min and max return bin midpoints when populated") {
    Histogram h{0.0, 10.0, 10};
    // Single bin populated → min == max == that bin's midpoint.
    for (int i = 0; i < 5; ++i) h.observe(4.5);
    CHECK(h.min() == doctest::Approx(4.5).epsilon(1e-9));
    CHECK(h.max() == doctest::Approx(4.5).epsilon(1e-9));

    // Two bins populated at the extremes (bin 0 and bin 9) → min midpoint
    // 0.5, max midpoint 9.5.
    Histogram g{0.0, 10.0, 10};
    g.observe(0.1);
    g.observe(9.9);
    CHECK(g.min() == doctest::Approx(0.5).epsilon(1e-9));
    CHECK(g.max() == doctest::Approx(9.5).epsilon(1e-9));
}

TEST_CASE("Histogram::min and max ignore empty interior bins") {
    Histogram h{0.0, 1.0, 10};
    // Populate only bins 2 and 7 (midpoints 0.25 and 0.75).
    for (int i = 0; i < 3; ++i) h.observe(0.25);
    for (int i = 0; i < 4; ++i) h.observe(0.75);
    CHECK(h.min() == doctest::Approx(0.25).epsilon(1e-9));
    CHECK(h.max() == doctest::Approx(0.75).epsilon(1e-9));

    // Sanity: min <= mean <= max for any non-empty histogram.
    CHECK(h.min() <= h.mean());
    CHECK(h.mean() <= h.max());
}

// ============== Histogram::merge ========================================

TEST_CASE("Histogram::merge sums counts and totals across compatible histograms") {
    Histogram a{0.0, 1.0, 10};
    Histogram b{0.0, 1.0, 10};
    for (int i = 0; i < 30; ++i) a.observe(0.15);  // bin 1
    for (int i = 0; i < 20; ++i) b.observe(0.85);  // bin 8

    REQUIRE(a.merge(b));
    CHECK(a.total() == 50);
    auto norm = a.normalized();
    CHECK(norm[1] == doctest::Approx(30.0 / 50.0).epsilon(1e-9));
    CHECK(norm[8] == doctest::Approx(20.0 / 50.0).epsilon(1e-9));

    // The source is unchanged.
    CHECK(b.total() == 20);
}

TEST_CASE("Histogram::merge rejects mismatched bins/lo/hi with invalid_argument") {
    Histogram a{0.0, 1.0, 10};
    a.observe(0.5);

    // Different bin count.
    Histogram bins_diff{0.0, 1.0, 8};
    auto r1 = a.merge(bins_diff);
    CHECK(!r1);
    CHECK(r1.error().code() == ErrorCode::invalid_argument);

    // Different lo.
    Histogram lo_diff{0.5, 1.0, 10};
    auto r2 = a.merge(lo_diff);
    CHECK(!r2);
    CHECK(r2.error().code() == ErrorCode::invalid_argument);

    // Different hi.
    Histogram hi_diff{0.0, 2.0, 10};
    auto r3 = a.merge(hi_diff);
    CHECK(!r3);
    CHECK(r3.error().code() == ErrorCode::invalid_argument);

    // The destination is untouched after failed merges.
    CHECK(a.total() == 1);
}

TEST_CASE("Histogram::merge integrates with stats and PSI") {
    Histogram global{0.0, 10.0, 10};
    Histogram shard1{0.0, 10.0, 10};
    Histogram shard2{0.0, 10.0, 10};
    for (int i = 0; i < 50; ++i) shard1.observe(2.5);
    for (int i = 0; i < 50; ++i) shard2.observe(7.5);

    REQUIRE(global.merge(shard1));
    REQUIRE(global.merge(shard2));
    CHECK(global.total() == 100);
    // Mean of two equal-mass atoms at 2.5 and 7.5 → 5.0.
    CHECK(global.mean() == doctest::Approx(5.0).epsilon(1e-9));
    // min/max reflect both bins now contain mass.
    CHECK(global.min() == doctest::Approx(2.5).epsilon(1e-9));
    CHECK(global.max() == doctest::Approx(7.5).epsilon(1e-9));

    // Merging into a histogram identical to the reference yields PSI ~ 0.
    Histogram ref{0.0, 10.0, 10};
    for (int i = 0; i < 50; ++i) ref.observe(2.5);
    for (int i = 0; i < 50; ++i) ref.observe(7.5);
    CHECK(Histogram::psi(ref, global) < 0.05);
}

// ============== DriftMonitor::has_feature ===============================

TEST_CASE("DriftMonitor::has_feature returns false on empty monitor") {
    DriftMonitor dm;
    CHECK(dm.has_feature("anything") == false);
    CHECK(dm.has_feature("") == false);
}

TEST_CASE("DriftMonitor::has_feature returns true for registered, false for ghosts") {
    DriftMonitor dm;
    REQUIRE(dm.register_feature("score", {0.1, 0.2}, 0.0, 1.0, 4));
    REQUIRE(dm.register_feature("latency", {0.5}, 0.0, 1.0, 4));
    CHECK(dm.has_feature("score"));
    CHECK(dm.has_feature("latency"));
    CHECK(!dm.has_feature("ghost"));
    // string_view-y access (substring shouldn't false-positive).
    CHECK(!dm.has_feature("scor"));
}

TEST_CASE("DriftMonitor::has_feature stays consistent across observe/reset/rotate") {
    DriftMonitor dm;
    REQUIRE(dm.register_feature("f", {0.1, 0.2}, 0.0, 1.0, 4));
    CHECK(dm.has_feature("f"));

    // observe() doesn't drop the feature.
    REQUIRE(dm.observe("f", 0.5));
    CHECK(dm.has_feature("f"));

    // Per-feature reset() doesn't unregister.
    REQUIRE(dm.reset("f"));
    CHECK(dm.has_feature("f"));

    // rotate() doesn't unregister either.
    dm.rotate();
    CHECK(dm.has_feature("f"));

    // And feature_count() agrees.
    CHECK(dm.feature_count() == 1);
}

// ============== MetricRegistry::reset_histograms ========================

TEST_CASE("MetricRegistry::reset_histograms drops histograms but keeps counters") {
    MetricRegistry m;
    m.inc("inferences_total", 42);
    m.inc("policy_violations_total", 3);
    m.observe("inference_latency_seconds", 0.05);
    m.observe("inference_latency_seconds", 0.10);

    REQUIRE(m.counter_count() == 2);
    // 2 observations of "inference_latency_seconds" → sum-of-obs == 2.
    REQUIRE(m.histogram_count_total() == 2);

    m.reset_histograms();

    // Counters are intact (values and entry count).
    CHECK(m.counter_count() == 2);
    CHECK(m.count("inferences_total") == 42);
    CHECK(m.count("policy_violations_total") == 3);

    // Histograms are gone — sum of observations is 0.
    CHECK(m.histogram_count_total() == 0);
    auto hc = m.histogram_count("inference_latency_seconds");
    CHECK(!hc);
    CHECK(hc.error().code() == ErrorCode::not_found);
}

TEST_CASE("MetricRegistry::reset_histograms on empty registry is a no-op") {
    MetricRegistry m;
    m.reset_histograms();
    CHECK(m.counter_count() == 0);
    CHECK(m.histogram_count_total() == 0);

    // Counters added after a no-op reset still work.
    m.inc("x", 5);
    m.reset_histograms();  // still no histograms; counter must survive.
    CHECK(m.count("x") == 5);
    CHECK(m.histogram_count_total() == 0);
}

TEST_CASE("MetricRegistry::reset_histograms leaves the registry usable for new histograms") {
    MetricRegistry m;
    m.inc("c", 7);
    m.observe("h_old", 0.1);
    m.observe("h_old", 0.2);

    m.reset_histograms();

    // Fresh observe() into a new (or same-named) histogram starts from
    // empty state.
    m.observe("h_new", 0.3);
    auto cn = m.histogram_count("h_new");
    REQUIRE(cn);
    CHECK(cn.value() == 1);

    // Re-observing under the old name creates a new histogram with no
    // residual count.
    m.observe("h_old", 0.5);
    auto co = m.histogram_count("h_old");
    REQUIRE(co);
    CHECK(co.value() == 1);

    // Counter survived untouched throughout.
    CHECK(m.count("c") == 7);

    // Prometheus output reflects the reset state.
    auto out = m.snapshot_prometheus();
    CHECK(out.find("asclepius_c 7") != std::string::npos);
    CHECK(out.find("asclepius_h_old_count 1") != std::string::npos);
    CHECK(out.find("asclepius_h_new_count 1") != std::string::npos);
}

// ============== Histogram::percentile ===================================

TEST_CASE("Histogram::percentile mirrors quantile with p/100 conversion") {
    Histogram h{0.0, 1.0, 10};
    for (int i = 0; i < 100; ++i) h.observe(static_cast<double>(i) / 100.0);
    // p=50 == median; p=0 -> lo; p=100 -> hi.
    CHECK(h.percentile(50.0) == doctest::Approx(h.quantile(0.5)));
    CHECK(h.percentile(0.0)  == doctest::Approx(0.0));
    CHECK(h.percentile(100.0) == doctest::Approx(1.0));
    CHECK(h.percentile(50.0) == doctest::Approx(0.5).epsilon(0.05));
}

TEST_CASE("Histogram::percentile clamps out-of-range and handles empty") {
    // Empty histogram returns 0.0 for any p.
    Histogram empty{0.0, 1.0, 10};
    CHECK(empty.percentile(50.0) == doctest::Approx(0.0));
    CHECK(empty.percentile(99.9) == doctest::Approx(0.0));

    Histogram h{0.0, 1.0, 10};
    for (int i = 0; i < 50; ++i) h.observe(0.45);
    // Values outside [0, 100] clamp.
    CHECK(h.percentile(-10.0)  == doctest::Approx(h.quantile(0.0)));
    CHECK(h.percentile(150.0)  == doctest::Approx(h.quantile(1.0)));
    // p=100 maps to hi.
    CHECK(h.percentile(100.0)  == doctest::Approx(1.0));
}

TEST_CASE("Histogram::percentile p95 lands in upper tail (integration)") {
    Histogram h{0.0, 10.0, 10};
    // Latency-shaped: 90 fast, 10 slow.
    for (int i = 0; i < 90; ++i) h.observe(0.5);  // bin 0
    for (int i = 0; i < 10; ++i) h.observe(9.5);  // bin 9
    auto p50 = h.percentile(50.0);
    auto p95 = h.percentile(95.0);
    auto p99 = h.percentile(99.0);
    // p50 must lie below p95 below p99.
    CHECK(p50 <= p95);
    CHECK(p95 <= p99);
    // The slow tail kicks in at the 90th percentile, so p95 must be in
    // the upper bin.
    CHECK(p95 >= 9.0);
    CHECK(p99 <= 10.0);
}

// ============== Histogram::cdf ==========================================

TEST_CASE("Histogram::cdf returns all-zeros for empty histograms") {
    Histogram h{0.0, 1.0, 10};
    auto c = h.cdf();
    REQUIRE(c.size() == h.bin_count());
    for (double v : c) CHECK(v == doctest::Approx(0.0));
}

TEST_CASE("Histogram::cdf is monotonic and ends at 1.0 when populated") {
    Histogram h{0.0, 10.0, 10};
    for (int b = 0; b < 10; ++b) {
        for (int i = 0; i < 10; ++i) h.observe(static_cast<double>(b) + 0.5);
    }
    auto c = h.cdf();
    REQUIRE(c.size() == 10);
    // Even fill -> 0.1, 0.2, ... 1.0.
    for (std::size_t i = 0; i < c.size(); ++i) {
        CHECK(c[i] == doctest::Approx(static_cast<double>(i + 1) / 10.0).epsilon(1e-9));
    }
    // Strictly non-decreasing and ends at 1.0.
    for (std::size_t i = 1; i < c.size(); ++i) {
        CHECK(c[i] >= c[i - 1]);
    }
    CHECK(c.back() == doctest::Approx(1.0).epsilon(1e-9));
}

TEST_CASE("Histogram::cdf integrates with skewed distributions") {
    Histogram h{0.0, 1.0, 10};
    // 90% in bin 0, 10% in bin 9 (the heavy-low / light-high case).
    for (int i = 0; i < 90; ++i) h.observe(0.05);
    for (int i = 0; i < 10; ++i) h.observe(0.95);

    auto c = h.cdf();
    REQUIRE(c.size() == 10);
    // Bin 0 already accounts for 0.9.
    CHECK(c[0] == doctest::Approx(0.9).epsilon(1e-9));
    // Empty interior bins keep cdf flat.
    for (std::size_t i = 1; i < 9; ++i) {
        CHECK(c[i] == doctest::Approx(0.9).epsilon(1e-9));
    }
    // Last bin closes the distribution.
    CHECK(c[9] == doctest::Approx(1.0).epsilon(1e-9));
}

// ============== DriftMonitor::summary ===================================

TEST_CASE("DriftMonitor::summary on empty monitor returns zeros and severity::none") {
    DriftMonitor dm;
    auto s = dm.summary();
    CHECK(s.feature_count == 0);
    CHECK(s.total_observations == 0);
    CHECK(s.max_severity == DriftSeverity::none);
}

TEST_CASE("DriftMonitor::summary aggregates observation counts across features") {
    DriftMonitor dm;
    REQUIRE(dm.register_feature("a", {0.1}, 0.0, 1.0, 10));
    REQUIRE(dm.register_feature("b", {0.1}, 0.0, 1.0, 10));
    for (int i = 0; i < 10; ++i) REQUIRE(dm.observe("a", 0.5));
    for (int i = 0; i < 7;  ++i) REQUIRE(dm.observe("b", 0.5));

    auto s = dm.summary();
    CHECK(s.feature_count == 2);
    CHECK(s.total_observations == 17);
    // Without strong drift these distributions still register some PSI;
    // we only assert that the severity field is well-formed (not nan-cast).
    CHECK(static_cast<int>(s.max_severity) >= static_cast<int>(DriftSeverity::none));
    CHECK(static_cast<int>(s.max_severity) <= static_cast<int>(DriftSeverity::severe));
}

TEST_CASE("DriftMonitor::summary picks worst severity across features") {
    DriftMonitor dm;
    // Two features: one stable, one wildly drifted.
    std::vector<double> baseline_stable(200, 0.5);
    std::vector<double> baseline_drift (200, 0.5);
    REQUIRE(dm.register_feature("stable", baseline_stable, 0.0, 1.0, 10));
    REQUIRE(dm.register_feature("drift",  baseline_drift,  0.0, 1.0, 10));

    // Stable: continue feeding around 0.5.
    for (int i = 0; i < 200; ++i) REQUIRE(dm.observe("stable", 0.5));
    // Drift: feed 0.95, very different from baseline at 0.5.
    for (int i = 0; i < 200; ++i) REQUIRE(dm.observe("drift", 0.95));

    auto s = dm.summary();
    CHECK(s.feature_count == 2);
    CHECK(s.total_observations == 400);
    // max_severity must be at least moder for the drifted feature.
    CHECK(static_cast<int>(s.max_severity) >= static_cast<int>(DriftSeverity::moder));
}

// ============== MetricRegistry::all_counter_names =======================

TEST_CASE("MetricRegistry::all_counter_names returns sorted list") {
    MetricRegistry m;
    m.inc("zeta");
    m.inc("alpha");
    m.inc("mu");
    m.inc("beta");
    auto names = m.all_counter_names();
    REQUIRE(names.size() == 4);
    CHECK(names[0] == "alpha");
    CHECK(names[1] == "beta");
    CHECK(names[2] == "mu");
    CHECK(names[3] == "zeta");
}

TEST_CASE("MetricRegistry::all_counter_names on empty registry returns empty vector") {
    MetricRegistry m;
    CHECK(m.all_counter_names().empty());

    // Histograms must not show up here — only counters.
    m.observe("latency", 0.1);
    CHECK(m.all_counter_names().empty());
}

TEST_CASE("MetricRegistry::all_counter_names is stable across re-increments") {
    MetricRegistry m;
    m.inc("c", 1);
    m.inc("a", 1);
    m.inc("b", 1);
    auto first = m.all_counter_names();

    // Re-incrementing existing counters must not change the name set or
    // its order.
    m.inc("a", 9);
    m.inc("b", 9);
    m.inc("c", 9);
    auto second = m.all_counter_names();
    REQUIRE(second.size() == first.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        CHECK(first[i] == second[i]);
    }
    // And the same set as list_counters() (just sorted).
    auto unsorted = m.list_counters();
    std::set<std::string> a(unsorted.begin(), unsorted.end());
    std::set<std::string> b(second.begin(), second.end());
    CHECK(a == b);
}

// ============== MetricRegistry::add =====================================

TEST_CASE("MetricRegistry::add behaves identically to inc()") {
    MetricRegistry m;
    m.add("widgets", 5);
    m.add("widgets", 3);
    CHECK(m.count("widgets") == 8);

    // Mixing add() and inc() on the same counter accumulates.
    m.inc("widgets", 2);
    m.add("widgets", 1);
    CHECK(m.count("widgets") == 11);
}

TEST_CASE("MetricRegistry::add registers a new counter with delta") {
    MetricRegistry m;
    REQUIRE(m.counter_count() == 0);
    m.add("new_counter", 42);
    CHECK(m.counter_count() == 1);
    CHECK(m.count("new_counter") == 42);
    // And it shows up in the sorted list.
    auto names = m.all_counter_names();
    REQUIRE(names.size() == 1);
    CHECK(names[0] == "new_counter");
}

TEST_CASE("MetricRegistry::add integrates with prometheus exposition") {
    MetricRegistry m;
    m.add("inferences_total", 100);
    m.add("inferences_total", 50);
    m.inc("policy_violations_total", 2);

    auto out = m.snapshot_prometheus();
    CHECK(out.find("asclepius_inferences_total 150")        != std::string::npos);
    CHECK(out.find("asclepius_policy_violations_total 2")   != std::string::npos);
    // Type line emitted normally.
    CHECK(out.find("# TYPE asclepius_inferences_total counter") != std::string::npos);
}

// ============== Histogram::variance =====================================

TEST_CASE("Histogram::variance returns 0.0 on empty or single-sample") {
    Histogram empty{0.0, 1.0, 10};
    CHECK(empty.variance() == doctest::Approx(0.0));

    Histogram one{0.0, 1.0, 10};
    one.observe(0.42);
    CHECK(one.total() == 1);
    CHECK(one.variance() == doctest::Approx(0.0));
}

TEST_CASE("Histogram::variance equals stddev squared (basic)") {
    Histogram h{0.0, 10.0, 10};
    // Two atoms at midpoints 0.5 and 9.5 → variance ((4.5)^2+(4.5)^2)/2 = 20.25.
    for (int i = 0; i < 50; ++i) h.observe(0.5);
    for (int i = 0; i < 50; ++i) h.observe(9.5);
    CHECK(h.variance() == doctest::Approx(20.25).epsilon(1e-9));
    CHECK(h.variance() == doctest::Approx(h.stddev() * h.stddev()).epsilon(1e-9));
}

TEST_CASE("Histogram::variance is zero for tightly-clustered, integrates with stddev") {
    Histogram tight{0.0, 10.0, 10};
    for (int i = 0; i < 50; ++i) tight.observe(4.5);
    CHECK(tight.variance() == doctest::Approx(0.0));

    // Across a range of distributions, variance == stddev^2 should always hold.
    Histogram a{0.0, 1.0, 10};
    for (int i = 0; i < 90; ++i) a.observe(0.05);
    for (int i = 0; i < 10; ++i) a.observe(0.95);
    const double sd = a.stddev();
    CHECK(a.variance() == doctest::Approx(sd * sd).epsilon(1e-9));
    CHECK(a.variance() > 0.0);
}

// ============== Histogram::iqr ==========================================

TEST_CASE("Histogram::iqr returns 0.0 on empty histogram") {
    Histogram empty{0.0, 1.0, 10};
    CHECK(empty.iqr() == doctest::Approx(0.0));

    Histogram off{5.0, 15.0, 10};
    CHECK(off.iqr() == doctest::Approx(0.0));
}

TEST_CASE("Histogram::iqr matches quantile(0.75) - quantile(0.25)") {
    Histogram h{0.0, 1.0, 10};
    for (int i = 0; i < 100; ++i) h.observe(static_cast<double>(i) / 100.0);
    const double q25 = h.quantile(0.25);
    const double q75 = h.quantile(0.75);
    CHECK(h.iqr() == doctest::Approx(q75 - q25).epsilon(1e-9));
    // For a roughly-uniform fill on [0, 1], IQR should be ~0.5.
    CHECK(h.iqr() == doctest::Approx(0.5).epsilon(0.1));
}

TEST_CASE("Histogram::iqr is zero for a single-bin distribution (integration)") {
    Histogram tight{0.0, 10.0, 10};
    // All mass in one bin: q25 and q75 both interpolate inside the same bin.
    for (int i = 0; i < 100; ++i) tight.observe(4.5);
    // Both quartiles fall in [4.0, 5.0] so IQR is bounded by the bin width.
    CHECK(tight.iqr() >= 0.0);
    CHECK(tight.iqr() <= 1.0);

    // Wider spread → larger IQR.
    Histogram wide{0.0, 10.0, 10};
    for (int i = 0; i < 50; ++i) wide.observe(0.5);
    for (int i = 0; i < 50; ++i) wide.observe(9.5);
    CHECK(wide.iqr() > tight.iqr());
}

// ============== Histogram::skewness =====================================

TEST_CASE("Histogram::skewness returns 0.0 for empty / single-sample / zero-variance") {
    Histogram empty{0.0, 1.0, 10};
    CHECK(empty.skewness() == doctest::Approx(0.0));

    Histogram one{0.0, 1.0, 10};
    one.observe(0.42);
    CHECK(one.skewness() == doctest::Approx(0.0));

    // All mass in one bin → variance==0 → skewness short-circuits to 0.
    Histogram flat{0.0, 10.0, 10};
    for (int i = 0; i < 50; ++i) flat.observe(4.5);
    CHECK(flat.skewness() == doctest::Approx(0.0));
}

TEST_CASE("Histogram::skewness is ~0 for symmetric distributions") {
    Histogram h{0.0, 10.0, 10};
    // Symmetric two-atom distribution at midpoints 0.5 and 9.5 with equal mass.
    for (int i = 0; i < 50; ++i) h.observe(0.5);
    for (int i = 0; i < 50; ++i) h.observe(9.5);
    CHECK(h.skewness() == doctest::Approx(0.0).epsilon(1e-9));
}

TEST_CASE("Histogram::skewness sign reflects tail direction (integration)") {
    // Heavy mass at low end, small tail at high end → POSITIVE skewness.
    Histogram right_tailed{0.0, 1.0, 10};
    for (int i = 0; i < 90; ++i) right_tailed.observe(0.05);
    for (int i = 0; i < 10; ++i) right_tailed.observe(0.95);
    CHECK(right_tailed.skewness() > 0.0);

    // Heavy mass at high end, small tail at low end → NEGATIVE skewness.
    Histogram left_tailed{0.0, 1.0, 10};
    for (int i = 0; i < 10; ++i) left_tailed.observe(0.05);
    for (int i = 0; i < 90; ++i) left_tailed.observe(0.95);
    CHECK(left_tailed.skewness() < 0.0);

    // Mirror-image distributions should have skewness of equal magnitude
    // but opposite sign.
    CHECK(right_tailed.skewness() == doctest::Approx(-left_tailed.skewness()).epsilon(1e-9));
}

// ============== DriftMonitor::any_severe ================================

TEST_CASE("DriftMonitor::any_severe returns false on empty monitor") {
    DriftMonitor dm;
    CHECK(dm.any_severe() == false);
}

TEST_CASE("DriftMonitor::any_severe returns false when all features are stable") {
    DriftMonitor dm;
    std::vector<double> baseline(200, 0.5);
    REQUIRE(dm.register_feature("a", baseline, 0.0, 1.0, 10));
    REQUIRE(dm.register_feature("b", baseline, 0.0, 1.0, 10));
    // Feed values matching the baseline distribution.
    for (int i = 0; i < 200; ++i) {
        REQUIRE(dm.observe("a", 0.5));
        REQUIRE(dm.observe("b", 0.5));
    }
    CHECK(dm.any_severe() == false);
}

TEST_CASE("DriftMonitor::any_severe true if at least one feature is severe (integration)") {
    DriftMonitor dm;
    std::vector<double> baseline(200, 0.5);
    REQUIRE(dm.register_feature("stable", baseline, 0.0, 1.0, 10));
    REQUIRE(dm.register_feature("drift",  baseline, 0.0, 1.0, 10));

    // Stable mirrors the baseline.
    for (int i = 0; i < 200; ++i) REQUIRE(dm.observe("stable", 0.5));
    // Drift wildly diverges → severe PSI.
    for (int i = 0; i < 200; ++i) REQUIRE(dm.observe("drift", 0.95));

    CHECK(dm.any_severe() == true);

    // Cross-check against summary().max_severity for consistency.
    auto s = dm.summary();
    CHECK(s.max_severity == DriftSeverity::severe);

    // After resetting the offending feature, the current window is empty
    // against a populated baseline. PSI computed against an empty current
    // bins to ~13.8 per occupied bin (eps clamping in psi()) — well above
    // 0.5, so the feature still classifies as severe. This is intentional:
    // an empty current window is itself a degenerate signal.
    REQUIRE(dm.reset("drift"));
    CHECK(dm.any_severe() == true);
}

// ============== MetricRegistry::diff ====================================

TEST_CASE("MetricRegistry::diff: simple positive deltas against a snapshot") {
    MetricRegistry m;
    m.inc("a", 5);
    m.inc("b", 10);
    auto snap = m.counter_snapshot();

    // Move forward.
    m.inc("a", 2);
    m.inc("b", 3);
    auto d = m.diff(snap);
    CHECK(d.at("a") == 2);
    CHECK(d.at("b") == 3);
    CHECK(d.size() == 2);
}

TEST_CASE("MetricRegistry::diff: missing baseline entries treated as 0; new counters surface in full") {
    MetricRegistry m;
    // Empty baseline: every current counter shows up as its full value.
    m.inc("fresh", 7);
    m.inc("also_fresh", 4);
    auto d = m.diff({});
    CHECK(d.at("fresh") == 7);
    CHECK(d.at("also_fresh") == 4);
    CHECK(d.size() == 2);

    // Partial baseline: counters absent from baseline contribute their
    // full current value as the delta.
    std::unordered_map<std::string, std::uint64_t> partial = {{"fresh", 3}};
    auto d2 = m.diff(partial);
    CHECK(d2.at("fresh") == 4);        // 7 - 3
    CHECK(d2.at("also_fresh") == 4);   // 4 - 0
}

TEST_CASE("MetricRegistry::diff: counters dropped since baseline emit negative deltas (integration)") {
    MetricRegistry m;
    m.inc("survivor", 2);
    m.inc("doomed", 9);
    auto snap = m.counter_snapshot();

    // clear() drops every counter; the diff against the prior snapshot
    // must report -baseline for each.
    m.clear();
    auto d = m.diff(snap);
    CHECK(d.at("survivor") == -2);
    CHECK(d.at("doomed") == -9);
    CHECK(d.size() == 2);

    // Rebuild a counter and re-diff. survivor reappears with +new; doomed
    // is still negative.
    m.inc("survivor", 5);
    auto d2 = m.diff(snap);
    CHECK(d2.at("survivor") == 3);   // 5 - 2
    CHECK(d2.at("doomed") == -9);    // 0 - 9 (still missing)
    CHECK(d2.size() == 2);

    // Diff against itself yields all-zero deltas.
    auto self = m.counter_snapshot();
    auto zero = m.diff(self);
    for (const auto& [_, v] : zero) CHECK(v == 0);
}

// ============== Histogram::sum ==========================================

TEST_CASE("Histogram::sum returns 0.0 on empty histogram") {
    Histogram empty{0.0, 1.0, 10};
    CHECK(empty.sum() == doctest::Approx(0.0));

    // Off-zero range — empty must still return 0.0, not lo or anything
    // derived from the range.
    Histogram off{5.0, 15.0, 10};
    CHECK(off.sum() == doctest::Approx(0.0));
}

TEST_CASE("Histogram::sum equals bin_midpoint*count weighted total") {
    Histogram h{0.0, 1.0, 10};
    // 90 obs at bin 0 (midpoint 0.05) + 10 obs at bin 9 (midpoint 0.95)
    // → sum = 90 * 0.05 + 10 * 0.95 = 4.5 + 9.5 = 14.0.
    for (int i = 0; i < 90; ++i) h.observe(0.05);
    for (int i = 0; i < 10; ++i) h.observe(0.95);
    CHECK(h.sum() == doctest::Approx(14.0).epsilon(1e-9));
}

TEST_CASE("Histogram::sum is mean*total (integration)") {
    Histogram h{0.0, 10.0, 10};
    for (int b = 0; b < 10; ++b) {
        for (int i = 0; i < 10; ++i) h.observe(static_cast<double>(b) + 0.5);
    }
    // mean ~5.0, total 100 → sum ~500.
    CHECK(h.sum() == doctest::Approx(h.mean() * static_cast<double>(h.total()))
                        .epsilon(1e-9));
    CHECK(h.sum() == doctest::Approx(500.0).epsilon(1e-9));

    // Single-bin distribution: sum == midpoint * total.
    Histogram tight{0.0, 10.0, 10};
    for (int i = 0; i < 25; ++i) tight.observe(4.5);
    CHECK(tight.sum() == doctest::Approx(4.5 * 25).epsilon(1e-9));
}

// ============== Histogram::range ========================================

TEST_CASE("Histogram::range returns 0.0 on empty histogram") {
    Histogram empty{0.0, 1.0, 10};
    CHECK(empty.range() == doctest::Approx(0.0));

    // Off-zero range with no observations — must NOT return hi - lo.
    Histogram off{5.0, 15.0, 10};
    CHECK(off.range() == doctest::Approx(0.0));
}

TEST_CASE("Histogram::range is zero for a single populated bin") {
    Histogram h{0.0, 10.0, 10};
    for (int i = 0; i < 5; ++i) h.observe(4.5);
    // min == max == 4.5 → range == 0.
    CHECK(h.range() == doctest::Approx(0.0).epsilon(1e-9));
}

TEST_CASE("Histogram::range matches max() - min() (integration)") {
    Histogram h{0.0, 10.0, 10};
    h.observe(0.1);   // bin 0, midpoint 0.5
    h.observe(9.9);   // bin 9, midpoint 9.5
    // range == 9.5 - 0.5 == 9.0.
    CHECK(h.range() == doctest::Approx(9.0).epsilon(1e-9));
    CHECK(h.range() == doctest::Approx(h.max() - h.min()).epsilon(1e-9));

    // Interior empty bins are correctly ignored — range tracks populated
    // extremes, not lo/hi.
    Histogram g{0.0, 1.0, 10};
    for (int i = 0; i < 3; ++i) g.observe(0.25);  // midpoint 0.25
    for (int i = 0; i < 4; ++i) g.observe(0.75);  // midpoint 0.75
    CHECK(g.range() == doctest::Approx(0.5).epsilon(1e-9));
}

// ============== MetricRegistry::has_counter =============================

TEST_CASE("MetricRegistry::has_counter returns false on empty registry") {
    MetricRegistry m;
    CHECK(m.has_counter("anything") == false);
    CHECK(m.has_counter("") == false);
}

TEST_CASE("MetricRegistry::has_counter returns true for incremented names only") {
    MetricRegistry m;
    m.inc("alpha");
    m.add("beta", 4);
    CHECK(m.has_counter("alpha"));
    CHECK(m.has_counter("beta"));
    CHECK(!m.has_counter("ghost"));
    // Substring should not false-positive.
    CHECK(!m.has_counter("alph"));
    // Histograms must NOT show up here — independent name space.
    m.observe("hist", 0.1);
    CHECK(!m.has_counter("hist"));
}

TEST_CASE("MetricRegistry::has_counter survives reset, cleared by clear() (integration)") {
    MetricRegistry m;
    m.inc("c", 5);
    CHECK(m.has_counter("c"));

    // reset() zeroes the value but keeps the entry — has_counter() stays
    // true (consistent with counter_count() not dropping after reset).
    REQUIRE(m.reset("c"));
    CHECK(m.has_counter("c"));
    CHECK(m.count("c") == 0);

    // clear() drops everything.
    m.clear();
    CHECK(!m.has_counter("c"));
}

// ============== MetricRegistry::has_histogram ===========================

TEST_CASE("MetricRegistry::has_histogram returns false on empty registry") {
    MetricRegistry m;
    CHECK(m.has_histogram("anything") == false);
    CHECK(m.has_histogram("") == false);
}

TEST_CASE("MetricRegistry::has_histogram returns true for observed names only") {
    MetricRegistry m;
    m.observe("latency", 0.1);
    m.observe("queue_depth", 7.0);
    CHECK(m.has_histogram("latency"));
    CHECK(m.has_histogram("queue_depth"));
    CHECK(!m.has_histogram("ghost"));
    // Counters must NOT show up here.
    m.inc("counter_only", 3);
    CHECK(!m.has_histogram("counter_only"));
    // Substring should not false-positive.
    CHECK(!m.has_histogram("laten"));
}

TEST_CASE("MetricRegistry::has_histogram clears with reset_histograms / clear (integration)") {
    MetricRegistry m;
    m.inc("c", 1);
    m.observe("h", 0.5);
    CHECK(m.has_histogram("h"));
    CHECK(m.has_counter("c"));

    // reset_histograms() drops histograms, leaves counters.
    m.reset_histograms();
    CHECK(!m.has_histogram("h"));
    CHECK(m.has_counter("c"));

    // Re-observing re-registers the histogram.
    m.observe("h", 0.2);
    CHECK(m.has_histogram("h"));

    // clear() wipes both.
    m.clear();
    CHECK(!m.has_histogram("h"));
    CHECK(!m.has_counter("c"));
}

// ============== DriftMonitor::most_drifted_feature ======================

TEST_CASE("DriftMonitor::most_drifted_feature: not_found on empty monitor") {
    DriftMonitor dm;
    auto r = dm.most_drifted_feature();
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::not_found);
}

TEST_CASE("DriftMonitor::most_drifted_feature picks the highest-PSI feature") {
    DriftMonitor dm;
    std::vector<double> baseline(200, 0.5);
    REQUIRE(dm.register_feature("stable", baseline, 0.0, 1.0, 10));
    REQUIRE(dm.register_feature("drifted", baseline, 0.0, 1.0, 10));

    // Stable mirrors baseline (low PSI); drifted diverges (high PSI).
    for (int i = 0; i < 200; ++i) REQUIRE(dm.observe("stable", 0.5));
    for (int i = 0; i < 200; ++i) REQUIRE(dm.observe("drifted", 0.95));

    auto r = dm.most_drifted_feature();
    REQUIRE(r);
    CHECK(r.value() == "drifted");
}

TEST_CASE("DriftMonitor::most_drifted_feature breaks ties alphabetically (integration)") {
    DriftMonitor dm;
    std::vector<double> baseline(100, 0.5);
    // Three features registered with identical baselines; never observed
    // → all current windows are empty → all PSIs are bitwise-equal. The
    // tie-break must yield the alphabetically smallest name.
    REQUIRE(dm.register_feature("zebra",  baseline, 0.0, 1.0, 10));
    REQUIRE(dm.register_feature("apple",  baseline, 0.0, 1.0, 10));
    REQUIRE(dm.register_feature("monkey", baseline, 0.0, 1.0, 10));

    auto r = dm.most_drifted_feature();
    REQUIRE(r);
    CHECK(r.value() == "apple");

    // Drift one feature mildly so its PSI is strictly larger; the
    // tie-breaker should now defer to the genuine maximum.
    for (int i = 0; i < 50; ++i) REQUIRE(dm.observe("zebra", 0.05));
    auto r2 = dm.most_drifted_feature();
    REQUIRE(r2);
    CHECK(r2.value() == "zebra");

    // Cross-check: report() must agree on which feature has the largest
    // PSI value.
    auto rep = dm.report();
    double max_psi = -1.0;
    std::string winner;
    for (const auto& d : rep) {
        if (d.psi > max_psi || (d.psi == max_psi && d.feature < winner)) {
            max_psi = d.psi;
            winner  = d.feature;
        }
    }
    CHECK(winner == r2.value());
}

TEST_CASE("DriftMonitor::observation_count resets after reset() and rotate()") {
    DriftMonitor dm;
    REQUIRE(dm.register_feature("a", {0.1}, 0.0, 1.0, 10));
    REQUIRE(dm.register_feature("b", {0.1}, 0.0, 1.0, 10));
    for (int i = 0; i < 25; ++i) {
        REQUIRE(dm.observe("a", 0.5));
        REQUIRE(dm.observe("b", 0.5));
    }
    {
        auto ca = dm.observation_count("a");
        auto cb = dm.observation_count("b");
        REQUIRE(ca); REQUIRE(cb);
        CHECK(ca.value() == 25);
        CHECK(cb.value() == 25);
    }

    // Per-feature reset clears only "a".
    REQUIRE(dm.reset("a"));
    {
        auto ca = dm.observation_count("a");
        auto cb = dm.observation_count("b");
        REQUIRE(ca); REQUIRE(cb);
        CHECK(ca.value() == 0);
        CHECK(cb.value() == 25);
    }

    // rotate() clears all current windows.
    for (int i = 0; i < 5; ++i) REQUIRE(dm.observe("a", 0.5));
    dm.rotate();
    {
        auto ca = dm.observation_count("a");
        auto cb = dm.observation_count("b");
        REQUIRE(ca); REQUIRE(cb);
        CHECK(ca.value() == 0);
        CHECK(cb.value() == 0);
    }
}

// ---- Feature 1: Histogram::median ---------------------------------------

TEST_CASE("Histogram::median basic — symmetric distribution") {
    Histogram h{0.0, 1.0, 10};
    for (int i = 0; i < 100; ++i) h.observe(0.5);
    // All mass in the bin covering 0.5; median lands inside it.
    CHECK(h.median() == doctest::Approx(0.5).epsilon(0.1));
}

TEST_CASE("Histogram::median edge — empty histogram returns 0.0") {
    Histogram h{0.0, 1.0, 10};
    CHECK(h.median() == doctest::Approx(0.0));
}

TEST_CASE("Histogram::median integration — agrees with quantile(0.5)") {
    Histogram h{0.0, 1.0, 20};
    std::mt19937 rng{42};
    std::uniform_real_distribution<double> d{0.0, 1.0};
    for (int i = 0; i < 1000; ++i) h.observe(d(rng));
    CHECK(h.median() == doctest::Approx(h.quantile(0.5)));
    CHECK(h.median() == doctest::Approx(h.percentile(50.0)));
}

// ---- Feature 2: Histogram::is_empty -------------------------------------

TEST_CASE("Histogram::is_empty basic — fresh histogram is empty") {
    Histogram h{0.0, 1.0, 10};
    CHECK(h.is_empty());
}

TEST_CASE("Histogram::is_empty edge — flips to false after a single observe") {
    Histogram h{-5.0, 5.0, 50};
    CHECK(h.is_empty());
    h.observe(0.0);
    CHECK_FALSE(h.is_empty());
    CHECK(h.total() == 1);
}

TEST_CASE("Histogram::is_empty integration — agrees with total() after merge") {
    Histogram a{0.0, 1.0, 10};
    Histogram b{0.0, 1.0, 10};
    CHECK(a.is_empty());
    CHECK(b.is_empty());
    for (int i = 0; i < 7; ++i) b.observe(0.3);
    REQUIRE(a.merge(b));
    CHECK_FALSE(a.is_empty());
    CHECK(a.total() == 7);
    // b is unaffected by merge.
    CHECK_FALSE(b.is_empty());
}

// ---- Feature 3: DriftMonitor::clear_alerts ------------------------------

TEST_CASE("DriftMonitor::clear_alerts basic — no-op on a fresh monitor") {
    DriftMonitor dm;
    // Nothing registered, nothing tracked → calling is harmless.
    dm.clear_alerts();
    CHECK(dm.feature_count() == 0);
}

TEST_CASE("DriftMonitor::clear_alerts edge — re-fires alert sink after ack") {
    DriftMonitor dm;
    std::vector<double> baseline(500, 0.2);
    REQUIRE(dm.register_feature("score", baseline, 0.0, 1.0, 10));

    int fire_count = 0;
    dm.set_alert_sink(
        [&](const DriftReport&) { ++fire_count; },
        DriftSeverity::moder);

    // Drive severity from none → severe; sink fires exactly once on the
    // crossing.
    for (int i = 0; i < 500; ++i) REQUIRE(dm.observe("score", 0.85));
    CHECK(fire_count == 1);

    // Further observations at the same severity must NOT re-fire.
    for (int i = 0; i < 50; ++i) REQUIRE(dm.observe("score", 0.85));
    CHECK(fire_count == 1);

    // Operator acknowledges → clear tracking → next observation that
    // remains at the same severity should re-fire (the recorded
    // "previous" severity has been forgotten, so the rise from none →
    // severe is detected anew).
    dm.clear_alerts();
    REQUIRE(dm.observe("score", 0.85));
    CHECK(fire_count == 2);
}

TEST_CASE("DriftMonitor::clear_alerts integration — preserves observations and reports") {
    DriftMonitor dm;
    std::vector<double> baseline(200, 0.5);
    REQUIRE(dm.register_feature("a", baseline, 0.0, 1.0, 10));
    REQUIRE(dm.register_feature("b", baseline, 0.0, 1.0, 10));

    int fired = 0;
    dm.set_alert_sink(
        [&](const DriftReport&) { ++fired; },
        DriftSeverity::moder);

    for (int i = 0; i < 200; ++i) REQUIRE(dm.observe("a", 0.95));
    const int fired_before = fired;
    CHECK(fired_before >= 1);

    dm.clear_alerts();

    // clear_alerts must NOT touch the histograms — observation counts,
    // PSI, and severity classification are all unchanged.
    auto ca = dm.observation_count("a");
    REQUIRE(ca);
    CHECK(ca.value() == 200);
    auto rep = dm.report();
    REQUIRE(rep.size() == 2);
    bool saw_a = false;
    for (const auto& r : rep) {
        if (r.feature == "a") {
            saw_a = true;
            CHECK(r.severity == DriftSeverity::severe);
            CHECK(r.current_n == 200);
        }
    }
    CHECK(saw_a);
}

// ---- Feature 4: DriftMonitor::feature_severity --------------------------

TEST_CASE("DriftMonitor::feature_severity basic — none for matching distributions") {
    DriftMonitor dm;
    std::vector<double> baseline;
    baseline.reserve(500);
    std::mt19937 rng{7};
    std::uniform_real_distribution<double> d{0.0, 1.0};
    for (int i = 0; i < 500; ++i) baseline.push_back(d(rng));
    REQUIRE(dm.register_feature("x", baseline, 0.0, 1.0, 10));

    // Mirror the baseline → small PSI → severity none.
    for (int i = 0; i < 500; ++i) REQUIRE(dm.observe("x", d(rng)));

    auto sev = dm.feature_severity("x");
    REQUIRE(sev);
    CHECK(sev.value() == DriftSeverity::none);
}

TEST_CASE("DriftMonitor::feature_severity edge — not_found for unregistered feature") {
    DriftMonitor dm;
    auto sev = dm.feature_severity("missing");
    REQUIRE_FALSE(sev);
    CHECK(sev.error().code() == ErrorCode::not_found);
}

TEST_CASE("DriftMonitor::feature_severity integration — agrees with report() and classify()") {
    DriftMonitor dm;
    std::vector<double> baseline(500, 0.2);
    REQUIRE(dm.register_feature("score", baseline, 0.0, 1.0, 10));
    for (int i = 0; i < 500; ++i) REQUIRE(dm.observe("score", 0.85));

    auto sev = dm.feature_severity("score");
    REQUIRE(sev);
    CHECK(sev.value() == DriftSeverity::severe);

    // Cross-check against report() which computes the same thing in bulk.
    auto rep = dm.report();
    REQUIRE(rep.size() == 1);
    CHECK(rep[0].severity == sev.value());
    CHECK(DriftMonitor::classify(rep[0].psi) == sev.value());
}

// ---- Feature 5: MetricRegistry::increment_or_create ---------------------

TEST_CASE("MetricRegistry::increment_or_create basic — creates on first call") {
    MetricRegistry m;
    CHECK_FALSE(m.has_counter("inferences_total"));
    m.increment_or_create("inferences_total");
    CHECK(m.has_counter("inferences_total"));
    CHECK(m.count("inferences_total") == 1);
}

TEST_CASE("MetricRegistry::increment_or_create edge — explicit delta accumulates") {
    MetricRegistry m;
    m.increment_or_create("bytes", 100);
    m.increment_or_create("bytes", 50);
    // Default-arg call (delta=1).
    m.increment_or_create("bytes");
    CHECK(m.count("bytes") == 151);
}

TEST_CASE("MetricRegistry::increment_or_create integration — interchangeable with inc/add") {
    MetricRegistry m;
    m.inc("ticks");                          // +1
    m.add("ticks", 2);                       // +2
    m.increment_or_create("ticks", 3);       // +3
    m.increment_or_create("ticks");          // +1
    CHECK(m.count("ticks") == 7);

    // And the alias surfaces in the prometheus exposition just like inc().
    auto prom = m.snapshot_prometheus();
    CHECK(prom.find("asclepius_ticks 7") != std::string::npos);
}

// ---- Feature 5: MetricRegistry::has -------------------------------------

TEST_CASE("MetricRegistry::has basic — false on fresh registry, true after inc/observe") {
    MetricRegistry m;
    CHECK_FALSE(m.has("inferences_total"));
    CHECK_FALSE(m.has("latency_seconds"));

    m.inc("inferences_total");
    m.observe("latency_seconds", 0.123);

    CHECK(m.has("inferences_total"));
    CHECK(m.has("latency_seconds"));
    // Unknown names still return false.
    CHECK_FALSE(m.has("not_emitted"));
}

TEST_CASE("MetricRegistry::has edge — survives reset/clear semantics correctly") {
    MetricRegistry m;
    m.inc("a", 5);
    m.observe("b", 0.5);
    REQUIRE(m.has("a"));
    REQUIRE(m.has("b"));

    // reset() zeroes the counter but leaves the name registered → has() still true.
    REQUIRE(m.reset("a"));
    CHECK(m.has("a"));
    CHECK(m.count("a") == 0);

    // reset_histograms() drops histograms entirely → has(b) flips to false,
    // has(a) stays true (counters untouched).
    m.reset_histograms();
    CHECK(m.has("a"));
    CHECK_FALSE(m.has("b"));

    // clear() drops everything → has() universally false.
    m.clear();
    CHECK_FALSE(m.has("a"));
    CHECK_FALSE(m.has("b"));
}

TEST_CASE("MetricRegistry::has integration — agrees with has_counter || has_histogram") {
    MetricRegistry m;
    m.inc("c1");
    m.observe("h1", 0.01);

    // Counter-only name.
    CHECK(m.has("c1"));
    CHECK(m.has_counter("c1"));
    CHECK_FALSE(m.has_histogram("c1"));
    CHECK(m.has("c1") == (m.has_counter("c1") || m.has_histogram("c1")));

    // Histogram-only name.
    CHECK(m.has("h1"));
    CHECK_FALSE(m.has_counter("h1"));
    CHECK(m.has_histogram("h1"));
    CHECK(m.has("h1") == (m.has_counter("h1") || m.has_histogram("h1")));

    // Unknown name — both predicates and the union are false.
    CHECK_FALSE(m.has("missing"));
    CHECK(m.has("missing") == (m.has_counter("missing") || m.has_histogram("missing")));

    // noexcept contract — verified at compile time. We pass a string_view
    // literal (""sv) rather than a const char*; the latter goes through
    // basic_string_view(const char*) which is NOT noexcept in the C++
    // standard (it calls char_traits::length, which the standard does
    // not mark noexcept). libstdc++ adds a vendor-extension noexcept
    // there but libc++ (Apple) follows the spec strictly, so a
    // const-char* call would compile-fail this static_assert on macOS.
    // The ""sv literal is unconditionally noexcept on both stdlibs.
    using namespace std::literals::string_view_literals;
    static_assert(noexcept(std::declval<const MetricRegistry&>().has("x"sv)),
                  "MetricRegistry::has must be noexcept");
}

// ---- Histogram::clear ---------------------------------------------------

TEST_CASE("Histogram::clear empties counts and total but keeps bins/lo/hi") {
    Histogram h{0.0, 1.0, 10};
    for (int i = 0; i < 50; ++i) h.observe(0.25);
    REQUIRE(h.total() == 50);
    REQUIRE(h.bin_count() == 10);

    h.clear();
    CHECK(h.total() == 0);
    CHECK(h.is_empty());
    // Geometry preserved.
    CHECK(h.bin_count() == 10);
    CHECK(h.lo() == doctest::Approx(0.0));
    CHECK(h.hi() == doctest::Approx(1.0));
    // Normalized must be all zeros after clear (matches empty-histogram contract).
    auto n = h.normalized();
    REQUIRE(n.size() == 10);
    for (auto v : n) CHECK(v == doctest::Approx(0.0));
}

TEST_CASE("Histogram::clear allows reuse — observations after clear count fresh") {
    Histogram h{0.0, 1.0, 4};
    for (int i = 0; i < 10; ++i) h.observe(0.1);  // bin 0
    REQUIRE(h.total() == 10);

    h.clear();
    REQUIRE(h.total() == 0);

    // Now observe into different bins; counts should reflect ONLY post-clear state.
    h.observe(0.6);   // bin 2
    h.observe(0.9);   // bin 3
    h.observe(0.85);  // bin 3
    CHECK(h.total() == 3);
    CHECK(h.nonzero_bin_count() == 2);
}

TEST_CASE("Histogram::clear is idempotent on an already-empty histogram") {
    Histogram h{-1.0, 1.0, 8};
    REQUIRE(h.total() == 0);
    h.clear();
    CHECK(h.total() == 0);
    CHECK(h.bin_count() == 8);
    h.clear();  // again
    CHECK(h.total() == 0);
    CHECK(h.bin_count() == 8);
    CHECK(h.lo() == doctest::Approx(-1.0));
    CHECK(h.hi() == doctest::Approx(1.0));
}

// ---- Histogram::nonzero_bin_count ---------------------------------------

TEST_CASE("Histogram::nonzero_bin_count returns 0 on empty histogram") {
    Histogram h{0.0, 1.0, 16};
    CHECK(h.nonzero_bin_count() == 0);
}

TEST_CASE("Histogram::nonzero_bin_count counts only bins with >=1 observation") {
    Histogram h{0.0, 1.0, 10};
    // Hit bin 0, bin 5, bin 9 — three distinct bins, multiple observations each.
    for (int i = 0; i < 3; ++i) h.observe(0.05);
    for (int i = 0; i < 7; ++i) h.observe(0.55);
    h.observe(0.95);
    CHECK(h.total() == 11);
    CHECK(h.nonzero_bin_count() == 3);
}

TEST_CASE("Histogram::nonzero_bin_count saturates at bin_count when all bins hit") {
    Histogram h{0.0, 1.0, 10};
    // Observation at the midpoint of each bin guarantees one count per bin.
    for (std::size_t i = 0; i < 10; ++i) {
        const double mid = (static_cast<double>(i) + 0.5) / 10.0;
        h.observe(mid);
    }
    CHECK(h.total() == 10);
    CHECK(h.nonzero_bin_count() == h.bin_count());
}

// ---- MetricRegistry::counter_total --------------------------------------

TEST_CASE("MetricRegistry::counter_total is 0 on empty registry") {
    MetricRegistry m;
    CHECK(m.counter_total() == 0);
}

TEST_CASE("MetricRegistry::counter_total sums all counter values") {
    MetricRegistry m;
    m.inc("a", 5);
    m.inc("b", 7);
    m.inc("c");      // +1
    m.inc("c", 2);   // c=3
    // total = 5 + 7 + 3 = 15
    CHECK(m.counter_total() == 15);
}

TEST_CASE("MetricRegistry::counter_total ignores histograms and survives reset/clear") {
    MetricRegistry m;
    m.inc("x", 4);
    m.observe("h", 0.1);   // histogram; must NOT contribute
    m.observe("h", 0.5);
    CHECK(m.counter_total() == 4);

    m.inc("y", 6);
    CHECK(m.counter_total() == 10);

    // reset() zeroes one counter but leaves the name registered.
    REQUIRE(m.reset("x"));
    CHECK(m.counter_total() == 6);  // y=6, x=0

    // clear() drops everything.
    m.clear();
    CHECK(m.counter_total() == 0);
}

// ---- DriftMonitor::observe_batch ----------------------------------------

TEST_CASE("DriftMonitor::observe_batch records every value and matches per-call observe") {
    DriftMonitor dm;
    std::vector<double> baseline(100, 0.5);
    REQUIRE(dm.register_feature("f", baseline, 0.0, 1.0, 10));

    std::vector<double> batch{0.05, 0.15, 0.25, 0.35, 0.95};
    REQUIRE(dm.observe_batch("f", std::span<const double>{batch}));

    auto cnt = dm.observation_count("f");
    REQUIRE(cnt);
    CHECK(*cnt == batch.size());
}

TEST_CASE("DriftMonitor::observe_batch returns not_found for unknown feature") {
    DriftMonitor dm;
    std::vector<double> v{0.1, 0.2};
    auto r = dm.observe_batch("missing", std::span<const double>{v});
    REQUIRE_FALSE(r);
    CHECK(r.error().code() == ErrorCode::not_found);

    // Empty span on unknown feature also returns not_found — feature
    // existence is checked before the early empty-span exit.
    std::vector<double> empty;
    auto r2 = dm.observe_batch("missing", std::span<const double>{empty});
    REQUIRE_FALSE(r2);
    CHECK(r2.error().code() == ErrorCode::not_found);
}

TEST_CASE("DriftMonitor::observe_batch — empty span is a valid no-op") {
    DriftMonitor dm;
    std::vector<double> baseline(100, 0.5);
    REQUIRE(dm.register_feature("f", baseline, 0.0, 1.0, 10));

    int fires = 0;
    dm.set_alert_sink([&](const DriftReport&) { ++fires; }, DriftSeverity::minor);

    std::vector<double> empty;
    REQUIRE(dm.observe_batch("f", std::span<const double>{empty}));

    auto cnt = dm.observation_count("f");
    REQUIRE(cnt);
    CHECK(*cnt == 0);
    CHECK(fires == 0);  // empty batch must not trigger the sink
}

TEST_CASE("DriftMonitor::observe_batch fires alert sink at most once per call") {
    DriftMonitor dm;
    std::vector<double> baseline(200, 0.5);
    REQUIRE(dm.register_feature("f", baseline, 0.0, 1.0, 10));

    int fires = 0;
    DriftSeverity last = DriftSeverity::none;
    dm.set_alert_sink([&](const DriftReport& r) { ++fires; last = r.severity; },
                      DriftSeverity::moder);

    // 200 values clustered at 0.95 — should drive PSI well past moder/severe.
    std::vector<double> batch(200, 0.95);
    REQUIRE(dm.observe_batch("f", std::span<const double>{batch}));

    CHECK(fires == 1);
    CHECK(static_cast<int>(last) >= static_cast<int>(DriftSeverity::moder));

    // A subsequent batch at the SAME severity must not re-fire (per-crossing
    // semantics, just like observe()).
    std::vector<double> batch2(200, 0.95);
    REQUIRE(dm.observe_batch("f", std::span<const double>{batch2}));
    CHECK(fires == 1);
}

// ---- DriftMonitor::has_alert_sink ---------------------------------------

TEST_CASE("DriftMonitor::has_alert_sink false on a fresh monitor") {
    DriftMonitor dm;
    CHECK_FALSE(dm.has_alert_sink());
}

TEST_CASE("DriftMonitor::has_alert_sink true after set_alert_sink with non-empty fn") {
    DriftMonitor dm;
    dm.set_alert_sink([](const DriftReport&) {}, DriftSeverity::moder);
    CHECK(dm.has_alert_sink());

    // Also true for severe-only threshold.
    DriftMonitor dm2;
    dm2.set_alert_sink([](const DriftReport&) {}, DriftSeverity::severe);
    CHECK(dm2.has_alert_sink());
}

TEST_CASE("DriftMonitor::has_alert_sink false after install of empty std::function") {
    DriftMonitor dm;
    // Install a real sink first → true.
    dm.set_alert_sink([](const DriftReport&) {}, DriftSeverity::moder);
    REQUIRE(dm.has_alert_sink());

    // Install an empty std::function → no longer wired.
    dm.set_alert_sink(DriftMonitor::AlertSink{}, DriftSeverity::moder);
    CHECK_FALSE(dm.has_alert_sink());

    // noexcept contract — call site must compile under noexcept.
    static_assert(noexcept(std::declval<const DriftMonitor&>().has_alert_sink()),
                  "DriftMonitor::has_alert_sink must be noexcept");
}

// ---- DriftMonitor::report_for_feature -----------------------------------

TEST_CASE("DriftMonitor::report_for_feature returns not_found for unregistered feature") {
    DriftMonitor dm;
    auto r = dm.report_for_feature("missing");
    REQUIRE_FALSE(r);
    CHECK(r.error().code() == ErrorCode::not_found);

    // Even after registering some other feature, an unknown name still
    // returns not_found.
    REQUIRE(dm.register_feature("present", std::vector<double>(50, 0.3)));
    auto r2 = dm.report_for_feature("still_missing");
    REQUIRE_FALSE(r2);
    CHECK(r2.error().code() == ErrorCode::not_found);
}

TEST_CASE("DriftMonitor::report_for_feature on identical reference/current → near-zero PSI") {
    DriftMonitor dm;
    std::vector<double> baseline;
    baseline.reserve(500);
    std::mt19937 rng{17};
    std::uniform_real_distribution<double> d{0.1, 0.9};
    for (int i = 0; i < 500; ++i) baseline.push_back(d(rng));
    REQUIRE(dm.register_feature("score", baseline));

    // Replay the same distribution into the current window.
    std::mt19937 rng2{17};
    std::uniform_real_distribution<double> d2{0.1, 0.9};
    for (int i = 0; i < 500; ++i) {
        REQUIRE(dm.observe("score", d2(rng2)));
    }

    auto r = dm.report_for_feature("score");
    REQUIRE(r);
    CHECK(r.value().feature == "score");
    CHECK(r.value().psi < 0.05);
    CHECK(r.value().severity == DriftSeverity::none);
    CHECK(r.value().reference_n == 500);
    CHECK(r.value().current_n   == 500);
}

TEST_CASE("DriftMonitor::report_for_feature on shifted current → severe") {
    DriftMonitor dm;
    std::vector<double> baseline(500, 0.2);
    REQUIRE(dm.register_feature("latency", baseline));
    for (int i = 0; i < 500; ++i) {
        REQUIRE(dm.observe("latency", 0.85));
    }

    auto r = dm.report_for_feature("latency");
    REQUIRE(r);
    CHECK(r.value().feature == "latency");
    CHECK(r.value().severity == DriftSeverity::severe);
    CHECK(r.value().psi > 0.5);

    // Single-feature report should be consistent with the full report().
    auto full = dm.report();
    REQUIRE(full.size() == 1);
    CHECK(full[0].psi == doctest::Approx(r.value().psi));
}

TEST_CASE("DriftMonitor::report_for_feature picks one feature out of many") {
    DriftMonitor dm;
    REQUIRE(dm.register_feature("a", std::vector<double>(200, 0.2)));
    REQUIRE(dm.register_feature("b", std::vector<double>(200, 0.5)));
    REQUIRE(dm.register_feature("c", std::vector<double>(200, 0.8)));

    // Drift only "b".
    for (int i = 0; i < 200; ++i) REQUIRE(dm.observe("b", 0.05));

    auto ra = dm.report_for_feature("a");
    auto rb = dm.report_for_feature("b");
    auto rc = dm.report_for_feature("c");
    REQUIRE(ra); REQUIRE(rb); REQUIRE(rc);

    // a and c never observed, so current_n == 0 → PSI computation uses
    // only the eps-floor and yields a non-zero number; what we care about
    // is that b's drift is the largest.
    CHECK(rb.value().psi > ra.value().psi);
    CHECK(rb.value().psi > rc.value().psi);
    CHECK(rb.value().feature == "b");
}

// ---- MetricRegistry::counter_value --------------------------------------

TEST_CASE("MetricRegistry::counter_value returns not_found for unknown counter") {
    MetricRegistry m;
    auto r = m.counter_value("nope");
    REQUIRE_FALSE(r);
    CHECK(r.error().code() == ErrorCode::not_found);
}

TEST_CASE("MetricRegistry::counter_value returns the tracked value (incl. zero)") {
    MetricRegistry m;
    m.inc("hits", 7);
    auto r = m.counter_value("hits");
    REQUIRE(r);
    CHECK(r.value() == 7);

    // Reset the counter to zero. counter_value() must still succeed and
    // return 0 — disambiguating "missing" from "zero", which count() can't.
    REQUIRE(m.reset("hits"));
    auto r0 = m.counter_value("hits");
    REQUIRE(r0);
    CHECK(r0.value() == 0);
    CHECK(m.count("hits") == 0);  // count() yields the same scalar value.
}

TEST_CASE("MetricRegistry::counter_value rejects histograms (separate name space)") {
    MetricRegistry m;
    m.observe("latency_seconds", 0.012);
    m.observe("latency_seconds", 0.045);

    // count() falls through to histograms and returns the obs count (=2).
    CHECK(m.count("latency_seconds") == 2);

    // counter_value() does NOT fall through — histograms aren't counters.
    auto r = m.counter_value("latency_seconds");
    REQUIRE_FALSE(r);
    CHECK(r.error().code() == ErrorCode::not_found);
}

// ---- MetricRegistry::histogram_quantile ---------------------------------

TEST_CASE("MetricRegistry::histogram_quantile returns 0.0 for unknown / empty histograms") {
    MetricRegistry m;
    // Unknown name.
    CHECK(m.histogram_quantile("nope", 0.5) == 0.0);
    CHECK(m.histogram_quantile("nope", 0.0) == 0.0);
    CHECK(m.histogram_quantile("nope", 1.0) == 0.0);
}

TEST_CASE("MetricRegistry::histogram_quantile interpolates within a bucket") {
    MetricRegistry m;
    // All observations land in the (0.005, 0.01] bucket.
    for (int i = 0; i < 100; ++i) m.observe("lat", 0.008);

    // q=0.5 should pick a value within the (0.005, 0.01] bucket. The
    // interpolation walks from lower=0.005 to upper=0.01 with the full
    // bucket mass, so the median resolves to 0.005 + 0.5 * 0.005 = 0.0075.
    const double med = m.histogram_quantile("lat", 0.5);
    CHECK(med >= 0.005);
    CHECK(med <= 0.01);
    CHECK(med == doctest::Approx(0.0075).epsilon(1e-9));

    // q clamping: negative → 0; > 1 → clamps to 1.
    CHECK(m.histogram_quantile("lat", -1.0) >= 0.0);
    CHECK(m.histogram_quantile("lat",  2.0) <= 0.01);
}

TEST_CASE("MetricRegistry::histogram_quantile is monotone non-decreasing in q") {
    MetricRegistry m;
    // Mix of buckets so a real CDF develops.
    for (int i = 0; i < 50; ++i) m.observe("lat", 0.002);
    for (int i = 0; i < 50; ++i) m.observe("lat", 0.020);
    for (int i = 0; i < 50; ++i) m.observe("lat", 0.300);
    for (int i = 0; i < 50; ++i) m.observe("lat", 1.500);

    const double q10 = m.histogram_quantile("lat", 0.10);
    const double q50 = m.histogram_quantile("lat", 0.50);
    const double q90 = m.histogram_quantile("lat", 0.90);
    CHECK(q10 <= q50);
    CHECK(q50 <= q90);
    // Sanity: q90 should sit above the 0.25 bucket because >75% of
    // observations have value <= 0.3.
    CHECK(q90 > 0.25);
    CHECK(q90 <= 5.0);  // never exceeds the last finite bucket edge
}

// ---- Histogram::reset_to ------------------------------------------------

TEST_CASE("Histogram::reset_to copies content from another histogram") {
    Histogram src{0.0, 1.0, 10};
    for (int i = 0; i < 50; ++i) src.observe(0.15);
    for (int i = 0; i < 30; ++i) src.observe(0.65);
    REQUIRE(src.total() == 80);

    Histogram dst{0.0, 1.0, 10};
    for (int i = 0; i < 5; ++i) dst.observe(0.95);
    REQUIRE(dst.total() == 5);

    dst.reset_to(src);
    CHECK(dst.total()     == src.total());
    CHECK(dst.lo()        == src.lo());
    CHECK(dst.hi()        == src.hi());
    CHECK(dst.bin_count() == src.bin_count());
    // PSI of two content-equal histograms is ~0.
    CHECK(Histogram::psi(src, dst) == doctest::Approx(0.0).epsilon(1e-12));
    CHECK(dst.normalized() == src.normalized());
}

TEST_CASE("Histogram::reset_to overwrites prior bin / lo / hi configuration") {
    Histogram src{-2.0, 8.0, 5};
    for (int i = 0; i < 10; ++i) src.observe(3.0);

    Histogram dst{0.0, 1.0, 20};   // intentionally different shape
    for (int i = 0; i < 100; ++i) dst.observe(0.4);

    dst.reset_to(src);
    CHECK(dst.lo()        == doctest::Approx(-2.0));
    CHECK(dst.hi()        == doctest::Approx( 8.0));
    CHECK(dst.bin_count() == 5);
    CHECK(dst.total()     == 10);

    // After reset, observing into dst must respect the *new* lo/hi.
    dst.observe(7.5);
    CHECK(dst.total() == 11);
}

TEST_CASE("Histogram::reset_to is a no-op on self-assignment") {
    Histogram h{0.0, 1.0, 10};
    for (int i = 0; i < 7; ++i) h.observe(0.25);
    REQUIRE(h.total() == 7);

    h.reset_to(h);  // self-assign — must not deadlock or corrupt counts
    CHECK(h.total() == 7);
    CHECK(h.lo()    == 0.0);
    CHECK(h.hi()    == 1.0);
}

TEST_CASE("Histogram::reset_to: source then mutated does NOT affect dst (deep copy)") {
    Histogram src{0.0, 1.0, 4};
    for (int i = 0; i < 12; ++i) src.observe(0.1);

    Histogram dst{0.0, 1.0, 4};
    dst.reset_to(src);
    REQUIRE(dst.total() == 12);

    // Mutating src after reset_to must NOT bleed into dst — copy is deep.
    for (int i = 0; i < 100; ++i) src.observe(0.9);
    CHECK(src.total() == 112);
    CHECK(dst.total() == 12);
}

// ---- DriftMonitor::observe_uniform --------------------------------------

TEST_CASE("DriftMonitor::observe_uniform folds n copies into the current window") {
    DriftMonitor dm;
    std::vector<double> baseline(500, 0.2);
    REQUIRE(dm.register_feature("score", baseline));

    dm.observe_uniform("score", 0.85, 250);

    auto cnt = dm.observation_count("score");
    REQUIRE(cnt);
    CHECK(cnt.value() == 250);

    // The drift report should reflect the bulk shift — same as if we
    // had called observe() 250 times.
    auto r = dm.report_for_feature("score");
    REQUIRE(r);
    CHECK(r.value().current_n == 250);
    CHECK(r.value().psi > 0.5);
}

TEST_CASE("DriftMonitor::observe_uniform is silent (no error) on unregistered feature") {
    DriftMonitor dm;
    // No register_feature() — call should be a silent no-op (not throw,
    // not add an entry).
    dm.observe_uniform("missing", 0.5, 100);
    CHECK_FALSE(dm.has_feature("missing"));
    CHECK(dm.feature_count() == 0);

    // Distinct from observe(), which DOES return not_found.
    auto r = dm.observe("missing", 0.5);
    REQUIRE_FALSE(r);
    CHECK(r.error().code() == ErrorCode::not_found);
}

TEST_CASE("DriftMonitor::observe_uniform with n=0 is a no-op") {
    DriftMonitor dm;
    REQUIRE(dm.register_feature("x", std::vector<double>(100, 0.3)));

    dm.observe_uniform("x", 0.99, 0);
    auto cnt = dm.observation_count("x");
    REQUIRE(cnt);
    CHECK(cnt.value() == 0);
}

TEST_CASE("DriftMonitor::observe_uniform fires alert sink at most once per call") {
    DriftMonitor dm;
    REQUIRE(dm.register_feature("y", std::vector<double>(500, 0.2)));

    int fire_count = 0;
    DriftSeverity last_sev = DriftSeverity::none;
    dm.set_alert_sink(
        [&](const DriftReport& r) {
            ++fire_count;
            last_sev = r.severity;
        },
        DriftSeverity::moder);

    // Push 500 uniform observations far from baseline → PSI spikes,
    // sink fires exactly once for this batch.
    dm.observe_uniform("y", 0.95, 500);
    CHECK(fire_count == 1);
    CHECK(static_cast<int>(last_sev) >= static_cast<int>(DriftSeverity::moder));

    // A second call at the same severity does NOT re-fire (per-crossing,
    // not per-call semantics).
    dm.observe_uniform("y", 0.95, 100);
    CHECK(fire_count == 1);
}

// ---- MetricRegistry::ratio ----------------------------------------------

TEST_CASE("MetricRegistry::ratio basic — count(numerator)/count(denominator)") {
    MetricRegistry m;
    m.inc("blocked_input", 3);
    m.inc("inference_attempts", 12);

    auto r = m.ratio("blocked_input", "inference_attempts");
    REQUIRE(r);
    CHECK(r.value() == doctest::Approx(0.25));

    // Mutating the underlying counters changes subsequent ratios.
    m.inc("blocked_input", 9);
    auto r2 = m.ratio("blocked_input", "inference_attempts");
    REQUIRE(r2);
    CHECK(r2.value() == doctest::Approx(1.0));
}

TEST_CASE("MetricRegistry::ratio edge — not_found vs invalid_argument disambiguated") {
    MetricRegistry m;
    m.inc("a", 5);
    m.inc("zero", 0);  // existing-but-zero counter

    // Numerator missing → not_found. Denominator missing → not_found.
    {
        auto r = m.ratio("missing", "a");
        REQUIRE_FALSE(r);
        CHECK(r.error().code() == ErrorCode::not_found);
    }
    {
        auto r = m.ratio("a", "missing");
        REQUIRE_FALSE(r);
        CHECK(r.error().code() == ErrorCode::not_found);
    }
    // Existing zero denominator → invalid_argument (NOT not_found —
    // counter_value semantics distinguish missing from 0).
    {
        auto r = m.ratio("a", "zero");
        REQUIRE_FALSE(r);
        CHECK(r.error().code() == ErrorCode::invalid_argument);
    }
}

TEST_CASE("MetricRegistry::ratio integration — agrees with counter_value() division") {
    MetricRegistry m;
    m.inc("hits", 7);
    m.inc("requests", 50);

    auto num = m.counter_value("hits");
    auto den = m.counter_value("requests");
    REQUIRE(num);
    REQUIRE(den);
    REQUIRE(den.value() != 0);

    auto rt = m.ratio("hits", "requests");
    REQUIRE(rt);
    const double expected =
        static_cast<double>(num.value()) / static_cast<double>(den.value());
    CHECK(rt.value() == doctest::Approx(expected));

    // 0-valued numerator is fine — only zero DENOMINATOR is invalid.
    m.inc("zero_top", 0);
    auto r0 = m.ratio("zero_top", "requests");
    REQUIRE(r0);
    CHECK(r0.value() == doctest::Approx(0.0));
}

// ---- MetricRegistry::counter_diff_total ---------------------------------

TEST_CASE("MetricRegistry::counter_diff_total basic — sum of |delta| across union") {
    MetricRegistry m;
    m.inc("a", 5);
    m.inc("b", 10);
    auto baseline = m.counter_snapshot();

    // Move +3 on 'a', -7 on 'b' (no decrement primitive; simulate via
    // baseline alteration), introduce new 'c' with +4.
    m.inc("a", 3);
    m.inc("c", 4);
    // Pretend baseline had b=10; now still 10 → delta 0.
    auto total = m.counter_diff_total(baseline);
    CHECK(total == 7);  // |3| + |0| + |4|
}

TEST_CASE("MetricRegistry::counter_diff_total edge — empty baseline / empty registry") {
    // Empty current ∪ empty baseline → 0.
    {
        MetricRegistry m;
        std::unordered_map<std::string, std::uint64_t> empty_base;
        CHECK(m.counter_diff_total(empty_base) == 0);
    }
    // Empty baseline, populated current → sum of all current values.
    {
        MetricRegistry m;
        m.inc("x", 4);
        m.inc("y", 6);
        std::unordered_map<std::string, std::uint64_t> empty_base;
        CHECK(m.counter_diff_total(empty_base) == 10);
    }
    // Populated baseline, empty current → sum of all baseline values
    // (counters "disappeared" since baseline).
    {
        MetricRegistry m;
        std::unordered_map<std::string, std::uint64_t> base{{"gone", 11}, {"poof", 4}};
        CHECK(m.counter_diff_total(base) == 15);
    }
}

TEST_CASE("MetricRegistry::counter_diff_total integration — equals sum |diff(baseline).values|") {
    MetricRegistry m;
    m.inc("kept_same", 5);
    m.inc("grew", 1);
    m.inc("dropped_in_baseline", 0);  // present in current with 0
    auto baseline = m.counter_snapshot();
    // baseline: {kept_same:5, grew:1, dropped_in_baseline:0}

    // After: kept_same unchanged, grew +9, dropped_in_baseline still 0,
    // and a new counter 'newish' with +2.
    m.inc("grew", 9);
    m.inc("newish", 2);

    auto d = m.diff(baseline);
    std::uint64_t expected_abs_sum = 0;
    for (const auto& [_, dv] : d) {
        expected_abs_sum += static_cast<std::uint64_t>(dv < 0 ? -dv : dv);
    }
    CHECK(m.counter_diff_total(baseline) == expected_abs_sum);
}

// ---- DriftMonitor::trend_for_feature ------------------------------------

TEST_CASE("DriftMonitor::trend_for_feature basic — returns single current snapshot") {
    DriftMonitor dm;
    REQUIRE(dm.register_feature("score", std::vector<double>(200, 0.3)));
    for (int i = 0; i < 200; ++i) {
        REQUIRE(dm.observe("score", 0.85));
    }

    // n is ignored in the current implementation; any value should yield
    // a single-element vector reflecting the current snapshot.
    auto t = dm.trend_for_feature("score", 8);
    REQUIRE(t.size() == 1);
    CHECK(t[0].feature == "score");
    CHECK(t[0].current_n == 200);
    CHECK(t[0].reference_n == 200);
    CHECK(t[0].psi > 0.0);
    CHECK(static_cast<int>(t[0].severity) >= static_cast<int>(DriftSeverity::moder));
}

TEST_CASE("DriftMonitor::trend_for_feature edge — unregistered feature returns empty") {
    DriftMonitor dm;
    // No features registered → empty (not an error — this is a "show me
    // trend" call where no-trend is a valid answer).
    CHECK(dm.trend_for_feature("ghost", 5).empty());

    // Register a different feature; the queried one is still unknown.
    REQUIRE(dm.register_feature("real", std::vector<double>(50, 0.1)));
    CHECK(dm.trend_for_feature("ghost", 5).empty());
    CHECK(dm.trend_for_feature("ghost", 0).empty());
}

TEST_CASE("DriftMonitor::trend_for_feature integration — agrees with report_for_feature") {
    DriftMonitor dm;
    REQUIRE(dm.register_feature("a", std::vector<double>(300, 0.2)));
    REQUIRE(dm.register_feature("b", std::vector<double>(300, 0.7)));
    for (int i = 0; i < 100; ++i) REQUIRE(dm.observe("a", 0.25));
    for (int i = 0; i < 100; ++i) REQUIRE(dm.observe("b", 0.75));

    auto rep = dm.report_for_feature("a");
    REQUIRE(rep);
    auto trend = dm.trend_for_feature("a", 4);
    REQUIRE(trend.size() == 1);
    CHECK(trend[0].feature == rep->feature);
    CHECK(trend[0].reference_n == rep->reference_n);
    CHECK(trend[0].current_n == rep->current_n);
    // PSI/KS/EMD computed on the same underlying histograms — values
    // recomputed back-to-back agree to bit-for-bit on a single thread.
    CHECK(trend[0].psi == doctest::Approx(rep->psi));
    CHECK(trend[0].ks_statistic == doctest::Approx(rep->ks_statistic));
    CHECK(trend[0].emd == doctest::Approx(rep->emd));
    CHECK(trend[0].severity == rep->severity);
}

// ---- MetricRegistry::is_empty -------------------------------------------

TEST_CASE("MetricRegistry::is_empty basic — true on fresh registry, false after counter") {
    MetricRegistry m;
    CHECK(m.is_empty());

    m.inc("first");
    CHECK_FALSE(m.is_empty());
    CHECK(m.has_counter("first"));
}

TEST_CASE("MetricRegistry::is_empty edge — false with only histogram, returns true after clear") {
    MetricRegistry m;
    REQUIRE(m.is_empty());

    // Histograms alone count as non-empty (counter and histogram name
    // spaces are independent).
    m.observe("latency", 0.05);
    CHECK_FALSE(m.is_empty());
    CHECK(m.has_histogram("latency"));

    // Reset just the histograms — only a counter remains? No: drop them.
    m.reset_histograms();
    CHECK(m.is_empty());

    // Reintroduce both, then full clear.
    m.inc("c");
    m.observe("h", 0.1);
    REQUIRE_FALSE(m.is_empty());
    m.clear();
    CHECK(m.is_empty());

    // noexcept contract — verified at compile time. is_empty() takes no
    // arguments, so the assert is on the bare call (no string_view
    // conversion concerns to worry about, unlike has(name) which needs
    // a ""sv literal to keep noexcept on libc++).
    static_assert(noexcept(std::declval<const MetricRegistry&>().is_empty()),
                  "MetricRegistry::is_empty must be noexcept");
}

TEST_CASE("MetricRegistry::is_empty integration — agrees with counter_count + histogram_count_total") {
    MetricRegistry m;
    CHECK(m.is_empty() == (m.counter_count() == 0 && m.histogram_count_total() == 0));

    m.inc("a");
    CHECK(m.is_empty() == (m.counter_count() == 0 && m.histogram_count_total() == 0));
    m.observe("h", 0.5);
    CHECK(m.is_empty() == (m.counter_count() == 0 && m.histogram_count_total() == 0));

    m.clear();
    CHECK(m.is_empty() == (m.counter_count() == 0 && m.histogram_count_total() == 0));
    CHECK(m.is_empty());
}

// ---- Histogram::bin_at --------------------------------------------------

TEST_CASE("Histogram::bin_at basic — returns count for in-range bins") {
    Histogram h{0.0, 1.0, 4};
    h.observe(0.05);  // bin 0
    h.observe(0.05);
    h.observe(0.30);  // bin 1
    h.observe(0.95);  // bin 3

    auto b0 = h.bin_at(0);
    REQUIRE(b0);
    CHECK(b0.value() == 2);

    auto b1 = h.bin_at(1);
    REQUIRE(b1);
    CHECK(b1.value() == 1);

    auto b2 = h.bin_at(2);
    REQUIRE(b2);
    CHECK(b2.value() == 0);

    auto b3 = h.bin_at(3);
    REQUIRE(b3);
    CHECK(b3.value() == 1);
}

TEST_CASE("Histogram::bin_at edge — index out of range returns invalid_argument") {
    Histogram h{0.0, 1.0, 4};
    {
        auto r = h.bin_at(4);  // exactly bin_count()
        REQUIRE_FALSE(r);
        CHECK(r.error().code() == ErrorCode::invalid_argument);
    }
    {
        auto r = h.bin_at(99);
        REQUIRE_FALSE(r);
        CHECK(r.error().code() == ErrorCode::invalid_argument);
    }
    // Empty histogram: in-range index returns 0, out-of-range still
    // returns invalid_argument.
    {
        auto r = h.bin_at(0);
        REQUIRE(r);
        CHECK(r.value() == 0);
    }
}

TEST_CASE("Histogram::bin_at integration — sum across bins equals total()") {
    Histogram h{0.0, 1.0, 5};
    h.observe(0.05);
    h.observe(0.25);
    h.observe(0.45);
    h.observe(0.65);
    h.observe(0.85);
    h.observe(0.85);
    REQUIRE(h.total() == 6);
    REQUIRE(h.bin_count() == 5);

    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < h.bin_count(); ++i) {
        auto r = h.bin_at(i);
        REQUIRE(r);
        sum += r.value();
    }
    CHECK(sum == h.total());

    // bin_at agrees with normalized() * total() on populated bins
    // (within rounding for the doubles->uint64 path).
    auto n = h.normalized();
    REQUIRE(n.size() == h.bin_count());
    for (std::size_t i = 0; i < h.bin_count(); ++i) {
        auto r = h.bin_at(i);
        REQUIRE(r);
        const double expected = n[i] * static_cast<double>(h.total());
        CHECK(static_cast<double>(r.value()) == doctest::Approx(expected));
    }
}

// ---- Histogram::p99 -----------------------------------------------------

TEST_CASE("Histogram::p99 basic — agrees with percentile(99.0)") {
    Histogram h{0.0, 1.0, 100};
    std::mt19937 rng{42};
    std::uniform_real_distribution<double> d{0.0, 1.0};
    for (int i = 0; i < 1000; ++i) h.observe(d(rng));

    // Sugar wrapper: must be bit-for-bit identical to percentile(99.0)
    // since both call quantile(0.99) under the hood with the same lock.
    CHECK(h.p99() == doctest::Approx(h.percentile(99.0)));
    // And that's identical to quantile(0.99).
    CHECK(h.p99() == doctest::Approx(h.quantile(0.99)));
}

TEST_CASE("Histogram::p99 edge — empty histogram returns 0.0") {
    Histogram h{0.0, 1.0, 10};
    REQUIRE(h.is_empty());
    // Empty histogram contract from quantile(): returns 0.0.
    CHECK(h.p99() == doctest::Approx(0.0));

    // Negative-domain empty histogram still returns 0.0 (the sentinel,
    // not lo()) — this matches percentile()'s documented contract.
    Histogram hn{-5.0, 5.0, 10};
    CHECK(hn.p99() == doctest::Approx(0.0));
}

TEST_CASE("Histogram::p99 integration — monotone with percentile, agrees with quantile(0.99)") {
    Histogram h{0.0, 1.0, 100};
    // 90% of mass low, 10% in the upper tail — p99 should land in the
    // upper-tail region by construction.
    for (int i = 0; i < 900; ++i) h.observe(0.10);
    for (int i = 0; i < 100; ++i) h.observe(0.92);
    REQUIRE(h.total() == 1000);

    const double m   = h.median();
    const double p99 = h.p99();
    // Median should sit near the 0.10 cluster (where 90% of mass is).
    CHECK(m < 0.2);
    // p99 sits above the median — by construction the top 1% of mass
    // lives in the 0.92 cluster, so p99 must reach into that region.
    CHECK(p99 > m);
    CHECK(p99 >= 0.5);
    // Monotone in p: p99 >= p50 >= p01 always.
    CHECK(h.p99() >= h.percentile(50.0));
    CHECK(h.percentile(50.0) >= h.percentile(1.0));
    // p99 must agree exactly with quantile(0.99) — it's literally a
    // sugar wrapper, no rounding path between them.
    CHECK(h.p99() == doctest::Approx(h.quantile(0.99)));
}

// ---- Histogram::nth_largest_bin -----------------------------------------

TEST_CASE("Histogram::nth_largest_bin basic — picks bin with highest count") {
    Histogram h{0.0, 1.0, 5};
    // Bin 0: 3 hits, bin 1: 1 hit, bin 2: 7 hits, bin 3: 2 hits, bin 4: 0.
    for (int i = 0; i < 3; ++i) h.observe(0.05);
    h.observe(0.25);
    for (int i = 0; i < 7; ++i) h.observe(0.45);
    for (int i = 0; i < 2; ++i) h.observe(0.65);

    CHECK(h.nth_largest_bin() == 2);

    // Adding more to a different bin should change the answer.
    for (int i = 0; i < 10; ++i) h.observe(0.85);  // bin 4 now has 10
    CHECK(h.nth_largest_bin() == 4);
}

TEST_CASE("Histogram::nth_largest_bin edge — empty histogram returns 0; ties → smallest index") {
    // Empty histogram: every bin has count 0, smallest index wins.
    Histogram empty{0.0, 1.0, 8};
    REQUIRE(empty.is_empty());
    CHECK(empty.nth_largest_bin() == 0);

    // Tie-breaking: bins 1 and 3 both have count 5, bin 0 has count 4.
    // Smallest index among the tied maxes wins → bin 1.
    Histogram h{0.0, 1.0, 5};
    for (int i = 0; i < 4; ++i) h.observe(0.05);   // bin 0 → 4
    for (int i = 0; i < 5; ++i) h.observe(0.25);   // bin 1 → 5
    for (int i = 0; i < 5; ++i) h.observe(0.65);   // bin 3 → 5
    CHECK(h.nth_largest_bin() == 1);

    // Three-way tie at 2 in bins 0, 2, 4: smallest index is 0.
    Histogram h3{0.0, 1.0, 5};
    for (int i = 0; i < 2; ++i) h3.observe(0.05);
    for (int i = 0; i < 2; ++i) h3.observe(0.45);
    for (int i = 0; i < 2; ++i) h3.observe(0.85);
    CHECK(h3.nth_largest_bin() == 0);
}

TEST_CASE("Histogram::nth_largest_bin integration — agrees with bin_at scan") {
    Histogram h{0.0, 1.0, 10};
    std::mt19937 rng{99};
    std::normal_distribution<double> nd{0.6, 0.1};
    for (int i = 0; i < 500; ++i) h.observe(std::clamp(nd(rng), 0.0, 1.0));
    REQUIRE(h.total() == 500);

    // Reference impl: scan bins via bin_at and pick smallest-index max.
    std::size_t best_idx = 0;
    std::uint64_t best_count = 0;
    {
        auto r0 = h.bin_at(0);
        REQUIRE(r0);
        best_count = r0.value();
    }
    for (std::size_t i = 1; i < h.bin_count(); ++i) {
        auto r = h.bin_at(i);
        REQUIRE(r);
        if (r.value() > best_count) {
            best_count = r.value();
            best_idx   = i;
        }
    }
    CHECK(h.nth_largest_bin() == best_idx);

    // The picked bin must have count >= every other bin.
    auto rb = h.bin_at(h.nth_largest_bin());
    REQUIRE(rb);
    for (std::size_t i = 0; i < h.bin_count(); ++i) {
        auto r = h.bin_at(i);
        REQUIRE(r);
        CHECK(rb.value() >= r.value());
    }
}

// ---- MetricRegistry::sum_counters_with_prefix ---------------------------

TEST_CASE("MetricRegistry::sum_counters_with_prefix basic — sums matching counters") {
    MetricRegistry m;
    m.inc("inferences_total",        7);
    m.inc("inferences_blocked",      3);
    m.inc("inferences_failed",       2);
    m.inc("policy_violations_total", 11);
    m.inc("audit_appends",           5);

    CHECK(m.sum_counters_with_prefix("inferences_") == 12);  // 7 + 3 + 2
    CHECK(m.sum_counters_with_prefix("policy_")     == 11);
    CHECK(m.sum_counters_with_prefix("audit_")      == 5);
    // No matches → 0.
    CHECK(m.sum_counters_with_prefix("missing_")    == 0);
}

TEST_CASE("MetricRegistry::sum_counters_with_prefix edge — empty prefix matches all") {
    MetricRegistry m;
    // Empty registry → 0 regardless of prefix.
    CHECK(m.sum_counters_with_prefix("")    == 0);
    CHECK(m.sum_counters_with_prefix("any") == 0);

    m.inc("a", 1);
    m.inc("b", 2);
    m.inc("c", 3);
    // Empty prefix is the "match every counter" sentinel → equivalent
    // to counter_total().
    CHECK(m.sum_counters_with_prefix("") == m.counter_total());
    CHECK(m.sum_counters_with_prefix("") == 6);

    // Histograms must NOT contribute (counters and histograms are
    // independent name spaces).
    m.observe("z_hist", 0.1);
    CHECK(m.sum_counters_with_prefix("") == 6);
    CHECK(m.sum_counters_with_prefix("z") == 0);
}

TEST_CASE("MetricRegistry::sum_counters_with_prefix integration — agrees with manual snapshot scan") {
    MetricRegistry m;
    m.inc("svc_a_requests", 10);
    m.inc("svc_a_errors",    1);
    m.inc("svc_b_requests", 20);
    m.inc("svc_b_errors",    4);

    auto snap = m.counter_snapshot();
    // Reference impl: iterate the snapshot and sum matching keys.
    std::uint64_t expected = 0;
    for (const auto& [k, v] : snap) {
        if (std::string_view{k}.starts_with("svc_a_")) expected += v;
    }
    CHECK(m.sum_counters_with_prefix("svc_a_") == expected);
    CHECK(m.sum_counters_with_prefix("svc_a_") == 11);

    // Whole-registry sum via empty prefix matches counter_total().
    CHECK(m.sum_counters_with_prefix("") == m.counter_total());
}

// ---- MetricRegistry::counter_max ----------------------------------------

TEST_CASE("MetricRegistry::counter_max basic — returns the largest value") {
    MetricRegistry m;
    m.inc("a", 3);
    m.inc("b", 17);
    m.inc("c", 1);
    m.inc("d", 9);

    CHECK(m.counter_max() == 17);

    // Adding a strictly larger counter shifts the max.
    m.inc("e", 100);
    CHECK(m.counter_max() == 100);

    // Adding a smaller value does not change the max.
    m.inc("f", 4);
    CHECK(m.counter_max() == 100);
}

TEST_CASE("MetricRegistry::counter_max edge — 0 on empty registry / on histograms-only") {
    MetricRegistry m;
    // Empty registry → 0, matching counter_total() on empty.
    CHECK(m.counter_max() == 0);
    CHECK(m.counter_max() == m.counter_total());

    // A registered counter at 0 still exists; max stays at 0.
    m.inc("zeroed", 0);
    CHECK(m.counter_max() == 0);

    // Histograms must NOT contribute to counter_max.
    m.observe("latency", 0.5);
    m.observe("latency", 0.8);
    CHECK(m.counter_max() == 0);

    // Once a real counter exists, the max reflects it.
    m.inc("real", 7);
    CHECK(m.counter_max() == 7);
    // After clear the registry is empty again → max returns to 0.
    m.clear();
    CHECK(m.counter_max() == 0);
}

TEST_CASE("MetricRegistry::counter_max integration — bounded by counter_total, ≥ each counter_value") {
    MetricRegistry m;
    m.inc("alpha", 5);
    m.inc("beta",  12);
    m.inc("gamma", 8);

    const auto cmax = m.counter_max();
    // counter_max is always <= counter_total (sum dominates per-element
    // max for non-negative values).
    CHECK(cmax <= m.counter_total());
    // counter_max is >= every individual counter_value.
    for (const auto& name : m.list_counters()) {
        auto v = m.counter_value(name);
        REQUIRE(v);
        CHECK(v.value() <= cmax);
    }
    // For this fixture the max equals 12 (the beta counter).
    CHECK(cmax == 12);
}

// ---- DriftMonitor::is_registered ----------------------------------------

TEST_CASE("DriftMonitor::is_registered basic — true after register, false otherwise") {
    DriftMonitor dm;
    CHECK_FALSE(dm.is_registered("score"));

    REQUIRE(dm.register_feature("score", std::vector<double>(50, 0.4)));
    CHECK(dm.is_registered("score"));
    CHECK_FALSE(dm.is_registered("other"));
}

TEST_CASE("DriftMonitor::is_registered edge — agrees with has_feature, noexcept compile-time") {
    DriftMonitor dm;
    REQUIRE(dm.register_feature("a", std::vector<double>(20, 0.1)));
    REQUIRE(dm.register_feature("b", std::vector<double>(20, 0.5)));

    // is_registered is a sugar wrapper over has_feature: same answer
    // for every name we probe (registered, unregistered, empty).
    for (const auto& name : {"a", "b", "missing", ""}) {
        CHECK(dm.is_registered(name) == dm.has_feature(name));
    }

    // noexcept contract — verified at compile time. Match the existing
    // has_feature/has pattern: pass a string_view literal so the call
    // is unconditionally noexcept on libc++ and libstdc++.
    using namespace std::literals::string_view_literals;
    static_assert(noexcept(std::declval<const DriftMonitor&>().is_registered("x"sv)),
                  "DriftMonitor::is_registered must be noexcept");
}

TEST_CASE("DriftMonitor::is_registered integration — survives observe/reset, false after rotate? no — feature stays registered") {
    DriftMonitor dm;
    REQUIRE(dm.register_feature("score", std::vector<double>(100, 0.3)));
    CHECK(dm.is_registered("score"));

    // observe() does not unregister.
    for (int i = 0; i < 50; ++i) REQUIRE(dm.observe("score", 0.4));
    CHECK(dm.is_registered("score"));

    // reset(name) clears the current window but keeps registration.
    REQUIRE(dm.reset("score"));
    CHECK(dm.is_registered("score"));

    // rotate() also keeps registration (it just rebuilds the current
    // window for every feature).
    dm.rotate();
    CHECK(dm.is_registered("score"));
}

// ---- DriftMonitor::reset_all --------------------------------------------

TEST_CASE("DriftMonitor::reset_all basic — clears every feature's current window") {
    DriftMonitor dm;
    REQUIRE(dm.register_feature("a", std::vector<double>(100, 0.2)));
    REQUIRE(dm.register_feature("b", std::vector<double>(100, 0.7)));

    for (int i = 0; i < 50; ++i) REQUIRE(dm.observe("a", 0.5));
    for (int i = 0; i < 30; ++i) REQUIRE(dm.observe("b", 0.5));
    {
        auto ca = dm.observation_count("a");
        auto cb = dm.observation_count("b");
        REQUIRE(ca);
        REQUIRE(cb);
        CHECK(ca.value() == 50);
        CHECK(cb.value() == 30);
    }

    dm.reset_all();
    {
        auto ca = dm.observation_count("a");
        auto cb = dm.observation_count("b");
        REQUIRE(ca);
        REQUIRE(cb);
        CHECK(ca.value() == 0);
        CHECK(cb.value() == 0);
    }
    // Baselines untouched — registrations preserved.
    CHECK(dm.is_registered("a"));
    CHECK(dm.is_registered("b"));
    auto ba = dm.baseline_count("a");
    auto bb = dm.baseline_count("b");
    REQUIRE(ba);
    REQUIRE(bb);
    CHECK(ba.value() == 100);
    CHECK(bb.value() == 100);
}

TEST_CASE("DriftMonitor::reset_all edge — clears last_severity_ so alert sink re-fires from bottom") {
    DriftMonitor dm;
    REQUIRE(dm.register_feature("score", std::vector<double>(500, 0.2), 0.0, 1.0, 10));

    int fire_count = 0;
    dm.set_alert_sink(
        [&](const DriftReport&) { ++fire_count; },
        DriftSeverity::moder);

    // Drive severity from none → severe; sink fires exactly once.
    for (int i = 0; i < 500; ++i) REQUIRE(dm.observe("score", 0.85));
    CHECK(fire_count == 1);

    // reset_all clears BOTH the current window AND the last_severity_
    // map. After the reset, the next batch that drives severity back up
    // from the freshly-empty current window must fire the sink again
    // (the recorded "previous" severity has been forgotten — distinct
    // from rotate(), which only rebuilds histograms and leaves
    // last_severity_ intact).
    dm.reset_all();
    {
        auto c = dm.observation_count("score");
        REQUIRE(c);
        CHECK(c.value() == 0);
    }

    for (int i = 0; i < 500; ++i) REQUIRE(dm.observe("score", 0.85));
    CHECK(fire_count == 2);
}

TEST_CASE("DriftMonitor::reset_all integration — distinct from rotate(): rotate keeps last_severity_") {
    // rotate() rebuilds current histograms but does NOT clear
    // last_severity_. Verify the contrast: after rotate(), driving back
    // up to severe does NOT re-fire the sink (the last recorded severity
    // is still "severe"); after reset_all(), it DOES re-fire.
    DriftMonitor dm;
    REQUIRE(dm.register_feature("rot", std::vector<double>(500, 0.2), 0.0, 1.0, 10));

    int fire_count = 0;
    dm.set_alert_sink(
        [&](const DriftReport&) { ++fire_count; },
        DriftSeverity::moder);

    // Initial rise: none → severe.
    for (int i = 0; i < 500; ++i) REQUIRE(dm.observe("rot", 0.85));
    REQUIRE(fire_count == 1);

    // rotate() — current windows clear, but last_severity_ stays at
    // severe. New observations that re-establish severe must NOT fire.
    dm.rotate();
    for (int i = 0; i < 500; ++i) REQUIRE(dm.observe("rot", 0.85));
    CHECK(fire_count == 1);  // unchanged — rotate did NOT clear alerts

    // reset_all() — clears both windows AND last_severity_. New rise
    // back to severe is a fresh crossing → sink fires again.
    dm.reset_all();
    for (int i = 0; i < 500; ++i) REQUIRE(dm.observe("rot", 0.85));
    CHECK(fire_count == 2);

    // No-op safety: reset_all on a monitor with no features must not crash.
    DriftMonitor empty;
    empty.reset_all();
    CHECK(empty.feature_count() == 0);
}

// ---- Histogram::cumulative_at -------------------------------------------

TEST_CASE("Histogram::cumulative_at basic — running sum up through bin i") {
    Histogram h{0.0, 1.0, 4};
    // bin 0: 2 hits, bin 1: 1 hit, bin 2: 0 hits, bin 3: 5 hits.
    h.observe(0.05);
    h.observe(0.05);
    h.observe(0.30);
    for (int i = 0; i < 5; ++i) h.observe(0.95);
    REQUIRE(h.total() == 8);

    auto c0 = h.cumulative_at(0);
    REQUIRE(c0);
    CHECK(c0.value() == 2);

    auto c1 = h.cumulative_at(1);
    REQUIRE(c1);
    CHECK(c1.value() == 3);

    auto c2 = h.cumulative_at(2);
    REQUIRE(c2);
    CHECK(c2.value() == 3);  // bin 2 has 0 hits → cumulative unchanged

    auto c3 = h.cumulative_at(3);
    REQUIRE(c3);
    CHECK(c3.value() == 8);  // last bin → cumulative == total()
    CHECK(c3.value() == h.total());
}

TEST_CASE("Histogram::cumulative_at edge — out-of-range index returns invalid_argument") {
    Histogram h{0.0, 1.0, 4};
    h.observe(0.5);
    {
        auto r = h.cumulative_at(4);  // exactly bin_count()
        REQUIRE_FALSE(r);
        CHECK(r.error().code() == ErrorCode::invalid_argument);
    }
    {
        auto r = h.cumulative_at(123);
        REQUIRE_FALSE(r);
        CHECK(r.error().code() == ErrorCode::invalid_argument);
    }
    // Empty histogram: in-range index returns 0, out-of-range still
    // returns invalid_argument.
    Histogram empty{0.0, 1.0, 4};
    {
        auto r = empty.cumulative_at(0);
        REQUIRE(r);
        CHECK(r.value() == 0);
    }
    {
        auto r = empty.cumulative_at(3);
        REQUIRE(r);
        CHECK(r.value() == 0);
    }
    {
        auto r = empty.cumulative_at(4);
        REQUIRE_FALSE(r);
        CHECK(r.error().code() == ErrorCode::invalid_argument);
    }
}

TEST_CASE("Histogram::cumulative_at integration — agrees with bin_at sum and is non-decreasing") {
    Histogram h{0.0, 1.0, 8};
    std::mt19937 rng{1234};
    std::uniform_real_distribution<double> d{0.0, 1.0};
    for (int i = 0; i < 200; ++i) h.observe(d(rng));
    REQUIRE(h.total() == 200);

    // Reference impl: sum bin_at(0..i) and compare.
    std::uint64_t running = 0;
    std::uint64_t prev_cum = 0;
    for (std::size_t i = 0; i < h.bin_count(); ++i) {
        auto rb = h.bin_at(i);
        REQUIRE(rb);
        running += rb.value();

        auto rc = h.cumulative_at(i);
        REQUIRE(rc);
        CHECK(rc.value() == running);

        // Cumulative is non-decreasing in i: each step adds >= 0.
        CHECK(rc.value() >= prev_cum);
        prev_cum = rc.value();
    }
    // Final cumulative == total().
    auto rfinal = h.cumulative_at(h.bin_count() - 1);
    REQUIRE(rfinal);
    CHECK(rfinal.value() == h.total());

    // cdf agreement: cumulative_at(i) / total() must match cdf()[i] for
    // any non-empty histogram.
    auto cdf = h.cdf();
    REQUIRE(cdf.size() == h.bin_count());
    for (std::size_t i = 0; i < h.bin_count(); ++i) {
        auto rc = h.cumulative_at(i);
        REQUIRE(rc);
        const double expected = static_cast<double>(rc.value())
                              / static_cast<double>(h.total());
        CHECK(cdf[i] == doctest::Approx(expected));
    }
}

// ---- MetricRegistry::counter_with_default ------------------------------

TEST_CASE("MetricRegistry::counter_with_default basic — returns counter value when present") {
    MetricRegistry m;
    m.inc("hits", 7);

    CHECK(m.counter_with_default("hits", 999) == 7);

    // Default ignored even when value is 0 (counter exists, just zeroed).
    REQUIRE(m.reset("hits"));
    CHECK(m.counter_with_default("hits", 999) == 0);

    // Default returned when the counter genuinely doesn't exist.
    CHECK(m.counter_with_default("missing", 42)  == 42);
    CHECK(m.counter_with_default("missing", 0)   == 0);
    CHECK(m.counter_with_default("missing", 100) == 100);
}

TEST_CASE("MetricRegistry::counter_with_default edge — disambiguates missing from 0") {
    MetricRegistry m;
    // Pre-registered counter at 0.
    m.inc("zeroed", 0);
    REQUIRE(m.has_counter("zeroed"));

    // count() returns 0 for both registered-zero and missing → can't tell apart.
    CHECK(m.count("zeroed")  == 0);
    CHECK(m.count("missing") == 0);

    // counter_with_default with a non-zero default disambiguates: the
    // existing-but-zero counter still returns 0; the missing counter
    // returns the sentinel.
    constexpr std::uint64_t kSentinel = 9999;
    CHECK(m.counter_with_default("zeroed",  kSentinel) == 0);
    CHECK(m.counter_with_default("missing", kSentinel) == kSentinel);

    // Empty registry: every name returns the default.
    MetricRegistry empty;
    CHECK(empty.counter_with_default("anything", 7) == 7);
    CHECK(empty.counter_with_default("",         3) == 3);
}

TEST_CASE("MetricRegistry::counter_with_default integration — histograms satisfy the lookup, agrees with count()") {
    MetricRegistry m;
    m.inc("c", 5);
    m.observe("h", 0.1);
    m.observe("h", 0.2);
    m.observe("h", 0.3);

    // Counter present → returns its value (matches count()).
    CHECK(m.counter_with_default("c", 999) == 5);
    CHECK(m.counter_with_default("c", 999) == m.count("c"));

    // Histogram present → returns its observation count (per spec note:
    // "Histograms still satisfy this — return their count"). Matches
    // count()'s fall-through behaviour.
    CHECK(m.counter_with_default("h", 999) == 3);
    CHECK(m.counter_with_default("h", 999) == m.count("h"));

    // Missing → returns default_value, NOT count()'s silent 0.
    CHECK(m.counter_with_default("missing", 42) == 42);
    CHECK(m.count("missing") == 0);
    CHECK(m.counter_with_default("missing", 42) != m.count("missing"));

    // Register the previously-missing name; the default should no longer apply.
    m.inc("missing", 1);
    CHECK(m.counter_with_default("missing", 42) == 1);
}

// ---- Histogram::observed_range -----------------------------------------

TEST_CASE("Histogram::observed_range basic — agrees with min()/max() on populated histograms") {
    Histogram h{0.0, 10.0, 10};
    // Populate bins 2 (midpoint 2.5) and 7 (midpoint 7.5).
    for (int i = 0; i < 4; ++i) h.observe(2.5);
    for (int i = 0; i < 6; ++i) h.observe(7.5);

    auto [pmin, pmax] = h.observed_range();
    CHECK(pmin == doctest::Approx(2.5).epsilon(1e-9));
    CHECK(pmax == doctest::Approx(7.5).epsilon(1e-9));

    // Must agree with min()/max() — they all read the same bin midpoints.
    CHECK(pmin == doctest::Approx(h.min()).epsilon(1e-9));
    CHECK(pmax == doctest::Approx(h.max()).epsilon(1e-9));
}

TEST_CASE("Histogram::observed_range edge — empty histogram returns (lo, hi)") {
    Histogram h{0.0, 1.0, 10};
    REQUIRE(h.is_empty());

    auto [pmin, pmax] = h.observed_range();
    CHECK(pmin == doctest::Approx(0.0));
    CHECK(pmax == doctest::Approx(1.0));
    // Sentinel matches min()/max() empty-contract.
    CHECK(pmin == doctest::Approx(h.lo()));
    CHECK(pmax == doctest::Approx(h.hi()));

    // Off-zero range: empty observed_range == (lo, hi).
    Histogram off{-5.0, 15.0, 8};
    auto [omin, omax] = off.observed_range();
    CHECK(omin == doctest::Approx(-5.0));
    CHECK(omax == doctest::Approx(15.0));

    // Single bin populated → min == max == that bin's midpoint.
    Histogram one{0.0, 10.0, 10};
    for (int i = 0; i < 3; ++i) one.observe(4.5);  // bin 4, midpoint 4.5
    auto [smin, smax] = one.observed_range();
    CHECK(smin == doctest::Approx(4.5).epsilon(1e-9));
    CHECK(smax == doctest::Approx(4.5).epsilon(1e-9));
    CHECK(smin == smax);
}

TEST_CASE("Histogram::observed_range integration — invariants on .first/.second and survives clear/observe") {
    Histogram h{0.0, 1.0, 20};
    std::mt19937 rng{7};
    std::uniform_real_distribution<double> d{0.1, 0.9};
    for (int i = 0; i < 200; ++i) h.observe(d(rng));
    REQUIRE(h.total() == 200);

    auto pr = h.observed_range();
    // first <= second always.
    CHECK(pr.first  <= pr.second);
    // For non-empty histograms both endpoints lie in [lo, hi].
    CHECK(pr.first  >= h.lo());
    CHECK(pr.second <= h.hi());
    // mean lies between observed_range endpoints.
    CHECK(pr.first  <= h.mean());
    CHECK(h.mean() <= pr.second);
    // observed_range agrees with (min, max).
    CHECK(pr.first  == doctest::Approx(h.min()).epsilon(1e-9));
    CHECK(pr.second == doctest::Approx(h.max()).epsilon(1e-9));

    // After clear(), observed_range falls back to the (lo, hi) sentinel.
    h.clear();
    auto pr_empty = h.observed_range();
    CHECK(pr_empty.first  == doctest::Approx(h.lo()));
    CHECK(pr_empty.second == doctest::Approx(h.hi()));

    // After fresh observations, observed_range tracks the new min/max.
    h.observe(0.05);  // bin 1 (midpoint 0.075 in 20-bin layout)
    h.observe(0.95);  // bin 19 (midpoint 0.975)
    auto pr2 = h.observed_range();
    CHECK(pr2.first  == doctest::Approx(h.min()).epsilon(1e-9));
    CHECK(pr2.second == doctest::Approx(h.max()).epsilon(1e-9));
    CHECK(pr2.first  <  pr2.second);
}

// ---- DriftMonitor::feature_count_observed ------------------------------

TEST_CASE("DriftMonitor::feature_count_observed basic — counts only features with current observations") {
    DriftMonitor dm;
    REQUIRE(dm.register_feature("a", std::vector<double>(50, 0.2)));
    REQUIRE(dm.register_feature("b", std::vector<double>(50, 0.4)));
    REQUIRE(dm.register_feature("c", std::vector<double>(50, 0.7)));

    // No current-window observations yet → 0 (despite 3 registered).
    CHECK(dm.feature_count() == 3);
    CHECK(dm.feature_count_observed() == 0);

    // Observe into "a" only → 1 of 3 has been observed.
    REQUIRE(dm.observe("a", 0.5));
    CHECK(dm.feature_count_observed() == 1);

    // Add observations to "c" → 2 of 3.
    REQUIRE(dm.observe("c", 0.9));
    REQUIRE(dm.observe("c", 0.95));
    CHECK(dm.feature_count_observed() == 2);

    // Observe "b" → all 3 have been observed.
    REQUIRE(dm.observe("b", 0.3));
    CHECK(dm.feature_count_observed() == 3);
    CHECK(dm.feature_count_observed() == dm.feature_count());
}

TEST_CASE("DriftMonitor::feature_count_observed edge — empty monitor and reset interactions") {
    // Empty monitor → 0.
    DriftMonitor empty;
    CHECK(empty.feature_count_observed() == 0);

    DriftMonitor dm;
    REQUIRE(dm.register_feature("score", std::vector<double>(100, 0.3)));
    CHECK(dm.feature_count_observed() == 0);

    // After observation: 1.
    for (int i = 0; i < 10; ++i) REQUIRE(dm.observe("score", 0.4));
    CHECK(dm.feature_count_observed() == 1);

    // reset(name) clears the current window → drops back to 0 (the
    // baseline isn't current-window data).
    REQUIRE(dm.reset("score"));
    CHECK(dm.feature_count_observed() == 0);
    CHECK(dm.feature_count() == 1);  // registration preserved

    // Re-observe → 1 again.
    REQUIRE(dm.observe("score", 0.5));
    CHECK(dm.feature_count_observed() == 1);

    // rotate() also empties current windows → 0.
    dm.rotate();
    CHECK(dm.feature_count_observed() == 0);

    // reset_all() empties every current window → 0.
    REQUIRE(dm.observe("score", 0.5));
    CHECK(dm.feature_count_observed() == 1);
    dm.reset_all();
    CHECK(dm.feature_count_observed() == 0);
}

TEST_CASE("DriftMonitor::feature_count_observed integration — bounded by feature_count, agrees with manual scan") {
    DriftMonitor dm;
    REQUIRE(dm.register_feature("alpha", std::vector<double>(40, 0.1)));
    REQUIRE(dm.register_feature("beta",  std::vector<double>(40, 0.5)));
    REQUIRE(dm.register_feature("gamma", std::vector<double>(40, 0.9)));

    // Reference impl: enumerate via list_features and probe
    // observation_count for each one. Inlined per check site to avoid
    // depending on doctest assertions from inside a helper lambda.
    auto manual_scan = [](const DriftMonitor& d) {
        std::size_t n = 0;
        for (const auto& name : d.list_features()) {
            auto c = d.observation_count(name);
            if (c && c.value() > 0) ++n;
        }
        return n;
    };

    // No observations → 0.
    CHECK(dm.feature_count_observed() == manual_scan(dm));
    CHECK(dm.feature_count_observed() == 0);

    REQUIRE(dm.observe("alpha", 0.2));
    CHECK(dm.feature_count_observed() == manual_scan(dm));

    REQUIRE(dm.observe("gamma", 0.8));
    CHECK(dm.feature_count_observed() == manual_scan(dm));

    REQUIRE(dm.observe("beta", 0.5));
    CHECK(dm.feature_count_observed() == manual_scan(dm));

    // Bounded by feature_count.
    CHECK(dm.feature_count_observed() <= dm.feature_count());
    CHECK(dm.feature_count_observed() == 3);

    // observe_batch updates current totals; the count tracks correctly.
    dm.rotate();
    CHECK(dm.feature_count_observed() == 0);

    std::vector<double> batch{0.1, 0.2, 0.3, 0.4};
    REQUIRE(dm.observe_batch("alpha", batch));
    CHECK(dm.feature_count_observed() == 1);
    CHECK(dm.feature_count_observed() == manual_scan(dm));
}

// ---- MetricRegistry::reset_all_counters --------------------------------

TEST_CASE("MetricRegistry::reset_all_counters basic — zeroes every counter while preserving names") {
    MetricRegistry m;
    m.inc("a", 5);
    m.inc("b", 12);
    m.inc("c", 1);
    REQUIRE(m.counter_count() == 3);
    REQUIRE(m.counter_total() == 18);

    m.reset_all_counters();

    // Names preserved (count unchanged, list still has all three).
    CHECK(m.counter_count() == 3);
    CHECK(m.has_counter("a"));
    CHECK(m.has_counter("b"));
    CHECK(m.has_counter("c"));

    // Every value is now 0.
    CHECK(m.count("a") == 0);
    CHECK(m.count("b") == 0);
    CHECK(m.count("c") == 0);
    CHECK(m.counter_total() == 0);
    CHECK(m.counter_max() == 0);

    // counter_value still succeeds (registration intact) — distinguishes
    // reset_all_counters() from clear() which would drop the names entirely.
    auto va = m.counter_value("a");
    REQUIRE(va);
    CHECK(va.value() == 0);
}

TEST_CASE("MetricRegistry::reset_all_counters edge — distinct from clear(), does not touch histograms") {
    MetricRegistry m;
    m.inc("ctr", 7);
    m.observe("hist", 0.1);
    m.observe("hist", 0.2);
    m.observe("hist", 0.3);
    REQUIRE(m.counter_count() == 1);
    // 3 observations of "hist" → sum-of-obs == 3 under the new contract.
    REQUIRE(m.histogram_count_total() == 3);

    m.reset_all_counters();

    // Counter zeroed, name preserved.
    CHECK(m.counter_count() == 1);
    CHECK(m.has_counter("ctr"));
    CHECK(m.count("ctr") == 0);

    // Histogram untouched: name AND observations preserved (sum still 3).
    CHECK(m.histogram_count_total() == 3);
    CHECK(m.has_histogram("hist"));
    auto hc = m.histogram_count("hist");
    REQUIRE(hc);
    CHECK(hc.value() == 3);
    auto hs = m.histogram_sum("hist");
    REQUIRE(hs);
    CHECK(hs.value() == doctest::Approx(0.6).epsilon(1e-9));

    // Contrast: clear() drops EVERYTHING.
    m.clear();
    CHECK(m.counter_count() == 0);
    CHECK(m.histogram_count_total() == 0);
    CHECK_FALSE(m.has_counter("ctr"));
    CHECK_FALSE(m.has_histogram("hist"));

    // No-op safety: reset_all_counters on an empty registry must not crash.
    MetricRegistry empty;
    empty.reset_all_counters();
    CHECK(empty.counter_count() == 0);
    CHECK(empty.is_empty());
}

TEST_CASE("MetricRegistry::reset_all_counters integration — registry remains usable for new increments") {
    MetricRegistry m;
    m.inc("requests", 100);
    m.inc("errors",   3);
    m.inc("retries",  17);
    REQUIRE(m.counter_total() == 120);

    m.reset_all_counters();
    REQUIRE(m.counter_total() == 0);

    // After reset, the counters can be incremented again from 0 — no
    // re-registration required.
    m.inc("requests", 5);
    m.inc("errors");      // delta defaults to 1
    CHECK(m.count("requests") == 5);
    CHECK(m.count("errors")   == 1);
    CHECK(m.count("retries")  == 0);  // untouched since the reset

    // diff() against a pre-reset snapshot reflects the reset:
    // requests went 100 → 5, errors went 3 → 1, retries went 17 → 0.
    auto baseline = std::unordered_map<std::string, std::uint64_t>{
        {"requests", 100}, {"errors", 3}, {"retries", 17},
    };
    auto d = m.diff(baseline);
    CHECK(d.at("requests") == -95);
    CHECK(d.at("errors")   == -2);
    CHECK(d.at("retries")  == -17);

    // Names that survive the reset still pass the discoverability checks.
    for (const auto& name : {"requests", "errors", "retries"}) {
        CHECK(m.has_counter(name));
        CHECK(m.has(name));
    }
}

// ---- Histogram move construction / assignment (feature 1) ---------------

TEST_CASE("Histogram move ctor basic — moved-into copy carries lo/hi/bins/total") {
    Histogram src{0.0, 1.0, 10};
    for (int i = 0; i < 30; ++i) src.observe(0.25);
    for (int i = 0; i < 20; ++i) src.observe(0.75);
    REQUIRE(src.total() == 50);
    const auto src_mean = src.mean();
    const auto src_p50  = src.median();

    Histogram moved{std::move(src)};
    // Geometry preserved.
    CHECK(moved.lo()        == doctest::Approx(0.0));
    CHECK(moved.hi()        == doctest::Approx(1.0));
    CHECK(moved.bin_count() == 10);
    // Observation state preserved.
    CHECK(moved.total() == 50);
    CHECK(moved.mean()   == doctest::Approx(src_mean));
    CHECK(moved.median() == doctest::Approx(src_p50));
    // Per-bin contents preserved end-to-end.
    auto bin25 = moved.bin_at(2);  // [0.20, 0.30) — bin index 2
    REQUIRE(bin25);
    CHECK(bin25.value() == 30);
    auto bin75 = moved.bin_at(7);  // [0.70, 0.80) — bin index 7
    REQUIRE(bin75);
    CHECK(bin75.value() == 20);
}

TEST_CASE("Histogram move ctor edge — moved-from instance is emptied but usable") {
    Histogram src{0.0, 1.0, 10};
    for (int i = 0; i < 50; ++i) src.observe(0.5);
    REQUIRE(src.total() == 50);

    Histogram moved{std::move(src)};
    REQUIRE(moved.total() == 50);

    // The moved-from histogram must be safe to interact with: total() ==
    // 0, observe() works, clear() is a no-op, no UB on access.
    CHECK(src.total() == 0);
    CHECK(src.is_empty());
    src.observe(0.4);  // re-using a moved-from instance is legal
    CHECK(src.total() == 1);
    src.clear();
    CHECK(src.total() == 0);
    // The moved-into histogram is independent: src.observe() didn't touch it.
    CHECK(moved.total() == 50);
}

TEST_CASE("Histogram move assignment integration — return-by-value compiles & works") {
    // The audit module needs Histogram to be returnable from
    // `Result<Histogram>`-shaped getters, so this is the main contract
    // the move ops have to satisfy. We synthesize the same call shape
    // here: a function that constructs a Histogram and returns it by
    // value, then a Result<Histogram> round-trip.
    auto make_populated = [](double lo, double hi, std::size_t bins) -> Histogram {
        Histogram h{lo, hi, bins};
        for (int i = 0; i < 100; ++i) h.observe(lo + 0.5 * (hi - lo));
        return h;  // requires Histogram to be move-constructible
    };
    Histogram h = make_populated(0.0, 1.0, 8);
    CHECK(h.total() == 100);
    CHECK(h.mean() == doctest::Approx(0.5).epsilon(0.1));

    // Move assignment: replace `h` with a freshly-built one.
    h = make_populated(-1.0, 1.0, 16);
    CHECK(h.lo() == doctest::Approx(-1.0));
    CHECK(h.hi() == doctest::Approx( 1.0));
    CHECK(h.bin_count() == 16);
    CHECK(h.total() == 100);

    // Self-assignment is a no-op (and must not deadlock — the
    // implementation short-circuits before touching the mutex).
    Histogram& self_ref = h;
    h = std::move(self_ref);
    CHECK(h.total() == 100);

    // Result<Histogram> integration: pack a histogram into a Result and
    // unpack it. Verifies Histogram travels through the variant cleanly.
    auto producer = []() -> Result<Histogram> {
        Histogram out{0.0, 10.0, 5};
        for (int i = 0; i < 7; ++i) out.observe(2.5);
        return out;  // move into Result<T>
    };
    auto r = producer();
    REQUIRE(r);
    CHECK(r.value().total() == 7);
}

// ---- MetricRegistry::counter_min (feature 2) ----------------------------

TEST_CASE("MetricRegistry::counter_min basic — returns smallest counter value") {
    MetricRegistry m;
    m.inc("requests", 100);
    m.inc("errors",   3);
    m.inc("retries",  17);

    auto v = m.counter_min();
    REQUIRE(v);
    CHECK(v.value() == 3);

    // Adding a smaller counter shifts the minimum.
    m.inc("dropped", 1);
    auto v2 = m.counter_min();
    REQUIRE(v2);
    CHECK(v2.value() == 1);

    // 0 is a legal value; counter_min picks it up correctly.
    m.inc("zeros", 0);  // create-on-first-use leaves the value at 0
    auto v3 = m.counter_min();
    REQUIRE(v3);
    CHECK(v3.value() == 0);
}

TEST_CASE("MetricRegistry::counter_min edge — not_found on empty registry") {
    MetricRegistry m;
    auto v = m.counter_min();
    REQUIRE_FALSE(v);
    CHECK(v.error().code() == ErrorCode::not_found);

    // Registry containing only histograms (no counters) is also empty
    // for counter_min purposes.
    m.observe("latency", 0.001);
    auto v2 = m.counter_min();
    REQUIRE_FALSE(v2);
    CHECK(v2.error().code() == ErrorCode::not_found);

    // After dropping all counters via reset_all_counters() the names
    // are still present (just zeroed), so counter_min should now return
    // 0 rather than not_found — distinguishing "no counters" from
    // "all counters at 0".
    m.inc("placeholder", 5);
    REQUIRE(m.counter_count() == 1);
    m.reset_all_counters();
    auto v3 = m.counter_min();
    REQUIRE(v3);
    CHECK(v3.value() == 0);
}

TEST_CASE("MetricRegistry::counter_min integration — bounded by counter_max, agrees with snapshot") {
    MetricRegistry m;
    m.inc("a", 12);
    m.inc("b", 4);
    m.inc("c", 27);
    m.inc("d", 9);

    auto vmin = m.counter_min();
    REQUIRE(vmin);
    const auto vmax = m.counter_max();
    // counter_min ≤ counter_max for any non-empty registry.
    CHECK(vmin.value() <= vmax);
    CHECK(vmax == 27);
    CHECK(vmin.value() == 4);

    // counter_min should match the manual scan across counter_snapshot().
    auto snap = m.counter_snapshot();
    REQUIRE_FALSE(snap.empty());
    std::uint64_t manual_min = snap.begin()->second;
    for (const auto& [k, v] : snap) {
        if (v < manual_min) manual_min = v;
    }
    CHECK(vmin.value() == manual_min);

    // Removing the smallest counter via reset() makes its value 0,
    // which becomes the new min (since the name is still registered).
    auto reset_ok = m.reset("b");
    REQUIRE(reset_ok);
    auto vmin2 = m.counter_min();
    REQUIRE(vmin2);
    CHECK(vmin2.value() == 0);
}

// ---- MetricRegistry::has_any (feature 3) --------------------------------

TEST_CASE("MetricRegistry::has_any basic — true once any metric is recorded") {
    MetricRegistry m;
    CHECK_FALSE(m.has_any());
    CHECK(m.is_empty());

    m.inc("ctr", 1);
    CHECK(m.has_any());
    CHECK_FALSE(m.is_empty());

    // After clear, has_any flips back to false.
    m.clear();
    CHECK_FALSE(m.has_any());
    CHECK(m.is_empty());
}

TEST_CASE("MetricRegistry::has_any edge — true for histogram-only registry; noexcept compile-time") {
    MetricRegistry m;
    m.observe("latency", 0.001);
    CHECK(m.has_any());
    CHECK_FALSE(m.is_empty());

    m.reset_histograms();
    CHECK_FALSE(m.has_any());

    // Counter-only registry: has_any true even when counter is at 0.
    m.inc("placeholder", 0);
    CHECK(m.has_any());
    m.reset_all_counters();
    // reset_all_counters preserves names (data zeroed), so has_any
    // stays true.
    CHECK(m.has_any());

    // noexcept contract — verified at compile time. Match the existing
    // noexcept-static_assert pattern used for is_empty()/has().
    static_assert(noexcept(std::declval<const MetricRegistry&>().has_any()),
                  "MetricRegistry::has_any must be noexcept");
}

TEST_CASE("MetricRegistry::has_any integration — bitwise opposite of is_empty across transitions") {
    MetricRegistry m;
    auto check_invariant = [&]() {
        CHECK(m.has_any() == !m.is_empty());
    };

    check_invariant();  // both empty
    m.inc("a", 5);
    check_invariant();
    m.observe("h", 0.1);
    check_invariant();
    m.reset_all_counters();
    check_invariant();  // counters zeroed but names live; histogram lives
    m.reset_histograms();
    check_invariant();  // histograms gone; counter names still live → has_any
    m.clear();
    check_invariant();  // all gone → !has_any
}

// ---- DriftMonitor::set_alert_threshold (feature 4) ----------------------

TEST_CASE("DriftMonitor::set_alert_threshold basic — re-tunes threshold without changing sink") {
    DriftMonitor dm;
    REQUIRE(dm.register_feature("f", std::vector<double>(50, 0.1), 0.0, 1.0, 10));

    int fires = 0;
    dm.set_alert_sink([&](const DriftReport&) { ++fires; }, DriftSeverity::severe);
    REQUIRE(dm.has_alert_sink());

    // Lower the threshold to "minor". The sink stays installed.
    dm.set_alert_threshold(DriftSeverity::minor);
    CHECK(dm.has_alert_sink());

    // Now an observation that would have been below "severe" can fire
    // under the new "minor" threshold. Observations far from the
    // baseline drive the PSI up.
    for (int i = 0; i < 30; ++i) {
        REQUIRE(dm.observe("f", 0.95));
    }
    CHECK(fires >= 1);
}

TEST_CASE("DriftMonitor::set_alert_threshold edge — does NOT clear last_severity_") {
    DriftMonitor dm;
    REQUIRE(dm.register_feature("f", std::vector<double>(50, 0.1), 0.0, 1.0, 10));

    int fires = 0;
    dm.set_alert_sink([&](const DriftReport&) { ++fires; }, DriftSeverity::moder);

    // Drive PSI past the moder threshold. Sink fires once on the rising
    // edge.
    for (int i = 0; i < 60; ++i) {
        REQUIRE(dm.observe("f", 0.95));
    }
    REQUIRE(fires >= 1);
    const int after_first = fires;

    // Bump the threshold UP to severe — last_severity_ retains the
    // already-recorded "moder" mark, and the implementation explicitly
    // does NOT clear the alert ladder. Subsequent observations that
    // hold steady at the same severity should NOT cause a re-fire.
    dm.set_alert_threshold(DriftSeverity::severe);
    for (int i = 0; i < 5; ++i) {
        REQUIRE(dm.observe("f", 0.95));
    }
    // We can't say much about the absolute fire count (severity may
    // rise to severe and fire once more), but we can assert the
    // last_severity_ map was NOT cleared by set_alert_threshold —
    // verify via clear_alerts() pairing: a follow-up clear_alerts()
    // SHOULD allow re-fires from the bottom of the ladder.
    const int before_clear = fires;
    dm.clear_alerts();
    dm.set_alert_threshold(DriftSeverity::minor);
    for (int i = 0; i < 30; ++i) {
        REQUIRE(dm.observe("f", 0.95));
    }
    CHECK(fires > before_clear);
}

TEST_CASE("DriftMonitor::set_alert_threshold integration — preserves sink identity and survives no-op tunes") {
    DriftMonitor dm;
    REQUIRE(dm.register_feature("f", std::vector<double>(50, 0.5), 0.0, 1.0, 10));

    int fires = 0;
    dm.set_alert_sink([&](const DriftReport&) { ++fires; }, DriftSeverity::moder);
    REQUIRE(dm.has_alert_sink());

    // Re-tune to the SAME threshold — no-op, sink stays.
    dm.set_alert_threshold(DriftSeverity::moder);
    CHECK(dm.has_alert_sink());

    // Re-tune through several values; sink must remain installed.
    dm.set_alert_threshold(DriftSeverity::none);
    CHECK(dm.has_alert_sink());
    dm.set_alert_threshold(DriftSeverity::severe);
    CHECK(dm.has_alert_sink());
    dm.set_alert_threshold(DriftSeverity::minor);
    CHECK(dm.has_alert_sink());

    // The sink must still be wired to the same callable (we can't peek
    // identity, but we can prove it fires by driving drift).
    for (int i = 0; i < 60; ++i) {
        REQUIRE(dm.observe("f", 0.95));
    }
    CHECK(fires >= 1);
}

// ---- Histogram::scale_by (feature 5) ------------------------------------

TEST_CASE("Histogram::scale_by basic — multiplies every bin by the factor") {
    Histogram h{0.0, 1.0, 4};
    for (int i = 0; i < 10; ++i) h.observe(0.1);   // bin 0
    for (int i = 0; i < 20; ++i) h.observe(0.4);   // bin 1
    for (int i = 0; i < 30; ++i) h.observe(0.7);   // bin 2
    REQUIRE(h.total() == 60);

    auto ok = h.scale_by(0.5);
    REQUIRE(ok);
    // Every bin is halved (with floor).
    auto b0 = h.bin_at(0); REQUIRE(b0); CHECK(b0.value() == 5);
    auto b1 = h.bin_at(1); REQUIRE(b1); CHECK(b1.value() == 10);
    auto b2 = h.bin_at(2); REQUIRE(b2); CHECK(b2.value() == 15);
    auto b3 = h.bin_at(3); REQUIRE(b3); CHECK(b3.value() == 0);
    // total_ matches the post-scale bin sum.
    CHECK(h.total() == 30);
}

TEST_CASE("Histogram::scale_by edge — factor==0 clears, factor<0 returns invalid_argument") {
    Histogram h{0.0, 1.0, 4};
    for (int i = 0; i < 17; ++i) h.observe(0.25);
    REQUIRE(h.total() == 17);

    // factor == 0 is a "clear via scale" — every count becomes 0.
    auto z = h.scale_by(0.0);
    REQUIRE(z);
    CHECK(h.total() == 0);
    CHECK(h.is_empty());
    auto b = h.bin_at(1); REQUIRE(b); CHECK(b.value() == 0);

    // factor < 0 is an invalid argument; histogram unchanged.
    for (int i = 0; i < 5; ++i) h.observe(0.25);
    REQUIRE(h.total() == 5);
    auto bad = h.scale_by(-0.5);
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code() == ErrorCode::invalid_argument);
    CHECK(h.total() == 5);  // untouched

    // factor == 1.0 is effectively a no-op (the floor-trunc only matters
    // for boundary-rational scales).
    auto noop = h.scale_by(1.0);
    REQUIRE(noop);
    CHECK(h.total() == 5);
}

TEST_CASE("Histogram::scale_by integration — preserves geometry; downsample-then-merge weighting") {
    Histogram a{0.0, 1.0, 10};
    Histogram b{0.0, 1.0, 10};
    for (int i = 0; i < 100; ++i) a.observe(0.25);  // 100 in bin 2
    for (int i = 0; i < 100; ++i) b.observe(0.75);  // 100 in bin 7

    // Downweight `a` to a quarter before merging; merged total should
    // be 25 (from a) + 100 (from b) = 125.
    REQUIRE(a.scale_by(0.25));
    CHECK(a.total() == 25);
    // Geometry is preserved (lo/hi/bin_count) — scale_by only touches
    // counts and total.
    CHECK(a.lo() == doctest::Approx(0.0));
    CHECK(a.hi() == doctest::Approx(1.0));
    CHECK(a.bin_count() == 10);

    REQUIRE(b.merge(a));
    CHECK(b.total() == 125);
    auto bin2 = b.bin_at(2); REQUIRE(bin2); CHECK(bin2.value() == 25);
    auto bin7 = b.bin_at(7); REQUIRE(bin7); CHECK(bin7.value() == 100);

    // Floor semantics: 7 * 0.5 = 3.5 → 3, 9 * 0.5 = 4.5 → 4. Verify by
    // building a histogram with odd bin counts and scaling.
    Histogram h{0.0, 1.0, 4};
    for (int i = 0; i < 7; ++i) h.observe(0.1);
    for (int i = 0; i < 9; ++i) h.observe(0.4);
    REQUIRE(h.total() == 16);
    REQUIRE(h.scale_by(0.5));
    auto b0 = h.bin_at(0); REQUIRE(b0); CHECK(b0.value() == 3);  // floor(3.5)
    auto b1 = h.bin_at(1); REQUIRE(b1); CHECK(b1.value() == 4);  // floor(4.5)
    // total_ = 3 + 4 = 7, NOT floor(16 * 0.5) = 8 — because the floor
    // remainders accumulated below the per-bucket boundary.
    CHECK(h.total() == 7);
}

// ============== Histogram::clone =========================================

TEST_CASE("Histogram::clone produces a deep copy with identical content") {
    Histogram src{0.0, 1.0, 8};
    for (int i = 0; i < 10; ++i) src.observe(0.1);
    for (int i = 0; i < 5;  ++i) src.observe(0.6);
    REQUIRE(src.total() == 15);

    Histogram dst = src.clone();
    CHECK(dst.lo()        == doctest::Approx(src.lo()));
    CHECK(dst.hi()        == doctest::Approx(src.hi()));
    CHECK(dst.bin_count() == src.bin_count());
    CHECK(dst.total()     == src.total());

    // Bin-wise content equal.
    for (std::size_t i = 0; i < src.bin_count(); ++i) {
        auto a = src.bin_at(i); REQUIRE(a);
        auto b = dst.bin_at(i); REQUIRE(b);
        CHECK(a.value() == b.value());
    }
}

TEST_CASE("Histogram::clone: source is unchanged after clone (distinct from move)") {
    Histogram src{-1.0, 1.0, 4};
    for (int i = 0; i < 7; ++i) src.observe(0.5);
    REQUIRE(src.total() == 7);

    Histogram dst = src.clone();

    // Source still observable, still has its observations.
    CHECK(src.total() == 7);
    src.observe(0.5);
    CHECK(src.total() == 8);
    // Mutating source did NOT touch the clone.
    CHECK(dst.total() == 7);
}

TEST_CASE("Histogram::clone: clone is independently mutable") {
    Histogram src{0.0, 1.0, 4};
    for (int i = 0; i < 4; ++i) src.observe(0.125);  // bin 0
    for (int i = 0; i < 4; ++i) src.observe(0.875);  // bin 3

    Histogram dst = src.clone();
    REQUIRE(dst.total() == 8);

    // Clearing the clone leaves the source intact.
    dst.clear();
    CHECK(dst.total() == 0);
    CHECK(src.total() == 8);

    // Scaling the clone leaves the source intact.
    Histogram dst2 = src.clone();
    REQUIRE(dst2.scale_by(0.5));
    CHECK(dst2.total() == 4);   // floor(4*0.5)+floor(4*0.5)=2+2=4
    CHECK(src.total()  == 8);   // untouched
}

TEST_CASE("Histogram::clone of an empty histogram preserves geometry") {
    Histogram src{2.0, 5.0, 6};
    Histogram dst = src.clone();
    CHECK(dst.lo()        == doctest::Approx(2.0));
    CHECK(dst.hi()        == doctest::Approx(5.0));
    CHECK(dst.bin_count() == 6);
    CHECK(dst.total()     == 0);
    CHECK(dst.is_empty());
}

// ============== Histogram::with_added ====================================

TEST_CASE("Histogram::with_added returns bin-wise sum, leaves both inputs unchanged") {
    Histogram a{0.0, 1.0, 4};
    Histogram b{0.0, 1.0, 4};
    for (int i = 0; i < 3; ++i) a.observe(0.1);   // bin 0
    for (int i = 0; i < 5; ++i) a.observe(0.6);   // bin 2
    for (int i = 0; i < 2; ++i) b.observe(0.1);   // bin 0
    for (int i = 0; i < 4; ++i) b.observe(0.85);  // bin 3
    REQUIRE(a.total() == 8);
    REQUIRE(b.total() == 6);

    Histogram c = a.with_added(b);

    // a + b totals.
    CHECK(c.total() == 14);
    // Bin-wise sums.
    auto c0 = c.bin_at(0); REQUIRE(c0); CHECK(c0.value() == 5);  // 3+2
    auto c1 = c.bin_at(1); REQUIRE(c1); CHECK(c1.value() == 0);
    auto c2 = c.bin_at(2); REQUIRE(c2); CHECK(c2.value() == 5);  // 5+0
    auto c3 = c.bin_at(3); REQUIRE(c3); CHECK(c3.value() == 4);  // 0+4

    // Inputs untouched (this is the contract that distinguishes
    // with_added() from merge()).
    CHECK(a.total() == 8);
    CHECK(b.total() == 6);
}

TEST_CASE("Histogram::with_added on geometry mismatch returns empty with this's geometry") {
    Histogram a{0.0, 1.0, 4};
    Histogram b{0.0, 1.0, 8};   // different bin count
    for (int i = 0; i < 3; ++i) a.observe(0.5);
    for (int i = 0; i < 7; ++i) b.observe(0.5);
    REQUIRE(a.total() == 3);
    REQUIRE(b.total() == 7);

    Histogram c = a.with_added(b);

    // Empty histogram with a's geometry.
    CHECK(c.lo()        == doctest::Approx(a.lo()));
    CHECK(c.hi()        == doctest::Approx(a.hi()));
    CHECK(c.bin_count() == a.bin_count());
    CHECK(c.total()     == 0);
    CHECK(c.is_empty());

    // Inputs still untouched.
    CHECK(a.total() == 3);
    CHECK(b.total() == 7);

    // Same geometry mismatch via lo/hi.
    Histogram d{1.0, 2.0, 4};   // same bins, different lo/hi
    Histogram e = a.with_added(d);
    CHECK(e.lo()        == doctest::Approx(a.lo()));
    CHECK(e.hi()        == doctest::Approx(a.hi()));
    CHECK(e.bin_count() == a.bin_count());
    CHECK(e.total()     == 0);
}

TEST_CASE("Histogram::with_added with self returns doubled counts") {
    Histogram a{0.0, 1.0, 4};
    for (int i = 0; i < 6; ++i) a.observe(0.125);
    REQUIRE(a.total() == 6);

    Histogram c = a.with_added(a);

    CHECK(c.total() == 12);
    auto c0 = c.bin_at(0); REQUIRE(c0); CHECK(c0.value() == 12);
    // Source unchanged.
    CHECK(a.total() == 6);
}

TEST_CASE("Histogram::with_added composes with merge — equivalent results") {
    Histogram a{0.0, 1.0, 4};
    Histogram b{0.0, 1.0, 4};
    for (int i = 0; i < 5; ++i) a.observe(0.125);
    for (int i = 0; i < 3; ++i) b.observe(0.625);

    // Path 1: with_added (non-mutating).
    Histogram fused = a.with_added(b);

    // Path 2: clone + merge (mutating into a copy).
    Histogram fused2 = a.clone();
    REQUIRE(fused2.merge(b));

    // The two routes must produce the same totals + per-bin counts.
    CHECK(fused.total() == fused2.total());
    for (std::size_t i = 0; i < fused.bin_count(); ++i) {
        auto x = fused.bin_at(i);  REQUIRE(x);
        auto y = fused2.bin_at(i); REQUIRE(y);
        CHECK(x.value() == y.value());
    }

    // Inputs to the non-mutating path remain intact.
    CHECK(a.total() == 5);
    CHECK(b.total() == 3);
}

// ============== MetricRegistry::counter_names_with_prefix ================

TEST_CASE("counter_names_with_prefix returns sorted names matching prefix") {
    MetricRegistry m;
    m.inc("policy.scrubber.hits");
    m.inc("policy.length.violations");
    m.inc("ledger.appends");
    m.inc("policy.action.blocks");
    m.inc("ledger.verifies");

    auto names = m.counter_names_with_prefix("policy.");
    REQUIRE(names.size() == 3);
    // Alphabetical order.
    CHECK(names[0] == "policy.action.blocks");
    CHECK(names[1] == "policy.length.violations");
    CHECK(names[2] == "policy.scrubber.hits");
}

TEST_CASE("counter_names_with_prefix: empty prefix returns every counter sorted") {
    MetricRegistry m;
    m.inc("zeta");
    m.inc("alpha");
    m.inc("mu");

    auto names = m.counter_names_with_prefix("");
    REQUIRE(names.size() == 3);
    CHECK(names[0] == "alpha");
    CHECK(names[1] == "mu");
    CHECK(names[2] == "zeta");

    // Equivalent to all_counter_names().
    auto all = m.all_counter_names();
    CHECK(names == all);
}

TEST_CASE("counter_names_with_prefix: no matches returns empty vector") {
    MetricRegistry m;
    m.inc("foo");
    m.inc("bar");
    auto names = m.counter_names_with_prefix("nope.");
    CHECK(names.empty());

    // An empty registry also returns empty for any prefix.
    MetricRegistry empty;
    CHECK(empty.counter_names_with_prefix("").empty());
    CHECK(empty.counter_names_with_prefix("anything").empty());
}

TEST_CASE("counter_names_with_prefix ignores histograms") {
    MetricRegistry m;
    m.inc("policy.hits", 1);
    m.observe("policy.latency_seconds", 0.05);  // histogram, NOT a counter

    auto names = m.counter_names_with_prefix("policy.");
    REQUIRE(names.size() == 1);
    CHECK(names[0] == "policy.hits");
}

// ============== DriftMonitor::list_severe_features =======================

TEST_CASE("list_severe_features empty when no features registered") {
    DriftMonitor dm;
    auto v = dm.list_severe_features();
    CHECK(v.empty());
}

TEST_CASE("list_severe_features empty when no feature is severe") {
    DriftMonitor dm;
    std::vector<double> baseline(200, 0.5);
    REQUIRE(dm.register_feature("calm", baseline, 0.0, 1.0, 10));
    // Observe values close to the baseline — PSI stays low.
    for (int i = 0; i < 200; ++i) REQUIRE(dm.observe("calm", 0.5));

    auto v = dm.list_severe_features();
    CHECK(v.empty());
}

TEST_CASE("list_severe_features lists severe features in alphabetical order") {
    DriftMonitor dm;
    std::vector<double> baseline(200, 0.5);
    REQUIRE(dm.register_feature("zulu",  baseline, 0.0, 1.0, 10));
    REQUIRE(dm.register_feature("alpha", baseline, 0.0, 1.0, 10));
    REQUIRE(dm.register_feature("mike",  baseline, 0.0, 1.0, 10));
    REQUIRE(dm.register_feature("calm",  baseline, 0.0, 1.0, 10));

    // Drift three features hard (PSI > 0.5 → severe). Feed "calm"
    // baseline-matching observations so it stays at PSI ~ 0 (otherwise
    // an empty current window classifies as severe via eps-clamped PSI).
    for (int i = 0; i < 200; ++i) {
        REQUIRE(dm.observe("zulu",  0.05));
        REQUIRE(dm.observe("alpha", 0.05));
        REQUIRE(dm.observe("mike",  0.05));
        REQUIRE(dm.observe("calm",  0.5));
    }

    auto v = dm.list_severe_features();
    REQUIRE(v.size() == 3);
    // Alphabetical order — distinguishing this from list_features() (unspecified).
    CHECK(v[0] == "alpha");
    CHECK(v[1] == "mike");
    CHECK(v[2] == "zulu");
}

TEST_CASE("list_severe_features matches report() filtered by severity == severe") {
    DriftMonitor dm;
    std::vector<double> baseline(200, 0.5);
    REQUIRE(dm.register_feature("a", baseline, 0.0, 1.0, 10));
    REQUIRE(dm.register_feature("b", baseline, 0.0, 1.0, 10));
    REQUIRE(dm.register_feature("c", baseline, 0.0, 1.0, 10));

    // Only push "a" and "c" to severe.
    for (int i = 0; i < 200; ++i) {
        REQUIRE(dm.observe("a", 0.05));
        REQUIRE(dm.observe("b", 0.5));   // stays calm
        REQUIRE(dm.observe("c", 0.95));
    }

    auto severe_names = dm.list_severe_features();
    auto rep          = dm.report();

    // Pull severe names from report() into a sorted set for comparison.
    std::vector<std::string> from_report;
    for (const auto& r : rep) {
        if (r.severity == DriftSeverity::severe) {
            from_report.push_back(r.feature);
        }
    }
    std::sort(from_report.begin(), from_report.end());

    CHECK(severe_names == from_report);
    REQUIRE(severe_names.size() == 2);
    CHECK(severe_names[0] == "a");
    CHECK(severe_names[1] == "c");
}

// ============== DriftMonitor::has_any_severe =============================

TEST_CASE("has_any_severe is false on an empty monitor") {
    DriftMonitor dm;
    CHECK_FALSE(dm.has_any_severe());
}

TEST_CASE("has_any_severe is false when no feature classifies as severe") {
    DriftMonitor dm;
    std::vector<double> baseline(200, 0.5);
    REQUIRE(dm.register_feature("a", baseline, 0.0, 1.0, 10));
    REQUIRE(dm.register_feature("b", baseline, 0.0, 1.0, 10));
    // Tiny perturbation; PSI stays well below the severe threshold.
    for (int i = 0; i < 200; ++i) {
        REQUIRE(dm.observe("a", 0.5));
        REQUIRE(dm.observe("b", 0.5));
    }
    CHECK_FALSE(dm.has_any_severe());
}

TEST_CASE("has_any_severe is true when at least one feature is severe") {
    DriftMonitor dm;
    std::vector<double> baseline(200, 0.5);
    REQUIRE(dm.register_feature("calm",   baseline, 0.0, 1.0, 10));
    REQUIRE(dm.register_feature("broken", baseline, 0.0, 1.0, 10));

    // Push only "broken" past severe (PSI > 0.5).
    for (int i = 0; i < 200; ++i) {
        REQUIRE(dm.observe("calm",   0.5));
        REQUIRE(dm.observe("broken", 0.05));
    }
    CHECK(dm.has_any_severe());

    // And it agrees with list_severe_features() / any_severe().
    CHECK(dm.any_severe());
    CHECK_FALSE(dm.list_severe_features().empty());
}

TEST_CASE("has_any_severe is noexcept and consistent with list_severe_features") {
    DriftMonitor dm;
    // Compile-time noexcept assertion on the method's contract.
    static_assert(noexcept(dm.has_any_severe()));

    std::vector<double> baseline(200, 0.5);
    REQUIRE(dm.register_feature("x", baseline, 0.0, 1.0, 10));
    REQUIRE(dm.register_feature("y", baseline, 0.0, 1.0, 10));

    // Step 1: feed baseline-matching observations to both → no severity.
    // (Empty current windows would themselves classify as severe via the
    // eps-clamped PSI; we observe the baseline value first to keep PSI
    // low and isolate the "no severity" branch.)
    for (int i = 0; i < 200; ++i) {
        REQUIRE(dm.observe("x", 0.5));
        REQUIRE(dm.observe("y", 0.5));
    }
    CHECK_FALSE(dm.has_any_severe());
    CHECK(dm.list_severe_features().empty());

    // Step 2: pivot y hard to one tail → bool flips, list contains y only.
    REQUIRE(dm.reset("y"));
    for (int i = 0; i < 200; ++i) REQUIRE(dm.observe("y", 0.05));
    CHECK(dm.has_any_severe());
    auto v = dm.list_severe_features();
    REQUIRE(v.size() == 1);
    CHECK(v[0] == "y");

    // Step 3: bool agrees with the cheaper non-noexcept any_severe()
    // and with !list_severe_features().empty(). Cross-check both.
    CHECK(dm.has_any_severe() == dm.any_severe());
    CHECK(dm.has_any_severe() == !dm.list_severe_features().empty());
}

// ============== MetricRegistry::counters_above ==========================

TEST_CASE("MetricRegistry::counters_above filters by strict-greater threshold") {
    MetricRegistry m;
    m.inc("low",   3);
    m.inc("mid",   10);
    m.inc("high",  42);
    m.inc("zero",  0);  // Counter exists but value 0.

    // threshold=0 → every NON-ZERO counter (strict >); "zero" omitted.
    auto above0 = m.counters_above(0);
    REQUIRE(above0.size() == 3);
    // Sorted alphabetically: "high", "low", "mid".
    CHECK(above0[0] == "high");
    CHECK(above0[1] == "low");
    CHECK(above0[2] == "mid");

    // threshold=10 → strict-greater means "mid" (==10) is omitted, only "high".
    auto above10 = m.counters_above(10);
    REQUIRE(above10.size() == 1);
    CHECK(above10[0] == "high");

    // threshold=100 → no counter exceeds 100.
    auto above100 = m.counters_above(100);
    CHECK(above100.empty());
}

TEST_CASE("MetricRegistry::counters_above returns empty on empty registry") {
    MetricRegistry m;
    CHECK(m.counters_above(0).empty());
    CHECK(m.counters_above(1000).empty());

    // Add and remove → still empty.
    m.inc("only_one");
    REQUIRE(m.reset("only_one"));   // value 0 now
    auto v = m.counters_above(0);
    CHECK(v.empty());               // strictly above 0 → none
}

TEST_CASE("MetricRegistry::counters_above output is alphabetically sorted") {
    MetricRegistry m;
    // Insert in a non-alphabetical order; result must still be sorted.
    m.inc("zeta",  5);
    m.inc("alpha", 5);
    m.inc("mu",    5);
    m.inc("beta",  5);

    auto v = m.counters_above(0);
    REQUIRE(v.size() == 4);
    CHECK(v[0] == "alpha");
    CHECK(v[1] == "beta");
    CHECK(v[2] == "mu");
    CHECK(v[3] == "zeta");
}

TEST_CASE("MetricRegistry::counters_above ignores histograms") {
    MetricRegistry m;
    m.inc("ctr", 100);
    m.observe("hist", 0.5);     // histogram, not a counter
    m.observe("hist", 0.6);

    auto v = m.counters_above(0);
    REQUIRE(v.size() == 1);
    CHECK(v[0] == "ctr");       // "hist" is a histogram, must not appear
}

// ============== Histogram::observed_max_bin_count =======================

TEST_CASE("Histogram::observed_max_bin_count returns 0 on empty histogram") {
    Histogram h{0.0, 1.0, 10};
    CHECK(h.observed_max_bin_count() == 0);

    Histogram other{-5.0, 5.0, 50};
    CHECK(other.observed_max_bin_count() == 0);
}

TEST_CASE("Histogram::observed_max_bin_count returns count of the heaviest bin") {
    Histogram h{0.0, 10.0, 10};
    // bin 0 (mid=0.5) gets 7 obs, bin 5 (mid=5.5) gets 3 obs, others 0.
    for (int i = 0; i < 7; ++i) h.observe(0.5);
    for (int i = 0; i < 3; ++i) h.observe(5.5);

    CHECK(h.observed_max_bin_count() == 7);
    // Cross-check against the modal bin's index — they should agree on
    // which bin is heaviest.
    CHECK(h.nth_largest_bin() == 0);
}

TEST_CASE("Histogram::observed_max_bin_count tracks shifting modal bin under observe") {
    Histogram h{0.0, 10.0, 10};
    // Initially every observation in bin 1 (mid=1.5).
    for (int i = 0; i < 5; ++i) h.observe(1.5);
    CHECK(h.observed_max_bin_count() == 5);

    // Pile a larger amount into a different bin.
    for (int i = 0; i < 12; ++i) h.observe(7.5);  // bin 7
    CHECK(h.observed_max_bin_count() == 12);
    CHECK(h.nth_largest_bin() == 7);

    // Add still more to the original bin so it overtakes.
    for (int i = 0; i < 20; ++i) h.observe(1.5);  // bin 1 → 25 obs total
    CHECK(h.observed_max_bin_count() == 25);
    CHECK(h.nth_largest_bin() == 1);
}

TEST_CASE("Histogram::observed_max_bin_count <= total()") {
    // Invariant: the heaviest bin can't have more observations than total.
    Histogram h{0.0, 1.0, 5};
    for (double v : {0.05, 0.05, 0.45, 0.95, 0.95, 0.95}) h.observe(v);
    CHECK(h.observed_max_bin_count() <= h.total());
    CHECK(h.observed_max_bin_count() == 3);  // three obs at 0.95 → bin 4
    CHECK(h.total() == 6);
}

// ============== DriftMonitor::worst_psi_feature =========================

TEST_CASE("DriftMonitor::worst_psi_feature returns not_found on empty monitor") {
    DriftMonitor dm;
    auto r = dm.worst_psi_feature();
    REQUIRE_FALSE(r);
    CHECK(r.error().code() == ErrorCode::not_found);
}

TEST_CASE("DriftMonitor::worst_psi_feature picks the highest-PSI feature") {
    DriftMonitor dm;
    std::vector<double> baseline(500, 0.5);
    REQUIRE(dm.register_feature("calm",  baseline, 0.0, 1.0, 10));
    REQUIRE(dm.register_feature("noisy", baseline, 0.0, 1.0, 10));

    // "calm" stays close to baseline.
    for (int i = 0; i < 500; ++i) REQUIRE(dm.observe("calm", 0.5));
    // "noisy" drifts hard to a different mode.
    for (int i = 0; i < 500; ++i) REQUIRE(dm.observe("noisy", 0.95));

    auto r = dm.worst_psi_feature();
    REQUIRE(r);
    CHECK(r->first == "noisy");
    // The returned report must agree with the feature name.
    CHECK(r->second.feature == "noisy");
    // PSI should be elevated (well above the calm-feature's PSI).
    CHECK(r->second.psi > 0.25);
    // Severity classification matches what classify(psi) would produce.
    CHECK(r->second.severity == DriftMonitor::classify(r->second.psi));
    // Sample-size fields populated.
    CHECK(r->second.reference_n == 500);
    CHECK(r->second.current_n   == 500);
}

TEST_CASE("DriftMonitor::worst_psi_feature breaks PSI ties alphabetically") {
    DriftMonitor dm;
    std::vector<double> baseline(200, 0.5);
    // Two features registered with identical baseline AND identical
    // observation pattern → identical PSI → alphabetical tiebreak.
    REQUIRE(dm.register_feature("zeta",  baseline, 0.0, 1.0, 10));
    REQUIRE(dm.register_feature("alpha", baseline, 0.0, 1.0, 10));
    for (int i = 0; i < 200; ++i) {
        REQUIRE(dm.observe("zeta",  0.5));
        REQUIRE(dm.observe("alpha", 0.5));
    }

    auto r = dm.worst_psi_feature();
    REQUIRE(r);
    // Equal PSI → alphabetical name wins.
    CHECK(r->first == "alpha");
    CHECK(r->second.feature == "alpha");

    // worst_psi_feature() must agree with most_drifted_feature() on the
    // winning name (the report-vs-name distinction is the only difference).
    auto md = dm.most_drifted_feature();
    REQUIRE(md);
    CHECK(*md == r->first);
}

TEST_CASE("DriftMonitor::worst_psi_feature single-feature monitor") {
    DriftMonitor dm;
    std::vector<double> baseline(300, 0.3);
    REQUIRE(dm.register_feature("only", baseline, 0.0, 1.0, 10));
    for (int i = 0; i < 300; ++i) REQUIRE(dm.observe("only", 0.3));

    auto r = dm.worst_psi_feature();
    REQUIRE(r);
    CHECK(r->first == "only");
    CHECK(r->second.feature == "only");
    // With current ≈ baseline, PSI should be small but the function still
    // returns the only registered feature.
    CHECK(r->second.psi < 0.25);
}

// ============== Histogram::is_balanced ==================================

TEST_CASE("Histogram::is_balanced is true on empty (vacuously)") {
    Histogram h{0.0, 1.0, 10};
    // Empty → vacuously balanced under any threshold.
    CHECK(h.is_balanced());
    CHECK(h.is_balanced(0.0));
    CHECK(h.is_balanced(0.5));
    CHECK(h.is_balanced(1.0));
}

TEST_CASE("Histogram::is_balanced default 0.4 threshold rejects single-bin collapse") {
    Histogram h{0.0, 1.0, 10};
    // Pile 10 observations into bin 5 → 100% mass in one bin.
    for (int i = 0; i < 10; ++i) h.observe(0.55);
    CHECK_FALSE(h.is_balanced());                 // default 0.4
    CHECK_FALSE(h.is_balanced(0.4));
    CHECK_FALSE(h.is_balanced(0.99));
    // 1.0 is the "everything passes" sentinel.
    CHECK(h.is_balanced(1.0));
}

TEST_CASE("Histogram::is_balanced accepts evenly-spread distributions") {
    Histogram h{0.0, 1.0, 10};
    // 100 observations spread evenly across bins → max share ~10%.
    for (int i = 0; i < 10; ++i) {
        for (int j = 0; j < 10; ++j) {
            // 10 observations per bin: lo + bin_w * (i + 0.5) keeps us on midpoints.
            h.observe(0.05 + 0.1 * static_cast<double>(i));
        }
    }
    CHECK(h.is_balanced());            // 0.1 < 0.4 → true
    CHECK(h.is_balanced(0.5));         // even more permissive
    CHECK(h.is_balanced(0.11));        // just above 10% share
    CHECK_FALSE(h.is_balanced(0.05));  // 5% threshold too tight
}

TEST_CASE("Histogram::is_balanced clamps max_share to [0, 1]") {
    Histogram h{0.0, 1.0, 4};
    // Half the observations in one bin, half in another → max share == 0.5.
    for (int i = 0; i < 4; ++i) h.observe(0.1);  // bin 0
    for (int i = 0; i < 4; ++i) h.observe(0.6);  // bin 2

    // max_share above 1.0 clamps to 1.0 → always true.
    CHECK(h.is_balanced(2.0));
    CHECK(h.is_balanced(100.0));
    // max_share below 0.0 clamps to 0.0 → never true on a non-empty hist.
    CHECK_FALSE(h.is_balanced(-1.0));
    CHECK_FALSE(h.is_balanced(-0.01));
    // At exactly the boundary share == 0.5 with strict less-than:
    // max_count (==4) < 0.5 * total (==4) → false.
    CHECK_FALSE(h.is_balanced(0.5));
    // Just above the share boundary → true.
    CHECK(h.is_balanced(0.51));
}

// ============== MetricRegistry::histogram_count_total (new semantics) ==

TEST_CASE("MetricRegistry::histogram_count_total returns 0 on empty registry") {
    MetricRegistry m;
    CHECK(m.histogram_count_total() == 0);

    // Adding only counters does not move the histogram total.
    m.inc("c1", 100);
    m.inc("c2", 50);
    CHECK(m.histogram_count_total() == 0);
}

TEST_CASE("MetricRegistry::histogram_count_total sums across multiple histograms") {
    MetricRegistry m;
    // Three histograms with different observation counts.
    for (int i = 0; i < 4;  ++i) m.observe("a", 0.1);   // 4 obs
    for (int i = 0; i < 7;  ++i) m.observe("b", 0.5);   // 7 obs
    for (int i = 0; i < 11; ++i) m.observe("c", 1.5);   // 11 obs

    // 4 + 7 + 11 = 22.
    CHECK(m.histogram_count_total() == 22);
    // Distinct from counter_total() which is unrelated to histograms.
    CHECK(m.counter_total() == 0);
}

TEST_CASE("MetricRegistry::histogram_count_total is distinct from counter_total") {
    MetricRegistry m;
    m.inc("ctr", 1000);
    m.observe("hist", 0.5);
    m.observe("hist", 0.6);
    m.observe("hist", 0.7);

    // counter_total: sum of counter VALUES.
    CHECK(m.counter_total()          == 1000);
    // histogram_count_total: sum of histogram OBSERVATIONS, not values.
    CHECK(m.histogram_count_total()  == 3);
    // The two values must not be conflated.
    CHECK(m.counter_total() != m.histogram_count_total());
}

TEST_CASE("MetricRegistry::histogram_count_total drops to 0 after reset_histograms") {
    MetricRegistry m;
    m.observe("h1", 0.1);
    m.observe("h1", 0.2);
    m.observe("h2", 0.5);
    REQUIRE(m.histogram_count_total() == 3);

    m.reset_histograms();
    CHECK(m.histogram_count_total() == 0);

    // After reset, fresh observations are visible.
    m.observe("h3", 0.9);
    CHECK(m.histogram_count_total() == 1);
}
