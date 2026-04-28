// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors

#include <doctest/doctest.h>

#include "asclepius/telemetry.hpp"

#include <cmath>

using namespace asclepius;

namespace {

void seed_classic_2x2(CalibrationMonitor& cm,
                      std::size_t tp, std::size_t fp,
                      std::size_t tn, std::size_t fn) {
    cm.record_n(CalibrationMonitor::Outcome::true_positive,  tp);
    cm.record_n(CalibrationMonitor::Outcome::false_positive, fp);
    cm.record_n(CalibrationMonitor::Outcome::true_negative,  tn);
    cm.record_n(CalibrationMonitor::Outcome::false_negative, fn);
}

}  // namespace

TEST_CASE("calibration: zero state has NaN rates and zero counts") {
    CalibrationMonitor cm;
    CHECK(cm.tp() == 0);
    CHECK(cm.fp() == 0);
    CHECK(cm.tn() == 0);
    CHECK(cm.fn() == 0);
    CHECK(cm.total() == 0);
    CHECK(std::isnan(cm.sensitivity()));
    CHECK(std::isnan(cm.specificity()));
    CHECK(std::isnan(cm.ppv()));
    CHECK(std::isnan(cm.npv()));
    CHECK(std::isnan(cm.accuracy()));
}

TEST_CASE("calibration: record/record_n increments the right counters") {
    CalibrationMonitor cm;
    cm.record(CalibrationMonitor::Outcome::true_positive);
    cm.record(CalibrationMonitor::Outcome::false_positive);
    cm.record_n(CalibrationMonitor::Outcome::true_negative,  10);
    cm.record_n(CalibrationMonitor::Outcome::false_negative,  3);
    CHECK(cm.tp() == 1);
    CHECK(cm.fp() == 1);
    CHECK(cm.tn() == 10);
    CHECK(cm.fn() == 3);
    CHECK(cm.total() == 15);
}

TEST_CASE("calibration: rates compute against expected denominators") {
    CalibrationMonitor cm;
    seed_classic_2x2(cm, /*tp=*/40, /*fp=*/10, /*tn=*/45, /*fn=*/5);
    // sensitivity = 40 / (40+5) = 0.888...
    // specificity = 45 / (45+10) = 0.818...
    // ppv         = 40 / (40+10) = 0.8
    // npv         = 45 / (45+5)  = 0.9
    // accuracy    = 85 / 100 = 0.85
    CHECK(cm.sensitivity() == doctest::Approx(40.0 / 45.0));
    CHECK(cm.specificity() == doctest::Approx(45.0 / 55.0));
    CHECK(cm.ppv()         == doctest::Approx(0.8));
    CHECK(cm.npv()         == doctest::Approx(0.9));
    CHECK(cm.accuracy()    == doctest::Approx(0.85));
}

TEST_CASE("calibration: is_below_floor returns false with too few samples") {
    CalibrationMonitor cm({/*sens=*/0.99, /*spec=*/0.5, /*tol=*/0.0});
    seed_classic_2x2(cm, 1, 0, 1, 1);
    // total = 3 < 30 → false even though sensitivity 1/2 = 0.5 < 0.99
    CHECK_FALSE(cm.is_below_floor(30));
}

TEST_CASE("calibration: sensitivity_below_floor flips when below target with enough samples") {
    CalibrationMonitor cm({/*sens=*/0.99, /*spec=*/0.0, /*tol=*/0.0});
    // 50 TP + 5 FN → sensitivity = 0.909 < 0.99
    seed_classic_2x2(cm, 50, 0, 0, 5);
    CHECK(cm.sensitivity_below_floor(30));
    CHECK(cm.is_below_floor(30));
}

TEST_CASE("calibration: tolerance shifts the alarm threshold") {
    CalibrationMonitor cm({/*sens=*/0.99, /*spec=*/0.0, /*tol=*/0.10});
    // sensitivity 0.909, floor 0.99, tol 0.10 → alarm when < 0.89.
    seed_classic_2x2(cm, 50, 0, 0, 5);
    CHECK_FALSE(cm.sensitivity_below_floor(30));  // 0.909 > 0.89 → not below
}

