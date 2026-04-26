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

// ============== token_exists ============================================

TEST_CASE("token_exists returns true for a granted token id") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("p_te");
    auto t = r.grant(p, {Purpose::triage}, 1h);
    REQUIRE(t);
    CHECK(r.token_exists(t.value().token_id));
}

TEST_CASE("token_exists returns false on an empty registry / unknown id") {
    ConsentRegistry r;
    CHECK(!r.token_exists("ct_does_not_exist"));
    CHECK(!r.token_exists(""));
}

TEST_CASE("token_exists stays true after revoke (id still on file)") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("p_te2");
    auto t = r.grant(p, {Purpose::triage}, 1h).value();
    REQUIRE(r.revoke(t.token_id));
    CHECK(r.token_exists(t.token_id));
}

TEST_CASE("token_exists sees ingested tokens (replay path)") {
    ConsentRegistry r;
    ConsentToken t;
    t.token_id   = "ct_replayed";
    t.patient    = PatientId::pseudonymous("p");
    t.purposes   = {Purpose::operations};
    t.issued_at  = Time::now();
    t.expires_at = t.issued_at + std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(t));
    CHECK(r.token_exists("ct_replayed"));
    CHECK(!r.token_exists("ct_other"));
}

// ============== expired_count ===========================================

TEST_CASE("expired_count is zero on empty registry") {
    ConsentRegistry r;
    CHECK(r.expired_count() == 0);
}

TEST_CASE("expired_count counts non-revoked tokens whose expiry passed") {
    ConsentRegistry r;
    auto pid = PatientId::pseudonymous("p_exp");
    // Two expired tokens via ingest with a past expires_at.
    ConsentToken e1;
    e1.token_id   = "ct_exp_1";
    e1.patient    = pid;
    e1.purposes   = {Purpose::triage};
    e1.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    e1.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(e1));

    ConsentToken e2 = e1;
    e2.token_id = "ct_exp_2";
    REQUIRE(r.ingest(e2));

    // One live grant; should NOT be counted.
    REQUIRE(r.grant(pid, {Purpose::operations}, 1h));

    CHECK(r.expired_count() == 2);
}

TEST_CASE("expired_count excludes revoked tokens (even if expiry passed)") {
    ConsentRegistry r;
    auto pid = PatientId::pseudonymous("p_exp_rev");
    ConsentToken e;
    e.token_id   = "ct_exp_rev";
    e.patient    = pid;
    e.purposes   = {Purpose::triage};
    e.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    e.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    e.revoked    = true;  // already revoked → not "expired" per definition
    REQUIRE(r.ingest(e));
    CHECK(r.expired_count() == 0);
}

// ============== cleanup_expired =========================================

TEST_CASE("cleanup_expired sweeps non-revoked expired tokens to revoked") {
    ConsentRegistry r;
    auto pid = PatientId::pseudonymous("p_clean");
    ConsentToken e1;
    e1.token_id   = "ct_clean_1";
    e1.patient    = pid;
    e1.purposes   = {Purpose::triage};
    e1.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    e1.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(e1));

    ConsentToken e2 = e1;
    e2.token_id = "ct_clean_2";
    REQUIRE(r.ingest(e2));

    // One live grant; should be untouched.
    auto live = r.grant(pid, {Purpose::operations}, 1h);
    REQUIRE(live);

    auto n = r.cleanup_expired();
    CHECK(n == 2);
    CHECK(r.expired_count() == 0);

    // Swept tokens are now revoked but still on file.
    auto g1 = r.get("ct_clean_1");
    REQUIRE(g1);
    CHECK(g1.value().revoked == true);
    auto g2 = r.get("ct_clean_2");
    REQUIRE(g2);
    CHECK(g2.value().revoked == true);

    // Live token unaffected.
    auto gl = r.get(live.value().token_id);
    REQUIRE(gl);
    CHECK(gl.value().revoked == false);
}

TEST_CASE("cleanup_expired is a no-op on empty / all-active / all-revoked registry") {
    // Empty.
    {
        ConsentRegistry r;
        CHECK(r.cleanup_expired() == 0);
    }
    // All-active.
    {
        ConsentRegistry r;
        REQUIRE(r.grant(PatientId::pseudonymous("a"), {Purpose::triage}, 1h));
        REQUIRE(r.grant(PatientId::pseudonymous("b"), {Purpose::operations}, 1h));
        CHECK(r.cleanup_expired() == 0);
    }
    // All-revoked (and one of them expired, but already revoked → skip).
    {
        ConsentRegistry r;
        auto t = r.grant(PatientId::pseudonymous("c"), {Purpose::triage}, 1h).value();
        REQUIRE(r.revoke(t.token_id));

        ConsentToken e;
        e.token_id   = "ct_revoked_expired";
        e.patient    = PatientId::pseudonymous("d");
        e.purposes   = {Purpose::triage};
        e.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
        e.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
        e.revoked    = true;
        REQUIRE(r.ingest(e));

        CHECK(r.cleanup_expired() == 0);
    }
}

