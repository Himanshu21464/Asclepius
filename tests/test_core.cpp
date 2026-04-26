// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include <doctest/doctest.h>

#include "asclepius/core.hpp"

#include <chrono>

using namespace asclepius;

TEST_CASE("Result<T> success carries the value") {
    Result<int> r{42};
    REQUIRE(r);
    CHECK(r.value() == 42);
    CHECK(*r == 42);
}

TEST_CASE("Result<T> error carries an Error") {
    Result<int> r{Error::invalid("boom")};
    REQUIRE(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
    CHECK(r.error().what() == "boom");
}

TEST_CASE("Result<void>::ok is success") {
    auto r = Result<void>::ok();
    CHECK(r);
}

TEST_CASE("ErrorCode round-trips to string") {
    CHECK(std::string{to_string(ErrorCode::policy_violation)} == "policy_violation");
    CHECK(std::string{to_string(ErrorCode::integrity_failure)} == "integrity_failure");
}

TEST_CASE("Time::now is monotonic non-decreasing") {
    auto a = Time::now();
    auto b = Time::now();
    CHECK(a <= b);
}

TEST_CASE("Time::iso8601 round-trips") {
    auto t  = Time::now();
    auto s  = t.iso8601();
    auto t2 = Time::from_iso8601(s);
    // We allow small loss due to ns vs the 9-digit fraction we emit.
    auto delta = std::chrono::nanoseconds{t.nanos_since_epoch() - t2.nanos_since_epoch()};
    CHECK(std::abs(delta.count()) < 1'000'000);  // 1ms
}

TEST_CASE("Time arithmetic") {
    auto a = Time{0};
    auto b = a + std::chrono::seconds{5};
    CHECK((b - a) == std::chrono::seconds{5});
}
