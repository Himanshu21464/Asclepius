// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include <doctest/doctest.h>

#include "asclepius/consent.hpp"

using namespace asclepius;
using namespace std::chrono_literals;

TEST_CASE("Granted consent permits the listed purpose") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("p1");
    auto t = r.grant(p, {Purpose::ambient_documentation, Purpose::triage}, 1h);
    REQUIRE(t);

    auto perm = r.permits(p, Purpose::ambient_documentation);
    REQUIRE(perm);
    CHECK(perm.value());

    auto deny = r.permits(p, Purpose::medication_review);
    REQUIRE(deny);
    CHECK(!deny.value());
}

TEST_CASE("Revoked consent stops permitting") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("p2");
    auto t = r.grant(p, {Purpose::diagnostic_suggestion}, 1h);
    REQUIRE(t);

    REQUIRE(r.revoke(t.value().token_id));
    auto perm = r.permits(p, Purpose::diagnostic_suggestion);
    REQUIRE(perm);
    CHECK(!perm.value());
}

TEST_CASE("Empty purpose list rejected") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("p3");
    auto t = r.grant(p, {}, 1h);
    CHECK(!t);
}

TEST_CASE("Purpose stringification round-trips") {
    auto p = Purpose::medication_review;
    auto s = to_string(p);
    auto r = purpose_from_string(s);
    REQUIRE(r);
    CHECK(r.value() == p);
}