TEST_CASE("calibration: specificity_below_floor mirrors sensitivity logic") {
    CalibrationMonitor cm({/*sens=*/0.0, /*spec=*/0.99, /*tol=*/0.0});
    seed_classic_2x2(cm, 0, 5, 50, 0);  // specificity = 50/55 = 0.909
    CHECK(cm.specificity_below_floor(30));
    CHECK_FALSE(cm.sensitivity_below_floor(30));
    CHECK(cm.is_below_floor(30));
}

TEST_CASE("calibration: snapshot reports all derived rates") {
    CalibrationMonitor cm({/*sens=*/0.95, /*spec=*/0.95, /*tol=*/0.0});
    seed_classic_2x2(cm, 95, 5, 95, 5);  // 95% sens, 95% spec
    auto s = cm.snapshot(/*min_samples=*/30);
    CHECK(s.tp == 95);
    CHECK(s.fp == 5);
    CHECK(s.tn == 95);
    CHECK(s.fn == 5);
    CHECK(s.total == 200);
    CHECK(s.sensitivity == doctest::Approx(0.95));
    CHECK(s.specificity == doctest::Approx(0.95));
    CHECK_FALSE(s.below_floor);
}

TEST_CASE("calibration: snapshot.below_floor is false below min_samples") {
    CalibrationMonitor cm({/*sens=*/0.99, /*spec=*/0.0, /*tol=*/0.0});
    seed_classic_2x2(cm, 1, 0, 0, 1);  // 50% sens, but only 2 samples
    auto s = cm.snapshot(/*min_samples=*/30);
    CHECK_FALSE(s.below_floor);
}

TEST_CASE("calibration: set_targets changes the floor live") {
    CalibrationMonitor cm({/*sens=*/0.50, /*spec=*/0.0, /*tol=*/0.0});
    seed_classic_2x2(cm, 50, 0, 0, 5);
    CHECK_FALSE(cm.sensitivity_below_floor(30));   // 0.909 ≥ 0.50

    cm.set_targets({/*sens=*/0.99, /*spec=*/0.0, /*tol=*/0.0});
    CHECK(cm.sensitivity_below_floor(30));         // 0.909 < 0.99
}

TEST_CASE("calibration: reset zeros every counter") {
    CalibrationMonitor cm;
    seed_classic_2x2(cm, 5, 5, 5, 5);
    CHECK(cm.total() == 20);
    cm.reset();
    CHECK(cm.total() == 0);
    CHECK(std::isnan(cm.sensitivity()));
}

TEST_CASE("calibration: targets() round-trip") {
    CalibrationMonitor::Targets t{0.95, 0.90, 0.05};
    CalibrationMonitor cm(t);
    auto t2 = cm.targets();
    CHECK(t2.sensitivity_floor == t.sensitivity_floor);
    CHECK(t2.specificity_floor == t.specificity_floor);
    CHECK(t2.tolerance         == t.tolerance);
}

TEST_CASE("calibration: summary_string matches documented format") {
    CalibrationMonitor cm;
    seed_classic_2x2(cm, 1, 2, 3, 4);
    auto s = cm.summary_string();
    CHECK(s.find("tp=1") != std::string::npos);
    CHECK(s.find("fp=2") != std::string::npos);
    CHECK(s.find("tn=3") != std::string::npos);
    CHECK(s.find("fn=4") != std::string::npos);
    CHECK(s.find("sens=") != std::string::npos);
    CHECK(s.find("spec=") != std::string::npos);
    CHECK(s.back() != '\n');
}

TEST_CASE("calibration: to_string exhausts all Outcome values") {
    using O = CalibrationMonitor::Outcome;
    CHECK(std::string{to_string(O::true_positive)}  == "true_positive");
    CHECK(std::string{to_string(O::false_positive)} == "false_positive");
    CHECK(std::string{to_string(O::true_negative)}  == "true_negative");
    CHECK(std::string{to_string(O::false_negative)} == "false_negative");
}

TEST_CASE("calibration: zero specificity edge case") {
    CalibrationMonitor cm({/*sens=*/0.0, /*spec=*/0.99, /*tol=*/0.0});
    // No negatives at all → specificity is NaN → not below floor.
    seed_classic_2x2(cm, 30, 0, 0, 0);
    CHECK_FALSE(cm.specificity_below_floor(30));
    CHECK(std::isnan(cm.specificity()));
}

TEST_CASE("calibration: record_n with n=0 is a no-op") {
    CalibrationMonitor cm;
    cm.record_n(CalibrationMonitor::Outcome::true_positive, 0);
    CHECK(cm.total() == 0);
}
