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

// ============== observer hook ===========================================

TEST_CASE("set_observer fires on grant") {
    ConsentRegistry r;
    int granted_count = 0;
    int revoked_count = 0;
    std::string seen_token;
    r.set_observer([&](ConsentRegistry::Event e, const ConsentToken& t) {
        if (e == ConsentRegistry::Event::granted) granted_count++;
        if (e == ConsentRegistry::Event::revoked) revoked_count++;
        seen_token = t.token_id;
    });
    auto t = r.grant(PatientId::pseudonymous("p"),
                     {Purpose::ambient_documentation}, 1h);
    REQUIRE(t);
    CHECK(granted_count == 1);
    CHECK(revoked_count == 0);
    CHECK(seen_token == t.value().token_id);
}

TEST_CASE("set_observer fires on revoke") {
    ConsentRegistry r;
    auto t = r.grant(PatientId::pseudonymous("p"),
                     {Purpose::ambient_documentation}, 1h).value();
    int revoked_count = 0;
    r.set_observer([&](ConsentRegistry::Event e, const ConsentToken&) {
        if (e == ConsentRegistry::Event::revoked) revoked_count++;
    });
    REQUIRE(r.revoke(t.token_id));
    CHECK(revoked_count == 1);
}

TEST_CASE("set_observer can be cleared with empty function") {
    ConsentRegistry r;
    int n = 0;
    r.set_observer([&](ConsentRegistry::Event, const ConsentToken&) { n++; });
    REQUIRE(r.grant(PatientId::pseudonymous("p"),
                    {Purpose::triage}, 1h));
    r.set_observer({});
    REQUIRE(r.grant(PatientId::pseudonymous("p"),
                    {Purpose::triage}, 1h));
    CHECK(n == 1);
}

TEST_CASE("set_observer is replaced on subsequent calls") {
    ConsentRegistry r;
    int a = 0, b = 0;
    r.set_observer([&](ConsentRegistry::Event, const ConsentToken&) { a++; });
    r.set_observer([&](ConsentRegistry::Event, const ConsentToken&) { b++; });
    REQUIRE(r.grant(PatientId::pseudonymous("p"),
                    {Purpose::triage}, 1h));
    CHECK(a == 0);
    CHECK(b == 1);
}

TEST_CASE("ingest restores a token verbatim") {
    ConsentRegistry r;
    ConsentToken t;
    t.token_id   = "ct_imported";
    t.patient    = PatientId::pseudonymous("p");
    t.purposes   = {Purpose::medication_review};
    t.issued_at  = Time::now();
    t.expires_at = t.issued_at + std::chrono::nanoseconds{std::chrono::hours{1}};
    t.revoked    = false;
    REQUIRE(r.ingest(t));
    auto got = r.get("ct_imported");
    REQUIRE(got);
    CHECK(got.value().token_id == "ct_imported");
    CHECK(got.value().purposes.size() == 1);
}

TEST_CASE("ingest does not fire the observer (replay safety)") {
    ConsentRegistry r;
    int n = 0;
    r.set_observer([&](ConsentRegistry::Event, const ConsentToken&) { n++; });
    ConsentToken t;
    t.token_id  = "ct_imported";
    t.patient   = PatientId::pseudonymous("p");
    t.purposes  = {Purpose::triage};
    t.issued_at = Time::now();
    t.expires_at= t.issued_at + std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(t));
    CHECK(n == 0);
}

TEST_CASE("ingest rejects duplicate token_id with conflict") {
    ConsentRegistry r;
    ConsentToken t;
    t.token_id   = "ct_dup";
    t.patient    = PatientId::pseudonymous("p");
    t.purposes   = {Purpose::triage};
    t.issued_at  = Time::now();
    t.expires_at = t.issued_at + std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(t));
    auto r2 = r.ingest(t);
    CHECK(!r2);
    CHECK(r2.error().code() == ErrorCode::conflict);
}

TEST_CASE("ingested revoked token correctly stops permitting") {
    ConsentRegistry r;
    auto pid = PatientId::pseudonymous("p");
    ConsentToken t;
    t.token_id  = "ct_revoked";
    t.patient   = pid;
    t.purposes  = {Purpose::triage};
    t.issued_at = Time::now();
    t.expires_at= t.issued_at + std::chrono::nanoseconds{std::chrono::hours{1}};
    t.revoked   = true;
    REQUIRE(r.ingest(t));
    auto p = r.permits(pid, Purpose::triage);
    REQUIRE(p);
    CHECK(p.value() == false);
}


// ============== tokens_for_patient + extend =============================

TEST_CASE("tokens_for_patient returns just that patient's tokens") {
    ConsentRegistry r;
    auto pa = PatientId::pseudonymous("a");
    auto pb = PatientId::pseudonymous("b");
    REQUIRE(r.grant(pa, {Purpose::triage}, 1h));
    REQUIRE(r.grant(pa, {Purpose::ambient_documentation}, 1h));
    REQUIRE(r.grant(pb, {Purpose::triage}, 1h));
    auto a = r.tokens_for_patient(pa);
    CHECK(a.size() == 2);
    auto b = r.tokens_for_patient(pb);
    CHECK(b.size() == 1);
    auto c = r.tokens_for_patient(PatientId::pseudonymous("ghost"));
    CHECK(c.empty());
}