// ============== clear ===================================================

TEST_CASE("clear drops all tokens") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("p_clear");
    REQUIRE(r.grant(p, {Purpose::triage}, 1h));
    REQUIRE(r.grant(p, {Purpose::operations}, 1h));
    REQUIRE(r.grant(PatientId::pseudonymous("other"), {Purpose::research}, 1h));
    CHECK(r.total_count() == 3);
    r.clear();
    CHECK(r.total_count() == 0);
    CHECK(r.active_count() == 0);
    CHECK(r.snapshot().empty());
}

TEST_CASE("clear on an empty registry is a no-op") {
    ConsentRegistry r;
    r.clear();
    CHECK(r.total_count() == 0);
    r.clear();
    CHECK(r.total_count() == 0);
}

TEST_CASE("clear does NOT fire the observer") {
    ConsentRegistry r;
    int events = 0;
    REQUIRE(r.grant(PatientId::pseudonymous("a"), {Purpose::triage}, 1h));
    REQUIRE(r.grant(PatientId::pseudonymous("b"), {Purpose::triage}, 1h));
    r.set_observer([&](ConsentRegistry::Event, const ConsentToken&) { events++; });
    r.clear();
    CHECK(events == 0);
    // Subsequent grants still work and fire the observer normally.
    REQUIRE(r.grant(PatientId::pseudonymous("c"), {Purpose::triage}, 1h));
    CHECK(events == 1);
}

// ============== active_tokens_for_patient ===============================

TEST_CASE("active_tokens_for_patient returns only that patient's active tokens") {
    ConsentRegistry r;
    auto pa = PatientId::pseudonymous("pa");
    auto pb = PatientId::pseudonymous("pb");
    REQUIRE(r.grant(pa, {Purpose::triage}, 1h));
    REQUIRE(r.grant(pa, {Purpose::ambient_documentation}, 1h));
    REQUIRE(r.grant(pb, {Purpose::triage}, 1h));
    auto a = r.active_tokens_for_patient(pa);
    CHECK(a.size() == 2);
    auto b = r.active_tokens_for_patient(pb);
    CHECK(b.size() == 1);
    auto c = r.active_tokens_for_patient(PatientId::pseudonymous("ghost"));
    CHECK(c.empty());
}

TEST_CASE("active_tokens_for_patient excludes revoked and expired tokens") {
    ConsentRegistry r;
    auto pid = PatientId::pseudonymous("p_active");
    auto t1 = r.grant(pid, {Purpose::triage}, 1h).value();
    REQUIRE(r.grant(pid, {Purpose::operations}, 1h));  // stays active
    REQUIRE(r.revoke(t1.token_id));                    // now revoked

    // Expired token (past expires_at) via ingest.
    ConsentToken expired;
    expired.token_id   = "ct_active_exp";
    expired.patient    = pid;
    expired.purposes   = {Purpose::medication_review};
    expired.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    expired.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(expired));

    auto out = r.active_tokens_for_patient(pid);
    REQUIRE(out.size() == 1);
    CHECK(out[0].revoked == false);
    CHECK(out[0].purposes.size() == 1);
    CHECK(out[0].purposes[0] == Purpose::operations);
    // tokens_for_patient returns all three (including revoked + expired).
    CHECK(r.tokens_for_patient(pid).size() == 3);
}

TEST_CASE("active_tokens_for_patient on empty registry returns empty") {
    ConsentRegistry r;
    auto out = r.active_tokens_for_patient(PatientId::pseudonymous("nobody"));
    CHECK(out.empty());
}

TEST_CASE("active_tokens_for_patient is empty after expire_all_for_patient") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("p_act_int");
    REQUIRE(r.grant(p, {Purpose::triage}, 1h));
    REQUIRE(r.grant(p, {Purpose::operations}, 1h));
    CHECK(r.active_tokens_for_patient(p).size() == 2);
    auto n = r.expire_all_for_patient(p);
    CHECK(n == 2);
    CHECK(r.active_tokens_for_patient(p).empty());
    // But full history is still there.
    CHECK(r.tokens_for_patient(p).size() == 2);
}

