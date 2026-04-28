// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors

#include <doctest/doctest.h>

#include "asclepius/telemetry.hpp"

#include <chrono>
#include <cmath>

using namespace asclepius;
using namespace std::chrono_literals;

namespace {

CohortLedger::Observation
mk(const char* patient, const char* metric, double value, Time at,
   const char* prov = "test") {
    return CohortLedger::Observation{
        PatientId::pseudonymous(patient),
        std::string{metric},
        value,
        at,
        std::string{prov},
    };
}

}  // namespace

TEST_CASE("cohort: empty state") {
    CohortLedger c;
    CHECK(c.total_observations() == 0);
    CHECK(c.patient_count() == 0);
    CHECK(c.distinct_metrics() == 0);
    CHECK(c.metrics().empty());
    CHECK(c.patients().empty());
    CHECK(c.snapshot().empty());
}

TEST_CASE("cohort: append + total/patient/metric counts") {
    CohortLedger c;
    auto t = Time::now();
    c.append(mk("p1", "hba1c", 7.2, t));
    c.append(mk("p2", "hba1c", 8.1, t));
    c.append(mk("p1", "bp_systolic", 140.0, t + std::chrono::nanoseconds{1h}));
    CHECK(c.total_observations() == 3);
    CHECK(c.patient_count()      == 2);
    CHECK(c.distinct_metrics()   == 2);
}

TEST_CASE("cohort: for_patient returns sorted by observed_at") {
    CohortLedger c;
    auto t = Time::now();
    c.append(mk("p1", "x", 1.0, t + std::chrono::nanoseconds{2h}));
    c.append(mk("p1", "x", 2.0, t));
    c.append(mk("p1", "x", 3.0, t + std::chrono::nanoseconds{1h}));
    c.append(mk("p2", "x", 99.0, t));
    auto r = c.for_patient(PatientId::pseudonymous("p1"));
    REQUIRE(r.size() == 3);
    CHECK(r[0].value == doctest::Approx(2.0));
    CHECK(r[1].value == doctest::Approx(3.0));
    CHECK(r[2].value == doctest::Approx(1.0));
}

TEST_CASE("cohort: for_metric scopes across patients") {
    CohortLedger c;
    auto t = Time::now();
    c.append(mk("p1", "hba1c", 7.2, t));
    c.append(mk("p2", "hba1c", 8.1, t + std::chrono::nanoseconds{1h}));
    c.append(mk("p1", "bp_systolic", 140.0, t));
    auto r = c.for_metric("hba1c");
    CHECK(r.size() == 2);
}

TEST_CASE("cohort: in_window is half-open") {
    CohortLedger c;
    auto t = Time::now();
    c.append(mk("p1", "x", 1.0, t));
    c.append(mk("p1", "x", 2.0, t + std::chrono::nanoseconds{1h}));
    c.append(mk("p1", "x", 3.0, t + std::chrono::nanoseconds{2h}));
    auto r = c.in_window(t + std::chrono::nanoseconds{1h},
                         t + std::chrono::nanoseconds{2h});
    REQUIRE(r.size() == 1);
    CHECK(r[0].value == doctest::Approx(2.0));
}

TEST_CASE("cohort: latest returns most recent observation") {
    CohortLedger c;
    auto t = Time::now();
    c.append(mk("p1", "hba1c", 7.2, t));
    c.append(mk("p1", "hba1c", 6.8, t + std::chrono::nanoseconds{1h}));
    c.append(mk("p1", "hba1c", 7.0, t + std::chrono::nanoseconds{2h}));
    auto r = c.latest(PatientId::pseudonymous("p1"), "hba1c");
    REQUIRE(r);
    CHECK(r.value().value == doctest::Approx(7.0));
}

TEST_CASE("cohort: latest returns not_found for missing patient/metric") {
    CohortLedger c;
    auto r = c.latest(PatientId::pseudonymous("nope"), "hba1c");
    CHECK(!r);
}

