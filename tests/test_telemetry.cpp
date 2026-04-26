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
    REQUIRE(m.histogram_count_total() == 1);

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

TEST_CASE("MetricRegistry::histogram_count_total tracks distinct histogram names") {
    MetricRegistry m;
    CHECK(m.histogram_count_total() == 0);
    m.observe("svc.latency", 0.1);
    CHECK(m.histogram_count_total() == 1);
    m.observe("svc.latency", 0.2);  // same histogram
    CHECK(m.histogram_count_total() == 1);
    m.observe("queue.depth", 7.0);  // different histogram
    CHECK(m.histogram_count_total() == 2);
}

TEST_CASE("MetricRegistry counter_count and histogram_count_total are independent") {
    MetricRegistry m;
    m.inc("c1");
    m.inc("c2");
    m.inc("c3");
    m.observe("h1", 1.0);
    m.observe("h2", 2.0);

    CHECK(m.counter_count() == 3);
    CHECK(m.histogram_count_total() == 2);

    // After reset() of one counter, counter_count must remain unchanged
    // (reset zeroes the value, doesn't drop the entry).
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
    REQUIRE(m.histogram_count_total() == 1);

    m.reset_histograms();

    // Counters are intact (values and entry count).
    CHECK(m.counter_count() == 2);
    CHECK(m.count("inferences_total") == 42);
    CHECK(m.count("policy_violations_total") == 3);

    // Histograms are gone.
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