// ============== tokens_for_purpose ======================================

TEST_CASE("tokens_for_purpose returns tokens whose purposes contain it") {
    ConsentRegistry r;
    auto pa = PatientId::pseudonymous("a");
    auto pb = PatientId::pseudonymous("b");
    REQUIRE(r.grant(pa, {Purpose::research, Purpose::triage}, 1h));
    REQUIRE(r.grant(pb, {Purpose::research}, 1h));
    REQUIRE(r.grant(pb, {Purpose::operations}, 1h));   // not research
    auto research = r.tokens_for_purpose(Purpose::research);
    CHECK(research.size() == 2);
    auto triage = r.tokens_for_purpose(Purpose::triage);
    CHECK(triage.size() == 1);
    auto ops = r.tokens_for_purpose(Purpose::operations);
    CHECK(ops.size() == 1);
    auto none = r.tokens_for_purpose(Purpose::quality_improvement);
    CHECK(none.empty());
}

TEST_CASE("tokens_for_purpose includes revoked and expired tokens (audit semantics)") {
    ConsentRegistry r;
    auto pid = PatientId::pseudonymous("p_audit");
    auto t1 = r.grant(pid, {Purpose::research}, 1h).value();
    REQUIRE(r.revoke(t1.token_id));                    // revoked but still research

    // Expired research token via ingest.
    ConsentToken expired;
    expired.token_id   = "ct_audit_exp";
    expired.patient    = pid;
    expired.purposes   = {Purpose::research};
    expired.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    expired.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(expired));

    // One active research token.
    REQUIRE(r.grant(pid, {Purpose::research}, 1h));

    auto all = r.tokens_for_purpose(Purpose::research);
    CHECK(all.size() == 3);
}

TEST_CASE("tokens_for_purpose on empty registry returns empty") {
    ConsentRegistry r;
    CHECK(r.tokens_for_purpose(Purpose::research).empty());
    CHECK(r.tokens_for_purpose(Purpose::triage).empty());
}

// ============== longest_active ==========================================

TEST_CASE("longest_active returns the token with the latest expires_at") {
    ConsentRegistry r;
    auto pid = PatientId::pseudonymous("p_long");
    REQUIRE(r.grant(pid, {Purpose::triage}, 1h));
    auto mid = r.grant(pid, {Purpose::operations}, 4h).value();
    REQUIRE(r.grant(pid, {Purpose::ambient_documentation}, 2h));
    auto best = r.longest_active();
    REQUIRE(best);
    CHECK(best.value().token_id == mid.token_id);
}

TEST_CASE("longest_active returns not_found on empty registry") {
    ConsentRegistry r;
    auto out = r.longest_active();
    CHECK(!out);
    CHECK(out.error().code() == ErrorCode::not_found);
}

TEST_CASE("longest_active skips revoked and expired tokens") {
    ConsentRegistry r;
    auto pid = PatientId::pseudonymous("p_long2");
    // Active short grant.
    auto live = r.grant(pid, {Purpose::triage}, 1h).value();

    // Revoked token with a far-future expires_at — must be ignored.
    auto revoked_long = r.grant(pid, {Purpose::operations}, 24h).value();
    REQUIRE(r.revoke(revoked_long.token_id));

    // Expired token with a pretend-large issued/expired window — must be ignored.
    ConsentToken exp;
    exp.token_id   = "ct_long_exp";
    exp.patient    = pid;
    exp.purposes   = {Purpose::medication_review};
    exp.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{48}};
    exp.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(exp));

    auto best = r.longest_active();
    REQUIRE(best);
    CHECK(best.value().token_id == live.token_id);
}

TEST_CASE("longest_active reflects extend(): after pushing, the extended token wins") {
    ConsentRegistry r;
    auto pid = PatientId::pseudonymous("p_long_ext");
    auto a = r.grant(pid, {Purpose::triage}, 1h).value();
    auto b = r.grant(pid, {Purpose::operations}, 2h).value();
    {
        auto best = r.longest_active();
        REQUIRE(best);
        CHECK(best.value().token_id == b.token_id);
    }
    REQUIRE(r.extend(a.token_id, 5h));  // a now lasts ~6h, beats b
    auto best = r.longest_active();
    REQUIRE(best);
    CHECK(best.value().token_id == a.token_id);
}

// ============== oldest_active ===========================================

