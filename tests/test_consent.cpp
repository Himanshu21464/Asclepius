// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include <doctest/doctest.h>

#include "asclepius/consent.hpp"

#include <atomic>
#include <thread>
#include <unordered_set>
#include <vector>

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

TEST_CASE("Every Purpose round-trips through string form") {
    for (auto p : {Purpose::ambient_documentation, Purpose::diagnostic_suggestion,
                   Purpose::triage, Purpose::medication_review,
                   Purpose::risk_stratification, Purpose::quality_improvement,
                   Purpose::research, Purpose::operations}) {
        auto r = purpose_from_string(to_string(p));
        REQUIRE(r);
        CHECK(r.value() == p);
    }
}

TEST_CASE("purpose_from_string rejects unknown strings") {
    CHECK(!purpose_from_string(""));
    CHECK(!purpose_from_string("AMBIENT_DOCUMENTATION"));
    CHECK(!purpose_from_string("nonexistent"));
}

TEST_CASE("Two grants for same patient: any matching purpose permits") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("p_dual");
    auto t1 = r.grant(p, {Purpose::triage}, 1h);
    auto t2 = r.grant(p, {Purpose::medication_review}, 1h);
    REQUIRE(t1);
    REQUIRE(t2);
    CHECK(t1.value().token_id != t2.value().token_id);
    auto a = r.permits(p, Purpose::triage);
    auto b = r.permits(p, Purpose::medication_review);
    auto c = r.permits(p, Purpose::research);
    REQUIRE(a); REQUIRE(b); REQUIRE(c);
    CHECK(a.value());
    CHECK(b.value());
    CHECK(!c.value());
}

TEST_CASE("Revoking one of two grants keeps the other live") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("p_partial");
    auto t1 = r.grant(p, {Purpose::triage}, 1h);
    auto t2 = r.grant(p, {Purpose::medication_review}, 1h);
    REQUIRE(t1); REQUIRE(t2);
    REQUIRE(r.revoke(t1.value().token_id));
    auto a = r.permits(p, Purpose::triage);
    auto b = r.permits(p, Purpose::medication_review);
    REQUIRE(a); REQUIRE(b);
    CHECK(!a.value());  // revoked
    CHECK(b.value());   // still live
}

TEST_CASE("revoke returns not_found for unknown token id") {
    ConsentRegistry r;
    auto a = r.revoke("ct_nonexistent");
    CHECK(!a);
    auto b = r.revoke("");
    CHECK(!b);
}

TEST_CASE("get round-trips a granted token") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("p_get");
    auto t = r.grant(p, {Purpose::quality_improvement}, 1h);
    REQUIRE(t);
    auto fetched = r.get(t.value().token_id);
    REQUIRE(fetched);
    CHECK(fetched.value().token_id == t.value().token_id);
    CHECK(fetched.value().patient == p);
    CHECK(fetched.value().purposes.size() == 1);
    CHECK(!fetched.value().revoked);
}

TEST_CASE("get returns not_found for unknown token id") {
    ConsentRegistry r;
    auto a = r.get("ct_unknown");
    CHECK(!a);
}

TEST_CASE("snapshot returns all tokens including revoked") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("p_snap");
    auto t1 = r.grant(p, {Purpose::triage}, 1h);
    auto t2 = r.grant(p, {Purpose::operations}, 1h);
    REQUIRE(t1); REQUIRE(t2);
    REQUIRE(r.revoke(t1.value().token_id));
    auto snap = r.snapshot();
    CHECK(snap.size() == 2);
    int revoked = 0;
    for (const auto& tok : snap) if (tok.revoked) ++revoked;
    CHECK(revoked == 1);
}

TEST_CASE("permits is per-patient, not global") {
    ConsentRegistry r;
    auto p1 = PatientId::pseudonymous("alice");
    auto p2 = PatientId::pseudonymous("bob");
    auto t = r.grant(p1, {Purpose::research}, 1h);
    REQUIRE(t);
    auto a = r.permits(p1, Purpose::research);
    auto b = r.permits(p2, Purpose::research);
    REQUIRE(a); REQUIRE(b);
    CHECK(a.value());
    CHECK(!b.value());  // p2 has no grant
}

TEST_CASE("Concurrent grants produce unique token ids") {
    ConsentRegistry r;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 100;
    std::vector<std::thread> ts;
    std::mutex out_mu;
    std::unordered_set<std::string> all;
    for (int i = 0; i < kThreads; ++i) {
        ts.emplace_back([&, i] {
            for (int j = 0; j < kPerThread; ++j) {
                auto p = PatientId::pseudonymous("p_" + std::to_string(i) + "_" + std::to_string(j));
                auto tk = r.grant(p, {Purpose::operations}, 1h);
                REQUIRE(tk);
                std::lock_guard<std::mutex> lk(out_mu);
                all.insert(tk.value().token_id);
            }
        });
    }
    for (auto& t : ts) t.join();
    CHECK(all.size() == kThreads * kPerThread);
    CHECK(r.snapshot().size() == kThreads * kPerThread);
}