TEST_CASE("cohort: stats_for_metric returns count/mean/stddev/min/max") {
    CohortLedger c;
    auto t = Time::now();
    for (double v : {1.0, 2.0, 3.0, 4.0, 5.0}) {
        c.append(mk("p", "x", v, t));
    }
    auto s = c.stats_for_metric("x");
    REQUIRE(s);
    CHECK(s.value().count == 5);
    CHECK(s.value().mean      == doctest::Approx(3.0));
    CHECK(s.value().min_value == doctest::Approx(1.0));
    CHECK(s.value().max_value == doctest::Approx(5.0));
    // Population stddev of {1..5} = sqrt(2)
    CHECK(s.value().stddev == doctest::Approx(std::sqrt(2.0)));
}

TEST_CASE("cohort: stats_for_metric returns not_found for unknown metric") {
    CohortLedger c;
    CHECK(!c.stats_for_metric("nope"));
}

TEST_CASE("cohort: stats_for_patient_metric scopes correctly") {
    CohortLedger c;
    auto t = Time::now();
    c.append(mk("p1", "x", 1.0, t));
    c.append(mk("p1", "x", 3.0, t));
    c.append(mk("p2", "x", 100.0, t));
    auto s = c.stats_for_patient_metric(PatientId::pseudonymous("p1"), "x");
    REQUIRE(s);
    CHECK(s.value().count == 2);
    CHECK(s.value().mean  == doctest::Approx(2.0));
}

TEST_CASE("cohort: metrics() returns sorted distinct metrics") {
    CohortLedger c;
    auto t = Time::now();
    c.append(mk("p", "z", 1.0, t));
    c.append(mk("p", "a", 1.0, t));
    c.append(mk("p", "m", 1.0, t));
    c.append(mk("p", "a", 2.0, t));  // duplicate
    auto m = c.metrics();
    REQUIRE(m.size() == 3);
    CHECK(m[0] == "a");
    CHECK(m[1] == "m");
    CHECK(m[2] == "z");
}

TEST_CASE("cohort: patients() returns sorted distinct patients") {
    CohortLedger c;
    auto t = Time::now();
    c.append(mk("p3", "x", 1.0, t));
    c.append(mk("p1", "x", 1.0, t));
    c.append(mk("p2", "x", 1.0, t));
    auto p = c.patients();
    REQUIRE(p.size() == 3);
    // pseudonymous ids prefix with "pat:"; sort within that.
    CHECK(p[0].str() < p[1].str());
    CHECK(p[1].str() < p[2].str());
}

TEST_CASE("cohort: snapshot is sorted by observed_at then patient.str()") {
    CohortLedger c;
    auto t = Time::now();
    c.append(mk("z", "x", 1.0, t));
    c.append(mk("a", "x", 2.0, t));   // same time, different patient
    c.append(mk("a", "x", 3.0, t + std::chrono::nanoseconds{1h}));
    auto s = c.snapshot();
    REQUIRE(s.size() == 3);
    CHECK(s[0].patient.str() == "pat:a");  // tie broken by patient
    CHECK(s[1].patient.str() == "pat:z");
    CHECK(s[2].patient.str() == "pat:a");
}

TEST_CASE("cohort: append_n batch insert") {
    CohortLedger c;
    auto t = Time::now();
    std::vector<CohortLedger::Observation> obs = {
        mk("p1", "x", 1.0, t),
        mk("p1", "x", 2.0, t + std::chrono::nanoseconds{1h}),
        mk("p1", "x", 3.0, t + std::chrono::nanoseconds{2h}),
    };
    c.append_n(obs);
    CHECK(c.total_observations() == 3);
}

TEST_CASE("cohort: clear drops all observations") {
    CohortLedger c;
    c.append(mk("p", "x", 1.0, Time::now()));
    c.append(mk("p", "y", 2.0, Time::now()));
    CHECK(c.total_observations() == 2);
    c.clear();
    CHECK(c.total_observations() == 0);
    CHECK(c.patient_count() == 0);
}