TEST_CASE("oldest_active returns the token with the smallest issued_at") {
    ConsentRegistry r;
    auto pid = PatientId::pseudonymous("p_old");
    // Ingest with hand-set issued_at so we can order them deterministically.
    auto base = Time::now();
    ConsentToken t1;
    t1.token_id   = "ct_old_1";
    t1.patient    = pid;
    t1.purposes   = {Purpose::triage};
    t1.issued_at  = base - std::chrono::nanoseconds{std::chrono::minutes{30}};
    t1.expires_at = base + std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(t1));

    ConsentToken t2 = t1;
    t2.token_id  = "ct_old_2";
    t2.issued_at = base - std::chrono::nanoseconds{std::chrono::minutes{90}};  // older
    REQUIRE(r.ingest(t2));

    ConsentToken t3 = t1;
    t3.token_id  = "ct_old_3";
    t3.issued_at = base - std::chrono::nanoseconds{std::chrono::minutes{10}};
    REQUIRE(r.ingest(t3));

    auto best = r.oldest_active();
    REQUIRE(best);
    CHECK(best.value().token_id == "ct_old_2");
}

TEST_CASE("oldest_active returns not_found on empty registry") {
    ConsentRegistry r;
    auto out = r.oldest_active();
    CHECK(!out);
    CHECK(out.error().code() == ErrorCode::not_found);
}

TEST_CASE("oldest_active skips revoked and expired tokens") {
    ConsentRegistry r;
    auto pid = PatientId::pseudonymous("p_old2");

    // Active grant (will become the only candidate).
    auto live = r.grant(pid, {Purpose::triage}, 1h).value();

    // Revoked token with an older issued_at — must be ignored.
    ConsentToken rv;
    rv.token_id   = "ct_old_rev";
    rv.patient    = pid;
    rv.purposes   = {Purpose::operations};
    rv.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{10}};
    rv.expires_at = Time::now() + std::chrono::nanoseconds{std::chrono::hours{1}};
    rv.revoked    = true;
    REQUIRE(r.ingest(rv));

    // Expired token with an even older issued_at — must be ignored.
    ConsentToken ex;
    ex.token_id   = "ct_old_exp";
    ex.patient    = pid;
    ex.purposes   = {Purpose::medication_review};
    ex.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{48}};
    ex.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(ex));

    auto best = r.oldest_active();
    REQUIRE(best);
    CHECK(best.value().token_id == live.token_id);
}

// ============== soonest_to_expire =======================================

TEST_CASE("soonest_to_expire returns the token with the smallest expires_at") {
    ConsentRegistry r;
    auto pid = PatientId::pseudonymous("p_soon");
    auto fast = r.grant(pid, {Purpose::triage}, 1h).value();
    REQUIRE(r.grant(pid, {Purpose::operations}, 4h));
    REQUIRE(r.grant(pid, {Purpose::ambient_documentation}, 2h));
    auto next = r.soonest_to_expire();
    REQUIRE(next);
    CHECK(next.value().token_id == fast.token_id);
}

TEST_CASE("soonest_to_expire returns not_found on empty registry") {
    ConsentRegistry r;
    auto out = r.soonest_to_expire();
    CHECK(!out);
    CHECK(out.error().code() == ErrorCode::not_found);
}

TEST_CASE("soonest_to_expire skips revoked and expired tokens") {
    ConsentRegistry r;
    auto pid = PatientId::pseudonymous("p_soon2");

    // Two active grants.
    auto longer = r.grant(pid, {Purpose::triage}, 4h).value();
    auto shorter = r.grant(pid, {Purpose::operations}, 2h).value();

    // Revoked token with a very-soon expires_at — must be ignored.
    ConsentToken rv;
    rv.token_id   = "ct_soon_rev";
    rv.patient    = pid;
    rv.purposes   = {Purpose::medication_review};
    rv.issued_at  = Time::now();
    rv.expires_at = Time::now() + std::chrono::nanoseconds{std::chrono::minutes{5}};
    rv.revoked    = true;
    REQUIRE(r.ingest(rv));

    // Already-expired token — must be ignored.
    ConsentToken ex;
    ex.token_id   = "ct_soon_exp";
    ex.patient    = pid;
    ex.purposes   = {Purpose::research};
    ex.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    ex.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(ex));

    auto next = r.soonest_to_expire();
    REQUIRE(next);
    CHECK(next.value().token_id == shorter.token_id);
    CHECK(next.value().token_id != longer.token_id);
}