TEST_CASE("tokens_for_patient includes revoked tokens") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("p");
    auto t = r.grant(p, {Purpose::triage}, 1h).value();
    REQUIRE(r.revoke(t.token_id));
    auto out = r.tokens_for_patient(p);
    REQUIRE(out.size() == 1);
    CHECK(out[0].revoked == true);
}

TEST_CASE("extend pushes expires_at forward") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("p");
    auto t = r.grant(p, {Purpose::triage}, 1h).value();
    auto orig_expires = t.expires_at;
    auto t2 = r.extend(t.token_id, 1h);
    REQUIRE(t2);
    CHECK(t2.value().expires_at > orig_expires);
}

TEST_CASE("extend rejects revoked tokens") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("p");
    auto t = r.grant(p, {Purpose::triage}, 1h).value();
    REQUIRE(r.revoke(t.token_id));
    auto r2 = r.extend(t.token_id, 1h);
    CHECK(!r2);
    CHECK(r2.error().code() == ErrorCode::permission_denied);
}

TEST_CASE("extend rejects unknown token_id") {
    ConsentRegistry r;
    auto r2 = r.extend("ghost", 1h);
    CHECK(!r2);
    CHECK(r2.error().code() == ErrorCode::not_found);
}

TEST_CASE("extend rejects non-positive ttl") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("p");
    auto t = r.grant(p, {Purpose::triage}, 1h).value();
    auto r2 = r.extend(t.token_id, std::chrono::seconds{0});
    CHECK(!r2);
    CHECK(r2.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("extend fires the observer as a granted event") {
    ConsentRegistry r;
    int n = 0;
    r.set_observer([&](ConsentRegistry::Event e, const ConsentToken&) {
        if (e == ConsentRegistry::Event::granted) n++;
    });
    auto p = PatientId::pseudonymous("p");
    auto t = r.grant(p, {Purpose::triage}, 1h).value();  // n=1
    REQUIRE(r.extend(t.token_id, 1h));                   // n=2
    CHECK(n == 2);
}

TEST_CASE("active_count vs total_count: revoked counts toward total only") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("p");
    auto t1 = r.grant(p, {Purpose::triage}, 1h).value();
    REQUIRE(r.grant(p, {Purpose::ambient_documentation}, 1h));
    REQUIRE(r.grant(p, {Purpose::medication_review}, 1h));
    CHECK(r.total_count() == 3);
    CHECK(r.active_count() == 3);
    REQUIRE(r.revoke(t1.token_id));
    CHECK(r.total_count() == 3);
    CHECK(r.active_count() == 2);
}

TEST_CASE("expire_all_for_patient revokes all of one patient's tokens") {
    ConsentRegistry r;
    auto pa = PatientId::pseudonymous("alice");
    auto pb = PatientId::pseudonymous("bob");
    REQUIRE(r.grant(pa, {Purpose::triage}, 1h));
    REQUIRE(r.grant(pa, {Purpose::ambient_documentation}, 1h));
    REQUIRE(r.grant(pa, {Purpose::medication_review}, 1h));
    REQUIRE(r.grant(pb, {Purpose::triage}, 1h));
    auto n = r.expire_all_for_patient(pa);
    CHECK(n == 3);
    auto perm = r.permits(pa, Purpose::triage);
    CHECK(perm.value() == false);
    auto perm_b = r.permits(pb, Purpose::triage);
    CHECK(perm_b.value() == true);
}

TEST_CASE("expire_all_for_patient: no-op for unknown patient") {
    ConsentRegistry r;
    auto n = r.expire_all_for_patient(PatientId::pseudonymous("ghost"));
    CHECK(n == 0);
}

TEST_CASE("expire_all_for_patient skips already-revoked tokens") {
    ConsentRegistry r;
    auto p  = PatientId::pseudonymous("p");
    auto t1 = r.grant(p, {Purpose::triage}, 1h).value();
    REQUIRE(r.grant(p, {Purpose::ambient_documentation}, 1h));
    REQUIRE(r.revoke(t1.token_id));  // already revoked
    auto n = r.expire_all_for_patient(p);
    CHECK(n == 1);  // only the second one needed revoking
}

TEST_CASE("expire_all_for_patient fires the observer once per token") {
    ConsentRegistry r;
    int revokes = 0;
    r.set_observer([&](ConsentRegistry::Event e, const ConsentToken&) {
        if (e == ConsentRegistry::Event::revoked) revokes++;
    });
    auto p = PatientId::pseudonymous("p");
    REQUIRE(r.grant(p, {Purpose::triage}, 1h));
    REQUIRE(r.grant(p, {Purpose::ambient_documentation}, 1h));
    auto n = r.expire_all_for_patient(p);
    CHECK(n == 2);
    CHECK(revokes == 2);
}