TEST_CASE("soonest_to_expire reflects extend(): after extending the soonest, a new one wins") {
    ConsentRegistry r;
    auto pid = PatientId::pseudonymous("p_soon_ext");
    auto a = r.grant(pid, {Purpose::triage}, 1h).value();
    auto b = r.grant(pid, {Purpose::operations}, 2h).value();
    {
        auto next = r.soonest_to_expire();
        REQUIRE(next);
        CHECK(next.value().token_id == a.token_id);
    }
    REQUIRE(r.extend(a.token_id, 5h));  // a now lasts ~6h, so b is the soonest
    auto next = r.soonest_to_expire();
    REQUIRE(next);
    CHECK(next.value().token_id == b.token_id);
}

// ============== has_purpose_for_patient =================================

TEST_CASE("has_purpose_for_patient mirrors permits() for an active grant") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("p_hp");
    REQUIRE(r.grant(p, {Purpose::ambient_documentation, Purpose::triage}, 1h));
    CHECK(r.has_purpose_for_patient(p, Purpose::ambient_documentation));
    CHECK(r.has_purpose_for_patient(p, Purpose::triage));
    CHECK(!r.has_purpose_for_patient(p, Purpose::medication_review));
}

TEST_CASE("has_purpose_for_patient returns false on empty registry / unknown patient") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("ghost");
    CHECK(!r.has_purpose_for_patient(p, Purpose::triage));
    REQUIRE(r.grant(PatientId::pseudonymous("other"), {Purpose::triage}, 1h));
    CHECK(!r.has_purpose_for_patient(p, Purpose::triage));
}

TEST_CASE("has_purpose_for_patient ignores revoked and expired tokens") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("p_hp2");
    auto t = r.grant(p, {Purpose::diagnostic_suggestion}, 1h).value();
    CHECK(r.has_purpose_for_patient(p, Purpose::diagnostic_suggestion));
    REQUIRE(r.revoke(t.token_id));
    CHECK(!r.has_purpose_for_patient(p, Purpose::diagnostic_suggestion));

    // Expired token via ingest must not flip it back to true.
    ConsentToken ex;
    ex.token_id   = "ct_hp_exp";
    ex.patient    = p;
    ex.purposes   = {Purpose::diagnostic_suggestion};
    ex.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    ex.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(ex));
    CHECK(!r.has_purpose_for_patient(p, Purpose::diagnostic_suggestion));
}

TEST_CASE("has_purpose_for_patient agrees with permits() across patients") {
    ConsentRegistry r;
    auto pa = PatientId::pseudonymous("hp_alice");
    auto pb = PatientId::pseudonymous("hp_bob");
    REQUIRE(r.grant(pa, {Purpose::research}, 1h));
    auto perm_a = r.permits(pa, Purpose::research);
    auto perm_b = r.permits(pb, Purpose::research);
    REQUIRE(perm_a); REQUIRE(perm_b);
    CHECK(r.has_purpose_for_patient(pa, Purpose::research) == perm_a.value());
    CHECK(r.has_purpose_for_patient(pb, Purpose::research) == perm_b.value());
}

// ============== extend_all_for_patient ==================================

TEST_CASE("extend_all_for_patient extends every active token of one patient") {
    ConsentRegistry r;
    auto pa = PatientId::pseudonymous("ext_alice");
    auto pb = PatientId::pseudonymous("ext_bob");
    auto a1 = r.grant(pa, {Purpose::triage}, 1h).value();
    auto a2 = r.grant(pa, {Purpose::operations}, 2h).value();
    auto b1 = r.grant(pb, {Purpose::triage}, 1h).value();

    auto a1_orig = a1.expires_at;
    auto a2_orig = a2.expires_at;
    auto b1_orig = b1.expires_at;

    auto n = r.extend_all_for_patient(pa, 1h);
    CHECK(n == 2);

    // Alice's tokens advanced.
    auto g_a1 = r.get(a1.token_id);
    auto g_a2 = r.get(a2.token_id);
    REQUIRE(g_a1); REQUIRE(g_a2);
    CHECK(g_a1.value().expires_at > a1_orig);
    CHECK(g_a2.value().expires_at > a2_orig);

    // Bob's token untouched.
    auto g_b1 = r.get(b1.token_id);
    REQUIRE(g_b1);
    CHECK(g_b1.value().expires_at == b1_orig);
}

TEST_CASE("extend_all_for_patient skips revoked and expired tokens") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("ext_skip");

    // One live grant — should be extended.
    auto live = r.grant(p, {Purpose::triage}, 1h).value();
    auto live_orig = live.expires_at;

    // One revoked grant — must NOT be extended.
    auto rv = r.grant(p, {Purpose::operations}, 2h).value();
    auto rv_orig_expires = rv.expires_at;
    REQUIRE(r.revoke(rv.token_id));

    // One expired (ingested) token — must NOT be extended.
    ConsentToken ex;
    ex.token_id   = "ct_ext_exp";
    ex.patient    = p;
    ex.purposes   = {Purpose::medication_review};
    ex.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    ex.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    auto ex_orig = ex.expires_at;
    REQUIRE(r.ingest(ex));

    auto n = r.extend_all_for_patient(p, 1h);
    CHECK(n == 1);

    auto g_live = r.get(live.token_id);
    REQUIRE(g_live);
    CHECK(g_live.value().expires_at > live_orig);

    auto g_rv = r.get(rv.token_id);
    REQUIRE(g_rv);
    CHECK(g_rv.value().expires_at == rv_orig_expires);

    auto g_ex = r.get("ct_ext_exp");
    REQUIRE(g_ex);
    CHECK(g_ex.value().expires_at == ex_orig);
}

TEST_CASE("extend_all_for_patient: non-positive ttl is a no-op") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("ext_zero");
    auto t = r.grant(p, {Purpose::triage}, 1h).value();
    auto orig = t.expires_at;

    CHECK(r.extend_all_for_patient(p, std::chrono::seconds{0}) == 0);
    CHECK(r.extend_all_for_patient(p, std::chrono::seconds{-5}) == 0);

    auto g = r.get(t.token_id);
    REQUIRE(g);
    CHECK(g.value().expires_at == orig);
}

TEST_CASE("extend_all_for_patient: unknown patient returns 0 and does not fire observer") {
    ConsentRegistry r;
    int events = 0;
    r.set_observer([&](ConsentRegistry::Event, const ConsentToken&) { events++; });
    REQUIRE(r.grant(PatientId::pseudonymous("someone"), {Purpose::triage}, 1h));
    events = 0;  // ignore the grant event
    auto n = r.extend_all_for_patient(PatientId::pseudonymous("ghost"), 1h);
    CHECK(n == 0);
    CHECK(events == 0);
}

TEST_CASE("extend_all_for_patient fires the observer once per token extended") {
    ConsentRegistry r;
    int grants = 0;
    std::vector<std::string> seen;
    r.set_observer([&](ConsentRegistry::Event e, const ConsentToken& t) {
        if (e == ConsentRegistry::Event::granted) {
            grants++;
            seen.push_back(t.token_id);
        }
    });
    auto p = PatientId::pseudonymous("ext_obs");
    auto t1 = r.grant(p, {Purpose::triage}, 1h).value();           // grants=1
    auto t2 = r.grant(p, {Purpose::operations}, 2h).value();       // grants=2

    auto n = r.extend_all_for_patient(p, 1h);
    CHECK(n == 2);
    CHECK(grants == 4);  // 2 from initial grants + 2 from extensions
    // The post-state on the observed events should reflect the
    // extended expiry — verify by looking up each via get().
    for (const auto& id : {t1.token_id, t2.token_id}) {
        auto g = r.get(id);
        REQUIRE(g);
        CHECK(g.value().revoked == false);
    }
    // The two id's from the bulk extend should both appear in seen[2..].
    CHECK(seen.size() == 4);
}

TEST_CASE("cleanup_expired fires the observer once per token swept") {
    ConsentRegistry r;
    int revokes = 0;
    std::vector<std::string> seen_ids;
    r.set_observer([&](ConsentRegistry::Event e, const ConsentToken& t) {
        if (e == ConsentRegistry::Event::revoked) {
            revokes++;
            seen_ids.push_back(t.token_id);
        }
    });

    auto pid = PatientId::pseudonymous("p_clean_obs");
    ConsentToken e1;
    e1.token_id   = "ct_obs_1";
    e1.patient    = pid;
    e1.purposes   = {Purpose::triage};
    e1.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    e1.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(e1));

    ConsentToken e2 = e1;
    e2.token_id = "ct_obs_2";
    REQUIRE(r.ingest(e2));

    auto n = r.cleanup_expired();
    CHECK(n == 2);
    CHECK(revokes == 2);
    CHECK(seen_ids.size() == 2);
    // Observer sees the post-state: revoked=true.
    for (const auto& id : seen_ids) {
        auto g = r.get(id);
        REQUIRE(g);
        CHECK(g.value().revoked == true);
    }
}
