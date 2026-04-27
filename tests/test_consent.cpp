// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

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

// ============== patient_count ===========================================

TEST_CASE("patient_count is zero on empty registry") {
    ConsentRegistry r;
    CHECK(r.patient_count() == 0);
}

TEST_CASE("patient_count counts distinct patients across multiple tokens") {
    ConsentRegistry r;
    auto pa = PatientId::pseudonymous("alice");
    auto pb = PatientId::pseudonymous("bob");
    auto pc = PatientId::pseudonymous("carol");
    REQUIRE(r.grant(pa, {Purpose::triage}, 1h));
    REQUIRE(r.grant(pa, {Purpose::operations}, 1h));     // same patient
    REQUIRE(r.grant(pb, {Purpose::triage}, 1h));
    REQUIRE(r.grant(pc, {Purpose::ambient_documentation}, 1h));
    CHECK(r.patient_count() == 3);
    CHECK(r.total_count() == 4);
}

TEST_CASE("patient_count includes revoked and expired patients") {
    ConsentRegistry r;
    auto pa = PatientId::pseudonymous("pc_alice");
    auto pb = PatientId::pseudonymous("pc_bob");
    auto t = r.grant(pa, {Purpose::triage}, 1h).value();
    REQUIRE(r.revoke(t.token_id));  // revoked but still on file

    // Expired token for a different patient.
    ConsentToken ex;
    ex.token_id   = "ct_pc_exp";
    ex.patient    = pb;
    ex.purposes   = {Purpose::operations};
    ex.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    ex.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(ex));

    CHECK(r.patient_count() == 2);
}

TEST_CASE("patient_count drops to zero after clear()") {
    ConsentRegistry r;
    REQUIRE(r.grant(PatientId::pseudonymous("a"), {Purpose::triage}, 1h));
    REQUIRE(r.grant(PatientId::pseudonymous("b"), {Purpose::triage}, 1h));
    CHECK(r.patient_count() == 2);
    r.clear();
    CHECK(r.patient_count() == 0);
}

// ============== patients ================================================

TEST_CASE("patients returns sorted distinct PatientIds") {
    ConsentRegistry r;
    // Insert in non-sorted order; expect lexicographic by underlying string.
    REQUIRE(r.grant(PatientId::pseudonymous("charlie"), {Purpose::triage}, 1h));
    REQUIRE(r.grant(PatientId::pseudonymous("alice"),   {Purpose::triage}, 1h));
    REQUIRE(r.grant(PatientId::pseudonymous("bob"),     {Purpose::triage}, 1h));
    REQUIRE(r.grant(PatientId::pseudonymous("alice"),   {Purpose::operations}, 1h));  // dup
    auto out = r.patients();
    REQUIRE(out.size() == 3);
    CHECK(out[0].str() == std::string_view{"pat:alice"});
    CHECK(out[1].str() == std::string_view{"pat:bob"});
    CHECK(out[2].str() == std::string_view{"pat:charlie"});
}

TEST_CASE("patients on empty registry returns empty vector") {
    ConsentRegistry r;
    auto out = r.patients();
    CHECK(out.empty());
}

TEST_CASE("patients includes patients from revoked and expired tokens") {
    ConsentRegistry r;
    auto pa = PatientId::pseudonymous("revoked_one");
    auto pb = PatientId::pseudonymous("expired_one");
    auto pc = PatientId::pseudonymous("active_one");

    auto t = r.grant(pa, {Purpose::triage}, 1h).value();
    REQUIRE(r.revoke(t.token_id));

    ConsentToken ex;
    ex.token_id   = "ct_pat_exp";
    ex.patient    = pb;
    ex.purposes   = {Purpose::operations};
    ex.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    ex.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(ex));

    REQUIRE(r.grant(pc, {Purpose::ambient_documentation}, 1h));

    auto out = r.patients();
    REQUIRE(out.size() == 3);
    // Sorted by underlying string: "pat:active_one", "pat:expired_one",
    // "pat:revoked_one".
    CHECK(out[0].str() == std::string_view{"pat:active_one"});
    CHECK(out[1].str() == std::string_view{"pat:expired_one"});
    CHECK(out[2].str() == std::string_view{"pat:revoked_one"});
}

TEST_CASE("patients agrees with patient_count on size") {
    ConsentRegistry r;
    REQUIRE(r.grant(PatientId::pseudonymous("p1"), {Purpose::triage}, 1h));
    REQUIRE(r.grant(PatientId::pseudonymous("p2"), {Purpose::triage}, 1h));
    REQUIRE(r.grant(PatientId::pseudonymous("p1"), {Purpose::operations}, 1h));
    REQUIRE(r.grant(PatientId::pseudonymous("p3"), {Purpose::operations}, 1h));
    CHECK(r.patients().size() == r.patient_count());
    CHECK(r.patient_count() == 3);
}

// ============== summary =================================================

TEST_CASE("summary on empty registry is all zeros") {
    ConsentRegistry r;
    auto s = r.summary();
    CHECK(s.total == 0);
    CHECK(s.active == 0);
    CHECK(s.expired == 0);
    CHECK(s.revoked == 0);
    CHECK(s.patients == 0);
}

TEST_CASE("summary partitions tokens into active, expired, revoked") {
    ConsentRegistry r;
    auto pa = PatientId::pseudonymous("sum_a");
    auto pb = PatientId::pseudonymous("sum_b");

    // Two active grants for pa.
    REQUIRE(r.grant(pa, {Purpose::triage}, 1h));
    REQUIRE(r.grant(pa, {Purpose::operations}, 1h));

    // One active grant for pb, then revoked.
    auto rv = r.grant(pb, {Purpose::ambient_documentation}, 1h).value();
    REQUIRE(r.revoke(rv.token_id));

    // One expired token for pb.
    ConsentToken ex;
    ex.token_id   = "ct_sum_exp";
    ex.patient    = pb;
    ex.purposes   = {Purpose::medication_review};
    ex.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    ex.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(ex));

    auto s = r.summary();
    CHECK(s.total == 4);
    CHECK(s.active == 2);
    CHECK(s.expired == 1);
    CHECK(s.revoked == 1);
    CHECK(s.patients == 2);
    // The four buckets active/expired/revoked must sum to total.
    CHECK(s.active + s.expired + s.revoked == s.total);
}

TEST_CASE("summary agrees with individual counters") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("sum_agree");
    REQUIRE(r.grant(p, {Purpose::triage}, 1h));
    auto t = r.grant(p, {Purpose::operations}, 1h).value();
    REQUIRE(r.revoke(t.token_id));

    ConsentToken ex;
    ex.token_id   = "ct_sum_agree_exp";
    ex.patient    = p;
    ex.purposes   = {Purpose::research};
    ex.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    ex.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(ex));

    auto s = r.summary();
    CHECK(s.total == r.total_count());
    CHECK(s.active == r.active_count());
    CHECK(s.expired == r.expired_count());
    CHECK(s.patients == r.patient_count());
}

TEST_CASE("summary reflects mutations: clear then re-grant") {
    ConsentRegistry r;
    REQUIRE(r.grant(PatientId::pseudonymous("a"), {Purpose::triage}, 1h));
    REQUIRE(r.grant(PatientId::pseudonymous("b"), {Purpose::triage}, 1h));
    {
        auto s = r.summary();
        CHECK(s.total == 2);
        CHECK(s.active == 2);
        CHECK(s.patients == 2);
    }
    r.clear();
    {
        auto s = r.summary();
        CHECK(s.total == 0);
        CHECK(s.active == 0);
        CHECK(s.expired == 0);
        CHECK(s.revoked == 0);
        CHECK(s.patients == 0);
    }
    REQUIRE(r.grant(PatientId::pseudonymous("c"), {Purpose::operations}, 1h));
    auto s = r.summary();
    CHECK(s.total == 1);
    CHECK(s.active == 1);
    CHECK(s.patients == 1);
}

// ============== tokens_expiring_within ==================================

TEST_CASE("tokens_expiring_within returns tokens lapsing inside the horizon") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("tew_p");
    // Short grant: well within a 10-minute horizon.
    auto soon = r.grant(p, {Purpose::triage}, std::chrono::seconds{60}).value();
    // Long grant: well outside a 10-minute horizon.
    REQUIRE(r.grant(p, {Purpose::operations}, 24h));

    auto out = r.tokens_expiring_within(std::chrono::minutes{10});
    REQUIRE(out.size() == 1);
    CHECK(out[0].token_id == soon.token_id);
}

TEST_CASE("tokens_expiring_within with horizon <= 0 returns empty") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("tew_zero");
    REQUIRE(r.grant(p, {Purpose::triage}, std::chrono::seconds{30}));
    CHECK(r.tokens_expiring_within(std::chrono::seconds{0}).empty());
    CHECK(r.tokens_expiring_within(std::chrono::seconds{-5}).empty());
}

TEST_CASE("tokens_expiring_within excludes revoked and already-expired tokens") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("tew_skip");

    // Active short grant inside the horizon — should be returned.
    auto live = r.grant(p, {Purpose::triage}, std::chrono::seconds{60}).value();

    // Revoked grant — must not appear, even with a short remaining ttl.
    auto rv = r.grant(p, {Purpose::operations}, std::chrono::seconds{30}).value();
    REQUIRE(r.revoke(rv.token_id));

    // Already-expired token — must not appear (expires_at <= now).
    ConsentToken ex;
    ex.token_id   = "ct_tew_exp";
    ex.patient    = p;
    ex.purposes   = {Purpose::medication_review};
    ex.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    ex.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(ex));

    auto out = r.tokens_expiring_within(std::chrono::minutes{10});
    REQUIRE(out.size() == 1);
    CHECK(out[0].token_id == live.token_id);
}

TEST_CASE("tokens_expiring_within: a wider horizon picks up more tokens") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("tew_wide");
    // 1m, 30m, 5h grants.
    auto a = r.grant(p, {Purpose::triage},                std::chrono::minutes{1}).value();
    auto b = r.grant(p, {Purpose::operations},            std::chrono::minutes{30}).value();
    auto c = r.grant(p, {Purpose::ambient_documentation}, std::chrono::hours{5}).value();

    CHECK(r.tokens_expiring_within(std::chrono::minutes{5}).size() == 1);
    CHECK(r.tokens_expiring_within(std::chrono::hours{1}).size()   == 2);
    CHECK(r.tokens_expiring_within(std::chrono::hours{24}).size()  == 3);

    // Sanity: extending the soonest one beyond the horizon drops it.
    REQUIRE(r.extend(a.token_id, std::chrono::hours{10}));
    auto out = r.tokens_expiring_within(std::chrono::minutes{5});
    CHECK(out.empty());
    (void)b; (void)c;
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

// ============== active_purposes_for_patient =============================

TEST_CASE("active_purposes_for_patient returns distinct sorted purposes") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("p_apfp");
    // Two grants with overlapping purpose lists; expect distinct.
    REQUIRE(r.grant(p, {Purpose::research, Purpose::triage}, 1h));
    REQUIRE(r.grant(p, {Purpose::triage, Purpose::ambient_documentation}, 1h));
    auto out = r.active_purposes_for_patient(p);
    REQUIRE(out.size() == 3);
    // Sorted by enum value: ambient_documentation(1) < triage(3) < research(7).
    CHECK(out[0] == Purpose::ambient_documentation);
    CHECK(out[1] == Purpose::triage);
    CHECK(out[2] == Purpose::research);
}

TEST_CASE("active_purposes_for_patient is empty on empty registry / unknown patient") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("ghost");
    CHECK(r.active_purposes_for_patient(p).empty());
    REQUIRE(r.grant(PatientId::pseudonymous("other"), {Purpose::triage}, 1h));
    CHECK(r.active_purposes_for_patient(p).empty());
}

TEST_CASE("active_purposes_for_patient excludes revoked and expired tokens") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("p_apfp_skip");
    // Live grant: triage stays.
    REQUIRE(r.grant(p, {Purpose::triage}, 1h));
    // Revoked grant: research must NOT appear.
    auto rv = r.grant(p, {Purpose::research}, 1h).value();
    REQUIRE(r.revoke(rv.token_id));
    // Expired grant: operations must NOT appear.
    ConsentToken ex;
    ex.token_id   = "ct_apfp_exp";
    ex.patient    = p;
    ex.purposes   = {Purpose::operations};
    ex.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    ex.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(ex));

    auto out = r.active_purposes_for_patient(p);
    REQUIRE(out.size() == 1);
    CHECK(out[0] == Purpose::triage);
}

TEST_CASE("active_purposes_for_patient agrees with has_purpose_for_patient") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("p_apfp_agree");
    REQUIRE(r.grant(p, {Purpose::ambient_documentation,
                        Purpose::diagnostic_suggestion,
                        Purpose::medication_review}, 1h));
    auto purposes = r.active_purposes_for_patient(p);
    for (auto pu : purposes) {
        CHECK(r.has_purpose_for_patient(p, pu));
    }
    // The complement should not be permitted.
    CHECK(!r.has_purpose_for_patient(p, Purpose::research));
    CHECK(!r.has_purpose_for_patient(p, Purpose::operations));
}

// ============== extend_to ===============================================

TEST_CASE("extend_to sets expires_at to the absolute deadline") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("p_ex2");
    auto t = r.grant(p, {Purpose::triage}, 1h).value();
    auto target = t.expires_at + std::chrono::nanoseconds{std::chrono::hours{5}};
    auto t2 = r.extend_to(t.token_id, target);
    REQUIRE(t2);
    CHECK(t2.value().expires_at == target);
    auto g = r.get(t.token_id);
    REQUIRE(g);
    CHECK(g.value().expires_at == target);
}

TEST_CASE("extend_to rejects a deadline strictly earlier than current expiry") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("p_ex2_shrink");
    auto t = r.grant(p, {Purpose::triage}, 1h).value();
    auto orig = t.expires_at;
    auto earlier = t.expires_at - std::chrono::nanoseconds{std::chrono::minutes{30}};
    auto r2 = r.extend_to(t.token_id, earlier);
    CHECK(!r2);
    CHECK(r2.error().code() == ErrorCode::invalid_argument);
    // Unchanged.
    auto g = r.get(t.token_id);
    REQUIRE(g);
    CHECK(g.value().expires_at == orig);
}

TEST_CASE("extend_to allows deadline equal to current expiry (no shrink, no error)") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("p_ex2_eq");
    auto t = r.grant(p, {Purpose::triage}, 1h).value();
    auto r2 = r.extend_to(t.token_id, t.expires_at);
    REQUIRE(r2);
    CHECK(r2.value().expires_at == t.expires_at);
}

TEST_CASE("extend_to rejects revoked tokens with denied") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("p_ex2_rev");
    auto t = r.grant(p, {Purpose::triage}, 1h).value();
    REQUIRE(r.revoke(t.token_id));
    auto target = t.expires_at + std::chrono::nanoseconds{std::chrono::hours{1}};
    auto r2 = r.extend_to(t.token_id, target);
    CHECK(!r2);
    CHECK(r2.error().code() == ErrorCode::permission_denied);
}

TEST_CASE("extend_to rejects unknown token_id with not_found") {
    ConsentRegistry r;
    auto target = Time::now() + std::chrono::nanoseconds{std::chrono::hours{1}};
    auto r2 = r.extend_to("ct_ghost_ex2", target);
    CHECK(!r2);
    CHECK(r2.error().code() == ErrorCode::not_found);
}

TEST_CASE("extend_to fires the observer as a granted event") {
    ConsentRegistry r;
    int grants = 0;
    r.set_observer([&](ConsentRegistry::Event e, const ConsentToken&) {
        if (e == ConsentRegistry::Event::granted) grants++;
    });
    auto p = PatientId::pseudonymous("p_ex2_obs");
    auto t = r.grant(p, {Purpose::triage}, 1h).value();   // grants=1
    auto target = t.expires_at + std::chrono::nanoseconds{std::chrono::hours{2}};
    REQUIRE(r.extend_to(t.token_id, target));             // grants=2
    CHECK(grants == 2);
}

// ============== expired_for_patient =====================================

TEST_CASE("expired_for_patient returns only that patient's expired non-revoked tokens") {
    ConsentRegistry r;
    auto pa = PatientId::pseudonymous("efp_alice");
    auto pb = PatientId::pseudonymous("efp_bob");

    // Alice has one expired, one live.
    ConsentToken a_exp;
    a_exp.token_id   = "ct_efp_a";
    a_exp.patient    = pa;
    a_exp.purposes   = {Purpose::triage};
    a_exp.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    a_exp.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(a_exp));
    REQUIRE(r.grant(pa, {Purpose::operations}, 1h));

    // Bob has one expired.
    ConsentToken b_exp;
    b_exp.token_id   = "ct_efp_b";
    b_exp.patient    = pb;
    b_exp.purposes   = {Purpose::research};
    b_exp.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{3}};
    b_exp.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    REQUIRE(r.ingest(b_exp));

    auto a_out = r.expired_for_patient(pa);
    REQUIRE(a_out.size() == 1);
    CHECK(a_out[0].token_id == "ct_efp_a");

    auto b_out = r.expired_for_patient(pb);
    REQUIRE(b_out.size() == 1);
    CHECK(b_out[0].token_id == "ct_efp_b");

    auto ghost = r.expired_for_patient(PatientId::pseudonymous("ghost"));
    CHECK(ghost.empty());
}

TEST_CASE("expired_for_patient excludes revoked tokens (even if expiry passed)") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("efp_rev");
    ConsentToken e;
    e.token_id   = "ct_efp_rev";
    e.patient    = p;
    e.purposes   = {Purpose::triage};
    e.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    e.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    e.revoked    = true;
    REQUIRE(r.ingest(e));
    CHECK(r.expired_for_patient(p).empty());
}

TEST_CASE("expired_for_patient excludes live (still-active) tokens") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("efp_live");
    REQUIRE(r.grant(p, {Purpose::triage}, 1h));
    REQUIRE(r.grant(p, {Purpose::operations}, 2h));
    CHECK(r.expired_for_patient(p).empty());
}

TEST_CASE("expired_for_patient becomes empty after cleanup_expired sweep") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("efp_sweep");
    ConsentToken e;
    e.token_id   = "ct_efp_sweep";
    e.patient    = p;
    e.purposes   = {Purpose::triage};
    e.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    e.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(e));
    CHECK(r.expired_for_patient(p).size() == 1);
    auto n = r.cleanup_expired();
    CHECK(n == 1);
    // After sweep the token is revoked, so it no longer qualifies as
    // "expired but not revoked".
    CHECK(r.expired_for_patient(p).empty());
}

// ============== dump_state_json =========================================

TEST_CASE("dump_state_json emits stable schema with all fields") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("dump_alice");
    auto t = r.grant(p, {Purpose::triage, Purpose::research}, 1h).value();

    auto s = r.dump_state_json();
    auto j = nlohmann::json::parse(s);
    REQUIRE(j.contains("tokens"));
    REQUIRE(j["tokens"].is_array());
    REQUIRE(j["tokens"].size() == 1);
    const auto& tok = j["tokens"][0];
    CHECK(tok.contains("token_id"));
    CHECK(tok.contains("patient"));
    CHECK(tok.contains("purposes"));
    CHECK(tok.contains("issued_at"));
    CHECK(tok.contains("expires_at"));
    CHECK(tok.contains("revoked"));
    CHECK(tok["token_id"].get<std::string>() == t.token_id);
    CHECK(tok["patient"].get<std::string>() == std::string{p.str()});
    CHECK(tok["revoked"].get<bool>() == false);
    REQUIRE(tok["purposes"].is_array());
    REQUIRE(tok["purposes"].size() == 2);
    // ISO-8601 strings have a 'T' separator.
    CHECK(tok["issued_at"].get<std::string>().find('T') != std::string::npos);
    CHECK(tok["expires_at"].get<std::string>().find('T') != std::string::npos);
}

TEST_CASE("dump_state_json on empty registry returns {\"tokens\": []}") {
    ConsentRegistry r;
    auto s = r.dump_state_json();
    auto j = nlohmann::json::parse(s);
    REQUIRE(j.contains("tokens"));
    REQUIRE(j["tokens"].is_array());
    CHECK(j["tokens"].empty());
}

TEST_CASE("dump_state_json includes revoked and expired tokens with correct flags") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("dump_mix");

    // Active grant.
    REQUIRE(r.grant(p, {Purpose::ambient_documentation}, 1h));

    // Revoked grant.
    auto rv = r.grant(p, {Purpose::operations}, 1h).value();
    REQUIRE(r.revoke(rv.token_id));

    // Expired (ingested) grant.
    ConsentToken ex;
    ex.token_id   = "ct_dump_exp";
    ex.patient    = p;
    ex.purposes   = {Purpose::medication_review};
    ex.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    ex.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(ex));

    auto j = nlohmann::json::parse(r.dump_state_json());
    REQUIRE(j["tokens"].size() == 3);

    int revoked_seen = 0;
    int active_seen  = 0;
    int expired_seen = 0;
    for (const auto& tok : j["tokens"]) {
        if (tok["revoked"].get<bool>()) {
            revoked_seen++;
        } else if (tok["token_id"].get<std::string>() == "ct_dump_exp") {
            expired_seen++;
        } else {
            active_seen++;
        }
    }
    CHECK(revoked_seen == 1);
    CHECK(active_seen == 1);
    CHECK(expired_seen == 1);
}

TEST_CASE("dump_state_json round-trips purposes via purpose_from_string") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("dump_purposes");
    REQUIRE(r.grant(p, {Purpose::quality_improvement, Purpose::risk_stratification}, 1h));
    auto j = nlohmann::json::parse(r.dump_state_json());
    REQUIRE(j["tokens"].size() == 1);
    const auto& purposes = j["tokens"][0]["purposes"];
    REQUIRE(purposes.is_array());
    REQUIRE(purposes.size() == 2);
    std::unordered_set<std::uint8_t> codes;
    for (const auto& s : purposes) {
        auto pr = purpose_from_string(s.get<std::string>());
        REQUIRE(pr);
        codes.insert(static_cast<std::uint8_t>(pr.value()));
    }
    CHECK(codes.count(static_cast<std::uint8_t>(Purpose::quality_improvement)) == 1);
    CHECK(codes.count(static_cast<std::uint8_t>(Purpose::risk_stratification)) == 1);
}

// ============== stats_for_patient =======================================

TEST_CASE("stats_for_patient returns zeroed Summary for an unknown patient") {
    ConsentRegistry r;
    auto known   = PatientId::pseudonymous("known_pid");
    auto unknown = PatientId::pseudonymous("ghost_pid");
    REQUIRE(r.grant(known, {Purpose::triage}, 1h));

    auto s = r.stats_for_patient(unknown);
    CHECK(s.total    == 0);
    CHECK(s.active   == 0);
    CHECK(s.expired  == 0);
    CHECK(s.revoked  == 0);
    CHECK(s.patients == 0);
}

TEST_CASE("stats_for_patient counts active / expired / revoked correctly") {
    ConsentRegistry r;
    auto pid = PatientId::pseudonymous("stats_pid");

    // Active.
    REQUIRE(r.grant(pid, {Purpose::ambient_documentation}, 1h));

    // Revoked.
    auto t_rev = r.grant(pid, {Purpose::triage}, 1h);
    REQUIRE(t_rev);
    REQUIRE(r.revoke(t_rev.value().token_id));

    // Expired (non-revoked) — via ingest with past expires_at.
    ConsentToken e;
    e.token_id   = "ct_stats_expired";
    e.patient    = pid;
    e.purposes   = {Purpose::operations};
    e.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    e.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(e));

    auto s = r.stats_for_patient(pid);
    CHECK(s.total    == 3);
    CHECK(s.active   == 1);
    CHECK(s.expired  == 1);
    CHECK(s.revoked  == 1);
    CHECK(s.patients == 1);
}

TEST_CASE("stats_for_patient is scoped: other patients' tokens don't leak in") {
    ConsentRegistry r;
    auto a = PatientId::pseudonymous("stats_a");
    auto b = PatientId::pseudonymous("stats_b");

    REQUIRE(r.grant(a, {Purpose::triage}, 1h));
    REQUIRE(r.grant(a, {Purpose::medication_review}, 1h));
    REQUIRE(r.grant(b, {Purpose::research}, 1h));
    auto t_b_rev = r.grant(b, {Purpose::operations}, 1h);
    REQUIRE(t_b_rev);
    REQUIRE(r.revoke(t_b_rev.value().token_id));

    auto sa = r.stats_for_patient(a);
    CHECK(sa.total    == 2);
    CHECK(sa.active   == 2);
    CHECK(sa.revoked  == 0);
    CHECK(sa.patients == 1);

    auto sb = r.stats_for_patient(b);
    CHECK(sb.total    == 2);
    CHECK(sb.active   == 1);
    CHECK(sb.revoked  == 1);
    CHECK(sb.patients == 1);

    // Cross-check vs global summary.
    auto g = r.summary();
    CHECK(g.total    == sa.total + sb.total);
    CHECK(g.active   == sa.active + sb.active);
    CHECK(g.revoked  == sa.revoked + sb.revoked);
    CHECK(g.patients == 2);
}

// ============== is_patient_known ========================================

TEST_CASE("is_patient_known is false for the empty registry / unknown patient") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("nobody");
    CHECK(!r.is_patient_known(p));

    REQUIRE(r.grant(PatientId::pseudonymous("someone_else"), {Purpose::triage}, 1h));
    CHECK(!r.is_patient_known(p));
}

TEST_CASE("is_patient_known sees every state — active, revoked, expired") {
    ConsentRegistry r;
    auto a = PatientId::pseudonymous("known_active");
    auto b = PatientId::pseudonymous("known_revoked");
    auto c = PatientId::pseudonymous("known_expired");

    REQUIRE(r.grant(a, {Purpose::triage}, 1h));

    auto tb = r.grant(b, {Purpose::triage}, 1h);
    REQUIRE(tb);
    REQUIRE(r.revoke(tb.value().token_id));

    ConsentToken e;
    e.token_id   = "ct_known_expired";
    e.patient    = c;
    e.purposes   = {Purpose::triage};
    e.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    e.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(e));

    CHECK(r.is_patient_known(a));
    CHECK(r.is_patient_known(b));
    CHECK(r.is_patient_known(c));
}

TEST_CASE("is_patient_known agrees with stats_for_patient.patients") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("agree_pid");
    auto q = PatientId::pseudonymous("absent_pid");
    REQUIRE(r.grant(p, {Purpose::quality_improvement}, 1h));

    CHECK(r.is_patient_known(p) == (r.stats_for_patient(p).patients == 1));
    CHECK(r.is_patient_known(q) == (r.stats_for_patient(q).patients == 1));
    CHECK(r.is_patient_known(p));
    CHECK(!r.is_patient_known(q));
}

// ============== recently_revoked ========================================

TEST_CASE("recently_revoked returns empty for window <= 0") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("rr_zero");
    auto t = r.grant(p, {Purpose::triage}, 1h);
    REQUIRE(t);
    REQUIRE(r.revoke(t.value().token_id));

    CHECK(r.recently_revoked(0s).empty());
    CHECK(r.recently_revoked(-1s).empty());
}

TEST_CASE("recently_revoked excludes long-running grants that were revoked") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("rr_long");

    // Old grant (issued 10h ago), since revoked: outside a 1h window.
    ConsentToken old;
    old.token_id   = "ct_rr_old";
    old.patient    = p;
    old.purposes   = {Purpose::triage};
    old.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{10}};
    old.expires_at = Time::now() + std::chrono::nanoseconds{std::chrono::hours{1}};
    old.revoked    = true;
    REQUIRE(r.ingest(old));

    // Fresh grant + revoke: within window.
    auto fresh = r.grant(p, {Purpose::medication_review}, 1h);
    REQUIRE(fresh);
    REQUIRE(r.revoke(fresh.value().token_id));

    auto out = r.recently_revoked(1h);
    REQUIRE(out.size() == 1);
    CHECK(out[0].token_id == fresh.value().token_id);
}

TEST_CASE("recently_revoked is sorted by issued_at descending; excludes non-revoked") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("rr_sort");

    // Three revoked grants with controlled issued_at inside a 1h window.
    ConsentToken older;
    older.token_id   = "ct_rr_older";
    older.patient    = p;
    older.purposes   = {Purpose::triage};
    older.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::minutes{50}};
    older.expires_at = Time::now() + std::chrono::nanoseconds{std::chrono::hours{1}};
    older.revoked    = true;
    REQUIRE(r.ingest(older));

    ConsentToken middle = older;
    middle.token_id  = "ct_rr_middle";
    middle.issued_at = Time::now() - std::chrono::nanoseconds{std::chrono::minutes{30}};
    REQUIRE(r.ingest(middle));

    ConsentToken newest = older;
    newest.token_id  = "ct_rr_newest";
    newest.issued_at = Time::now() - std::chrono::nanoseconds{std::chrono::minutes{5}};
    REQUIRE(r.ingest(newest));

    // A non-revoked recent grant — should NOT appear.
    REQUIRE(r.grant(p, {Purpose::operations}, 1h));

    auto out = r.recently_revoked(1h);
    REQUIRE(out.size() == 3);
    CHECK(out[0].token_id == "ct_rr_newest");
    CHECK(out[1].token_id == "ct_rr_middle");
    CHECK(out[2].token_id == "ct_rr_older");

    // A narrower window excludes the older entries.
    auto narrow = r.recently_revoked(10min);
    REQUIRE(narrow.size() == 1);
    CHECK(narrow[0].token_id == "ct_rr_newest");
}

// ============== most_recently_granted ===================================

TEST_CASE("most_recently_granted returns not_found on empty registry") {
    ConsentRegistry r;
    auto out = r.most_recently_granted();
    CHECK(!out);
}

TEST_CASE("most_recently_granted picks the largest issued_at across all states") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("mrg_pid");

    // Old expired token via ingest.
    ConsentToken old;
    old.token_id   = "ct_mrg_old";
    old.patient    = p;
    old.purposes   = {Purpose::triage};
    old.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{5}};
    old.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{4}};
    REQUIRE(r.ingest(old));

    // Live grant — newest.
    auto fresh = r.grant(p, {Purpose::medication_review}, 1h);
    REQUIRE(fresh);

    auto out = r.most_recently_granted();
    REQUIRE(out);
    CHECK(out.value().token_id == fresh.value().token_id);
}

TEST_CASE("most_recently_granted ignores revoked/expired status — newest wins") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("mrg_states");

    // Active grant (older).
    auto active = r.grant(p, {Purpose::triage}, 1h);
    REQUIRE(active);

    // Revoked grant injected with a strictly newer issued_at.
    ConsentToken rv;
    rv.token_id   = "ct_mrg_revoked_newest";
    rv.patient    = p;
    rv.purposes   = {Purpose::operations};
    rv.issued_at  = active.value().issued_at +
                    std::chrono::nanoseconds{std::chrono::seconds{30}};
    rv.expires_at = rv.issued_at + std::chrono::nanoseconds{std::chrono::hours{1}};
    rv.revoked    = true;
    REQUIRE(r.ingest(rv));

    auto out = r.most_recently_granted();
    REQUIRE(out);
    CHECK(out.value().token_id == "ct_mrg_revoked_newest");
    CHECK(out.value().revoked);
}

// ============== permits_any_purpose =====================================

TEST_CASE("permits_any_purpose: false on empty registry / unknown patient") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("pap_empty");
    CHECK(!r.permits_any_purpose(p));

    auto other = PatientId::pseudonymous("pap_other");
    REQUIRE(r.grant(other, {Purpose::triage}, 1h));
    CHECK(!r.permits_any_purpose(p));
}

TEST_CASE("permits_any_purpose: true while any active token exists, false after revoke") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("pap_basic");
    auto t = r.grant(p, {Purpose::ambient_documentation}, 1h);
    REQUIRE(t);
    CHECK(r.permits_any_purpose(p));

    REQUIRE(r.revoke(t.value().token_id));
    CHECK(!r.permits_any_purpose(p));
}

TEST_CASE("permits_any_purpose: ignores expired and revoked, sees any live grant") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("pap_mixed");

    // Expired token via ingest.
    ConsentToken expired;
    expired.token_id   = "ct_pap_expired";
    expired.patient    = p;
    expired.purposes   = {Purpose::triage};
    expired.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    expired.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(expired));
    CHECK(!r.permits_any_purpose(p));

    // Revoked token via ingest.
    ConsentToken revoked;
    revoked.token_id   = "ct_pap_revoked";
    revoked.patient    = p;
    revoked.purposes   = {Purpose::operations};
    revoked.issued_at  = Time::now();
    revoked.expires_at = Time::now() + std::chrono::nanoseconds{std::chrono::hours{1}};
    revoked.revoked    = true;
    REQUIRE(r.ingest(revoked));
    CHECK(!r.permits_any_purpose(p));

    // Now add a live grant — should flip true.
    auto live = r.grant(p, {Purpose::research}, 1h);
    REQUIRE(live);
    CHECK(r.permits_any_purpose(p));
}

// ============== token_count_for_patient =================================

TEST_CASE("token_count_for_patient: zero on empty registry / unknown patient") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("tcp_empty");
    CHECK(r.token_count_for_patient(p) == 0u);

    auto other = PatientId::pseudonymous("tcp_other");
    REQUIRE(r.grant(other, {Purpose::triage}, 1h));
    CHECK(r.token_count_for_patient(p) == 0u);
}

TEST_CASE("token_count_for_patient: counts active + revoked + expired") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("tcp_mixed");

    auto active = r.grant(p, {Purpose::triage}, 1h);
    REQUIRE(active);

    auto to_revoke = r.grant(p, {Purpose::operations}, 1h);
    REQUIRE(to_revoke);
    REQUIRE(r.revoke(to_revoke.value().token_id));

    ConsentToken expired;
    expired.token_id   = "ct_tcp_expired";
    expired.patient    = p;
    expired.purposes   = {Purpose::research};
    expired.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    expired.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(expired));

    CHECK(r.token_count_for_patient(p) == 3u);
}

TEST_CASE("token_count_for_patient: agrees with tokens_for_patient().size()") {
    ConsentRegistry r;
    auto p1 = PatientId::pseudonymous("tcp_agree_1");
    auto p2 = PatientId::pseudonymous("tcp_agree_2");

    REQUIRE(r.grant(p1, {Purpose::triage}, 1h));
    REQUIRE(r.grant(p1, {Purpose::operations}, 1h));
    REQUIRE(r.grant(p2, {Purpose::research}, 1h));

    CHECK(r.token_count_for_patient(p1) == r.tokens_for_patient(p1).size());
    CHECK(r.token_count_for_patient(p2) == r.tokens_for_patient(p2).size());
    CHECK(r.token_count_for_patient(p1) == 2u);
    CHECK(r.token_count_for_patient(p2) == 1u);
}

// ============== remove ==================================================

TEST_CASE("remove returns not_found for unknown token id") {
    ConsentRegistry r;
    auto a = r.remove("ct_does_not_exist");
    CHECK(!a);
    auto b = r.remove("");
    CHECK(!b);
}

TEST_CASE("remove erases the entry — get / token_exists go negative") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("rm_basic");
    auto t = r.grant(p, {Purpose::triage}, 1h);
    REQUIRE(t);
    const auto id = t.value().token_id;

    CHECK(r.token_exists(id));
    REQUIRE(r.remove(id));
    CHECK(!r.token_exists(id));
    CHECK(!r.get(id));

    // remove is not idempotent — second call is not_found.
    CHECK(!r.remove(id));
}

TEST_CASE("remove does NOT fire the observer (destructive, ledger-invisible)") {
    ConsentRegistry r;
    std::atomic<int> grant_events{0};
    std::atomic<int> revoke_events{0};
    r.set_observer([&](ConsentRegistry::Event e, const ConsentToken&) {
        if (e == ConsentRegistry::Event::granted) grant_events++;
        else if (e == ConsentRegistry::Event::revoked) revoke_events++;
    });

    auto p = PatientId::pseudonymous("rm_obs");
    auto t = r.grant(p, {Purpose::triage}, 1h);
    REQUIRE(t);
    CHECK(grant_events.load() == 1);
    CHECK(revoke_events.load() == 0);

    REQUIRE(r.remove(t.value().token_id));
    // Observer should NOT have fired for the remove — destructive ops
    // are intentionally invisible to the ledger mirror.
    CHECK(grant_events.load() == 1);
    CHECK(revoke_events.load() == 0);

    // And the registry now reports zero tokens.
    CHECK(r.total_count() == 0u);
    CHECK(!r.permits_any_purpose(p));
}

TEST_CASE("remove vs revoke: revoke keeps the row, remove drops it") {
    ConsentRegistry r;
    auto p  = PatientId::pseudonymous("rm_vs_revoke");
    auto t1 = r.grant(p, {Purpose::triage}, 1h);
    auto t2 = r.grant(p, {Purpose::operations}, 1h);
    REQUIRE(t1); REQUIRE(t2);

    REQUIRE(r.revoke(t1.value().token_id));
    REQUIRE(r.remove(t2.value().token_id));

    // Revoked token still resolvable; removed token is gone.
    auto got1 = r.get(t1.value().token_id);
    REQUIRE(got1);
    CHECK(got1.value().revoked);

    auto got2 = r.get(t2.value().token_id);
    CHECK(!got2);

    CHECK(r.token_count_for_patient(p) == 1u);
}

// ============== find_by_purpose =========================================

TEST_CASE("find_by_purpose: not_found when no tokens / wrong patient") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("fbp_empty");
    auto out = r.find_by_purpose(p, Purpose::triage);
    CHECK(!out);

    auto other = PatientId::pseudonymous("fbp_other");
    REQUIRE(r.grant(other, {Purpose::triage}, 1h));
    auto still = r.find_by_purpose(p, Purpose::triage);
    CHECK(!still);
}

TEST_CASE("find_by_purpose returns an active token granting the purpose") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("fbp_basic");
    auto t = r.grant(p, {Purpose::ambient_documentation, Purpose::triage}, 1h);
    REQUIRE(t);

    auto a = r.find_by_purpose(p, Purpose::triage);
    REQUIRE(a);
    CHECK(a.value().token_id == t.value().token_id);

    auto b = r.find_by_purpose(p, Purpose::ambient_documentation);
    REQUIRE(b);
    CHECK(b.value().token_id == t.value().token_id);

    // Purpose not granted → not_found.
    auto c = r.find_by_purpose(p, Purpose::medication_review);
    CHECK(!c);
}

TEST_CASE("find_by_purpose ignores revoked and expired tokens") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("fbp_states");

    // Revoked token granting research.
    auto rv = r.grant(p, {Purpose::research}, 1h);
    REQUIRE(rv);
    REQUIRE(r.revoke(rv.value().token_id));

    // Expired token granting research, ingested directly.
    ConsentToken exp;
    exp.token_id   = "ct_fbp_expired";
    exp.patient    = p;
    exp.purposes   = {Purpose::research};
    exp.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    exp.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(exp));

    // Neither active — should report not_found.
    auto miss = r.find_by_purpose(p, Purpose::research);
    CHECK(!miss);

    // Add an active grant; now found.
    auto live = r.grant(p, {Purpose::research, Purpose::triage}, 1h);
    REQUIRE(live);
    auto hit = r.find_by_purpose(p, Purpose::research);
    REQUIRE(hit);
    CHECK(hit.value().token_id == live.value().token_id);
    CHECK(!hit.value().revoked);
}

// ============== tokens_granted_within ===================================

TEST_CASE("tokens_granted_within: empty for horizon <= 0") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("tgw_zero");
    REQUIRE(r.grant(p, {Purpose::triage}, 1h));
    CHECK(r.tokens_granted_within(0s).empty());
    CHECK(r.tokens_granted_within(-5s).empty());
}

TEST_CASE("tokens_granted_within: includes both active and revoked recent grants") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("tgw_states");

    // Recent active grant.
    auto active = r.grant(p, {Purpose::triage}, 1h);
    REQUIRE(active);

    // Recent grant that we revoke — still counts as granted within.
    auto rv = r.grant(p, {Purpose::ambient_documentation}, 1h);
    REQUIRE(rv);
    REQUIRE(r.revoke(rv.value().token_id));

    auto out = r.tokens_granted_within(1h);
    CHECK(out.size() == 2u);

    // Old grant ingested with a stale issued_at — outside the window.
    ConsentToken old;
    old.token_id   = "ct_tgw_old";
    old.patient    = p;
    old.purposes   = {Purpose::research};
    old.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{5}};
    old.expires_at = Time::now() + std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(old));

    auto narrow = r.tokens_granted_within(1h);
    CHECK(narrow.size() == 2u);  // old grant excluded
    auto wide   = r.tokens_granted_within(std::chrono::seconds{std::chrono::hours{6}});
    CHECK(wide.size() == 3u);    // old grant included
}

TEST_CASE("tokens_granted_within: sorted by issued_at descending (newest first)") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("tgw_sort");

    // Three ingested tokens at distinct issued_at values, all within window.
    const auto now = Time::now();
    auto mk = [&](const char* id, std::chrono::minutes ago) {
        ConsentToken t;
        t.token_id   = id;
        t.patient    = p;
        t.purposes   = {Purpose::triage};
        t.issued_at  = now - std::chrono::nanoseconds{ago};
        t.expires_at = now + std::chrono::nanoseconds{std::chrono::hours{1}};
        return t;
    };
    REQUIRE(r.ingest(mk("ct_tgw_a", std::chrono::minutes{30})));
    REQUIRE(r.ingest(mk("ct_tgw_b", std::chrono::minutes{5})));
    REQUIRE(r.ingest(mk("ct_tgw_c", std::chrono::minutes{15})));

    auto out = r.tokens_granted_within(1h);
    REQUIRE(out.size() == 3u);
    // Newest first: b (5m), c (15m), a (30m).
    CHECK(out[0].token_id == "ct_tgw_b");
    CHECK(out[1].token_id == "ct_tgw_c");
    CHECK(out[2].token_id == "ct_tgw_a");
}

TEST_CASE("tokens_granted_within: empty registry returns empty vector") {
    ConsentRegistry r;
    CHECK(r.tokens_granted_within(1h).empty());
    CHECK(r.tokens_granted_within(24h).empty());
}

// ---- active_purpose_count_for_patient ------------------------------------

TEST_CASE("active_purpose_count_for_patient: zero for unknown patient and empty registry") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("apc_unknown");
    CHECK(r.active_purpose_count_for_patient(p) == 0u);

    auto other = PatientId::pseudonymous("apc_other");
    REQUIRE(r.grant(other, {Purpose::triage}, 1h));
    CHECK(r.active_purpose_count_for_patient(p) == 0u);
}

TEST_CASE("active_purpose_count_for_patient: counts distinct purposes across multiple tokens") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("apc_distinct");
    // Token 1: triage + ambient_documentation
    REQUIRE(r.grant(p, {Purpose::triage, Purpose::ambient_documentation}, 1h));
    // Token 2: ambient_documentation (overlap) + research
    REQUIRE(r.grant(p, {Purpose::ambient_documentation, Purpose::research}, 1h));
    // Token 3: triage again (overlap)
    REQUIRE(r.grant(p, {Purpose::triage}, 1h));

    // Distinct purposes: triage, ambient_documentation, research → 3.
    CHECK(r.active_purpose_count_for_patient(p) == 3u);
}

TEST_CASE("active_purpose_count_for_patient: ignores revoked / expired and matches active_purposes_for_patient size") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("apc_filter");

    auto live = r.grant(p, {Purpose::triage, Purpose::medication_review}, 1h);
    REQUIRE(live);

    auto rev = r.grant(p, {Purpose::research, Purpose::operations}, 1h);
    REQUIRE(rev);
    REQUIRE(r.revoke(rev.value().token_id));

    // Ingest an expired token with a purpose not present in the live one.
    ConsentToken expired;
    expired.token_id   = "ct_apc_expired";
    expired.patient    = p;
    expired.purposes   = {Purpose::quality_improvement};
    expired.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    expired.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(expired));

    // Only triage + medication_review should remain active for p.
    CHECK(r.active_purpose_count_for_patient(p) == 2u);
    CHECK(r.active_purpose_count_for_patient(p)
          == r.active_purposes_for_patient(p).size());
}

// ---- is_token_active -----------------------------------------------------

TEST_CASE("is_token_active: true for live token, false for unknown id") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("ita_basic");
    auto t = r.grant(p, {Purpose::triage}, 1h);
    REQUIRE(t);

    CHECK(r.is_token_active(t.value().token_id));
    CHECK_FALSE(r.is_token_active("ct_does_not_exist"));
    CHECK_FALSE(r.is_token_active(""));
}

TEST_CASE("is_token_active: false for revoked token, distinct from token_exists") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("ita_revoked");
    auto t = r.grant(p, {Purpose::triage}, 1h);
    REQUIRE(t);
    REQUIRE(r.revoke(t.value().token_id));

    CHECK_FALSE(r.is_token_active(t.value().token_id));
    // Row still present — token_exists() is the historical view.
    CHECK(r.token_exists(t.value().token_id));
}

TEST_CASE("is_token_active: false for expired token") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("ita_expired");
    ConsentToken expired;
    expired.token_id   = "ct_ita_expired";
    expired.patient    = p;
    expired.purposes   = {Purpose::triage};
    expired.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    expired.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(expired));

    CHECK_FALSE(r.is_token_active("ct_ita_expired"));
    CHECK(r.token_exists("ct_ita_expired"));
}

// ---- expire_purpose_for_patient ------------------------------------------

TEST_CASE("expire_purpose_for_patient: revokes only tokens whose purposes contain the target") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("epfp_basic");

    auto with_research    = r.grant(p, {Purpose::triage, Purpose::research}, 1h);
    auto without_research = r.grant(p, {Purpose::triage, Purpose::medication_review}, 1h);
    auto research_only    = r.grant(p, {Purpose::research}, 1h);
    REQUIRE(with_research);
    REQUIRE(without_research);
    REQUIRE(research_only);

    auto n = r.expire_purpose_for_patient(p, Purpose::research);
    REQUIRE(n);
    CHECK(n.value() == 2u);

    // Revoked: with_research, research_only. Untouched: without_research.
    auto a = r.get(with_research.value().token_id);
    REQUIRE(a);
    CHECK(a.value().revoked);

    auto b = r.get(research_only.value().token_id);
    REQUIRE(b);
    CHECK(b.value().revoked);

    auto c = r.get(without_research.value().token_id);
    REQUIRE(c);
    CHECK_FALSE(c.value().revoked);
}

TEST_CASE("expire_purpose_for_patient: scoped to patient and ignores revoked/expired tokens") {
    ConsentRegistry r;
    auto p1 = PatientId::pseudonymous("epfp_p1");
    auto p2 = PatientId::pseudonymous("epfp_p2");

    auto p1_research = r.grant(p1, {Purpose::research}, 1h);
    auto p2_research = r.grant(p2, {Purpose::research}, 1h);
    REQUIRE(p1_research);
    REQUIRE(p2_research);

    // Pre-revoked token for p1: should not be re-counted.
    auto p1_already = r.grant(p1, {Purpose::research}, 1h);
    REQUIRE(p1_already);
    REQUIRE(r.revoke(p1_already.value().token_id));

    auto n = r.expire_purpose_for_patient(p1, Purpose::research);
    REQUIRE(n);
    CHECK(n.value() == 1u);

    // p2's token untouched.
    auto p2_after = r.get(p2_research.value().token_id);
    REQUIRE(p2_after);
    CHECK_FALSE(p2_after.value().revoked);
}

TEST_CASE("expire_purpose_for_patient: fires observer once per token revoked") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("epfp_obs");

    std::atomic<std::size_t> revoked_events{0};
    std::atomic<std::size_t> granted_events{0};
    r.set_observer([&](ConsentRegistry::Event e, const ConsentToken&) {
        if (e == ConsentRegistry::Event::revoked) revoked_events++;
        else                                       granted_events++;
    });

    REQUIRE(r.grant(p, {Purpose::research, Purpose::triage}, 1h));
    REQUIRE(r.grant(p, {Purpose::research}, 1h));
    REQUIRE(r.grant(p, {Purpose::triage}, 1h));  // not affected

    granted_events = 0;  // reset to count only revokes from the next call

    auto n = r.expire_purpose_for_patient(p, Purpose::research);
    REQUIRE(n);
    CHECK(n.value() == 2u);
    CHECK(revoked_events.load() == 2u);
    CHECK(granted_events.load() == 0u);
}

TEST_CASE("expire_purpose_for_patient: zero when no matching tokens") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("epfp_zero");
    REQUIRE(r.grant(p, {Purpose::triage}, 1h));

    auto n = r.expire_purpose_for_patient(p, Purpose::research);
    REQUIRE(n);
    CHECK(n.value() == 0u);

    auto n2 = r.expire_purpose_for_patient(
        PatientId::pseudonymous("epfp_unknown"), Purpose::triage);
    REQUIRE(n2);
    CHECK(n2.value() == 0u);
}

// ---- longest_lived_active_for_patient ------------------------------------

TEST_CASE("longest_lived_active_for_patient: not_found when no active token") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("llafp_empty");

    auto miss = r.longest_lived_active_for_patient(p);
    CHECK_FALSE(miss);
    CHECK(miss.error().code() == ErrorCode::not_found);

    // Revoked-only patient still gets not_found.
    auto t = r.grant(p, {Purpose::triage}, 1h);
    REQUIRE(t);
    REQUIRE(r.revoke(t.value().token_id));
    auto still_miss = r.longest_lived_active_for_patient(p);
    CHECK_FALSE(still_miss);
    CHECK(still_miss.error().code() == ErrorCode::not_found);
}

TEST_CASE("longest_lived_active_for_patient: returns the token with the latest expires_at") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("llafp_pick");

    auto short_t  = r.grant(p, {Purpose::triage}, 1h);
    auto medium_t = r.grant(p, {Purpose::triage}, 6h);
    auto long_t   = r.grant(p, {Purpose::triage}, 24h);
    REQUIRE(short_t);
    REQUIRE(medium_t);
    REQUIRE(long_t);

    auto best = r.longest_lived_active_for_patient(p);
    REQUIRE(best);
    CHECK(best.value().token_id == long_t.value().token_id);
}

TEST_CASE("longest_lived_active_for_patient: scoped to patient — distinct from longest_active()") {
    ConsentRegistry r;
    auto p1 = PatientId::pseudonymous("llafp_p1");
    auto p2 = PatientId::pseudonymous("llafp_p2");

    auto p1_short = r.grant(p1, {Purpose::triage}, 1h);
    auto p2_long  = r.grant(p2, {Purpose::triage}, 24h);
    REQUIRE(p1_short);
    REQUIRE(p2_long);

    auto global_best = r.longest_active();
    REQUIRE(global_best);
    CHECK(global_best.value().token_id == p2_long.value().token_id);

    auto p1_best = r.longest_lived_active_for_patient(p1);
    REQUIRE(p1_best);
    CHECK(p1_best.value().token_id == p1_short.value().token_id);
}

TEST_CASE("longest_lived_active_for_patient: skips revoked and expired tokens for the patient") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("llafp_filter");

    // Active short-lived token.
    auto live = r.grant(p, {Purpose::triage}, 1h);
    REQUIRE(live);

    // Revoked token with would-be-longest expiry.
    auto rev = r.grant(p, {Purpose::triage}, 24h);
    REQUIRE(rev);
    REQUIRE(r.revoke(rev.value().token_id));

    // Ingest an expired token with a far-future-but-already-passed
    // expires_at (it's "expired" because expires_at <= now). We just
    // make it expired at time of grant.
    ConsentToken expired;
    expired.token_id   = "ct_llafp_expired";
    expired.patient    = p;
    expired.purposes   = {Purpose::triage};
    expired.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    expired.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(expired));

    auto best = r.longest_lived_active_for_patient(p);
    REQUIRE(best);
    CHECK(best.value().token_id == live.value().token_id);
}

// ---- tokens_count_by_purpose -------------------------------------------

TEST_CASE("tokens_count_by_purpose: counts each purpose once per active token") {
    ConsentRegistry r;
    auto p1 = PatientId::pseudonymous("tcbp_a");
    auto p2 = PatientId::pseudonymous("tcbp_b");
    REQUIRE(r.grant(p1, {Purpose::triage, Purpose::medication_review}, 1h));
    REQUIRE(r.grant(p2, {Purpose::triage}, 1h));
    REQUIRE(r.grant(p2, {Purpose::research}, 1h));

    auto m = r.tokens_count_by_purpose();
    CHECK(m[Purpose::triage]            == 2u);
    CHECK(m[Purpose::medication_review] == 1u);
    CHECK(m[Purpose::research]          == 1u);
    // Purposes with no active tokens do not appear in the map.
    CHECK(m.find(Purpose::operations) == m.end());
}

TEST_CASE("tokens_count_by_purpose: skips revoked and expired tokens") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("tcbp_filter");
    auto live = r.grant(p, {Purpose::triage}, 1h);
    auto rev  = r.grant(p, {Purpose::triage, Purpose::research}, 1h);
    REQUIRE(live);
    REQUIRE(rev);
    REQUIRE(r.revoke(rev.value().token_id));

    // An expired token granting a third purpose.
    ConsentToken expired;
    expired.token_id   = "ct_tcbp_expired";
    expired.patient    = p;
    expired.purposes   = {Purpose::operations};
    expired.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    expired.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(expired));

    auto m = r.tokens_count_by_purpose();
    CHECK(m[Purpose::triage] == 1u);
    CHECK(m.find(Purpose::research)   == m.end());
    CHECK(m.find(Purpose::operations) == m.end());
}

TEST_CASE("tokens_count_by_purpose: empty registry returns empty map") {
    ConsentRegistry r;
    auto m = r.tokens_count_by_purpose();
    CHECK(m.empty());
}

TEST_CASE("tokens_count_by_purpose: token with duplicated purpose still counts once") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("tcbp_dup");
    // ingest a malformed token with a duplicated purpose.
    ConsentToken t;
    t.token_id   = "ct_tcbp_dup";
    t.patient    = p;
    t.purposes   = {Purpose::triage, Purpose::triage};
    t.issued_at  = Time::now();
    t.expires_at = Time::now() + std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(t));

    auto m = r.tokens_count_by_purpose();
    CHECK(m[Purpose::triage] == 1u);
}

// ---- patient_with_most_tokens ------------------------------------------

TEST_CASE("patient_with_most_tokens: returns the heaviest patient") {
    ConsentRegistry r;
    auto heavy = PatientId::pseudonymous("pwmt_heavy");
    auto light = PatientId::pseudonymous("pwmt_light");
    REQUIRE(r.grant(heavy, {Purpose::triage}, 1h));
    REQUIRE(r.grant(heavy, {Purpose::research}, 1h));
    REQUIRE(r.grant(heavy, {Purpose::operations}, 1h));
    REQUIRE(r.grant(light, {Purpose::triage}, 1h));

    auto top = r.patient_with_most_tokens();
    REQUIRE(top);
    CHECK(top.value() == heavy);
}

TEST_CASE("patient_with_most_tokens: counts active + revoked + expired alike") {
    ConsentRegistry r;
    auto a = PatientId::pseudonymous("pwmt_a");
    auto b = PatientId::pseudonymous("pwmt_b");

    // a has 1 active token.
    REQUIRE(r.grant(a, {Purpose::triage}, 1h));
    // b has 1 active + 1 revoked + 1 expired = 3.
    auto b_active = r.grant(b, {Purpose::triage}, 1h);
    auto b_revoke = r.grant(b, {Purpose::triage}, 1h);
    REQUIRE(b_active);
    REQUIRE(b_revoke);
    REQUIRE(r.revoke(b_revoke.value().token_id));

    ConsentToken expired;
    expired.token_id   = "ct_pwmt_b_expired";
    expired.patient    = b;
    expired.purposes   = {Purpose::triage};
    expired.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    expired.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(expired));

    auto top = r.patient_with_most_tokens();
    REQUIRE(top);
    CHECK(top.value() == b);
}

TEST_CASE("patient_with_most_tokens: empty registry returns not_found") {
    ConsentRegistry r;
    auto top = r.patient_with_most_tokens();
    REQUIRE(!top);
    CHECK(top.error().code() == ErrorCode::not_found);
}

TEST_CASE("patient_with_most_tokens: single patient is trivially the winner") {
    ConsentRegistry r;
    auto only = PatientId::pseudonymous("pwmt_solo");
    REQUIRE(r.grant(only, {Purpose::triage}, 1h));
    auto top = r.patient_with_most_tokens();
    REQUIRE(top);
    CHECK(top.value() == only);
}

// ---- serialize_to_json --------------------------------------------------

TEST_CASE("serialize_to_json: empty registry yields empty array") {
    ConsentRegistry r;
    auto s = r.serialize_to_json();
    auto j = nlohmann::json::parse(s);
    REQUIRE(j.is_array());
    CHECK(j.empty());
}

TEST_CASE("serialize_to_json: emits all expected fields per token") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("ser_one");
    auto t = r.grant(p, {Purpose::triage, Purpose::research}, 2h);
    REQUIRE(t);

    auto s = r.serialize_to_json();
    auto j = nlohmann::json::parse(s);
    REQUIRE(j.is_array());
    REQUIRE(j.size() == 1u);

    auto& o = j[0];
    CHECK(o["token_id"].get<std::string>() == t.value().token_id);
    CHECK(o["patient"].get<std::string>()  == std::string{p.str()});
    REQUIRE(o["purposes"].is_array());
    CHECK(o["purposes"].size() == 2u);
    CHECK(o["revoked"].get<bool>() == false);
    CHECK(o["issued_at"].is_string());
    CHECK(o["expires_at"].is_string());
}

TEST_CASE("serialize_to_json: includes revoked tokens with revoked=true") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("ser_rev");
    auto t = r.grant(p, {Purpose::operations}, 1h);
    REQUIRE(t);
    REQUIRE(r.revoke(t.value().token_id));

    auto j = nlohmann::json::parse(r.serialize_to_json());
    REQUIRE(j.is_array());
    REQUIRE(j.size() == 1u);
    CHECK(j[0]["revoked"].get<bool>() == true);
    CHECK(j[0]["token_id"].get<std::string>() == t.value().token_id);
}

TEST_CASE("serialize_to_json: round-trips multiple tokens through deserialize_from_json") {
    ConsentRegistry r;
    auto p1 = PatientId::pseudonymous("ser_rt_1");
    auto p2 = PatientId::pseudonymous("ser_rt_2");
    REQUIRE(r.grant(p1, {Purpose::triage}, 1h));
    REQUIRE(r.grant(p2, {Purpose::research, Purpose::medication_review}, 3h));

    auto blob = r.serialize_to_json();

    ConsentRegistry r2;
    auto n = r2.deserialize_from_json(blob);
    REQUIRE(n);
    CHECK(n.value() == 2u);
    CHECK(r2.total_count() == 2u);
}

// ---- deserialize_from_json ---------------------------------------------

TEST_CASE("deserialize_from_json: appends without clearing existing state") {
    ConsentRegistry r;
    auto p_existing = PatientId::pseudonymous("deser_existing");
    REQUIRE(r.grant(p_existing, {Purpose::triage}, 1h));
    REQUIRE(r.total_count() == 1u);

    ConsentRegistry src;
    auto p_new = PatientId::pseudonymous("deser_new");
    REQUIRE(src.grant(p_new, {Purpose::operations}, 1h));
    auto blob = src.serialize_to_json();

    auto n = r.deserialize_from_json(blob);
    REQUIRE(n);
    CHECK(n.value() == 1u);
    CHECK(r.total_count() == 2u);
}

TEST_CASE("deserialize_from_json: skips tokens already present without erroring") {
    ConsentRegistry src;
    auto p = PatientId::pseudonymous("deser_dup");
    REQUIRE(src.grant(p, {Purpose::triage}, 1h));
    REQUIRE(src.grant(p, {Purpose::research}, 1h));
    auto blob = src.serialize_to_json();

    ConsentRegistry dst;
    // First ingest: both land.
    auto n1 = dst.deserialize_from_json(blob);
    REQUIRE(n1);
    CHECK(n1.value() == 2u);
    CHECK(dst.total_count() == 2u);

    // Second ingest of the same blob: all collide, nothing new ingested,
    // total unchanged, no error.
    auto n2 = dst.deserialize_from_json(blob);
    REQUIRE(n2);
    CHECK(n2.value() == 0u);
    CHECK(dst.total_count() == 2u);
}

TEST_CASE("deserialize_from_json: rejects malformed JSON") {
    ConsentRegistry r;
    auto bad = r.deserialize_from_json("{not valid json");
    REQUIRE(!bad);
    CHECK(bad.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("deserialize_from_json: does NOT fire the observer") {
    ConsentRegistry src;
    auto p = PatientId::pseudonymous("deser_obs");
    REQUIRE(src.grant(p, {Purpose::triage}, 1h));
    auto blob = src.serialize_to_json();

    ConsentRegistry dst;
    std::atomic<int> calls{0};
    dst.set_observer([&](ConsentRegistry::Event, const ConsentToken&) {
        calls.fetch_add(1);
    });

    auto n = dst.deserialize_from_json(blob);
    REQUIRE(n);
    CHECK(n.value() == 1u);
    CHECK(calls.load() == 0);
}

TEST_CASE("deserialize_from_json: accepts the {tokens: [...]} envelope from dump_state_json") {
    ConsentRegistry src;
    auto p = PatientId::pseudonymous("deser_env");
    REQUIRE(src.grant(p, {Purpose::quality_improvement}, 1h));
    auto blob = src.dump_state_json();

    ConsentRegistry dst;
    auto n = dst.deserialize_from_json(blob);
    REQUIRE(n);
    CHECK(n.value() == 1u);
    CHECK(dst.total_count() == 1u);
}

// ---- token_age ----------------------------------------------------------

TEST_CASE("token_age: returns a non-negative duration for a freshly granted token") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("age_fresh");
    auto t = r.grant(p, {Purpose::triage}, 1h);
    REQUIRE(t);

    auto age = r.token_age(t.value().token_id);
    REQUIRE(age);
    CHECK(age.value().count() >= 0);
    // A freshly issued token should be much younger than its TTL.
    CHECK(age.value() < std::chrono::seconds{60});
}

TEST_CASE("token_age: returns not_found for unknown token id") {
    ConsentRegistry r;
    auto a = r.token_age("ct_does_not_exist");
    REQUIRE(!a);
    CHECK(a.error().code() == ErrorCode::not_found);

    auto b = r.token_age("");
    REQUIRE(!b);
    CHECK(b.error().code() == ErrorCode::not_found);
}

TEST_CASE("token_age: still resolves for a revoked token (revoke does not erase)") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("age_rev");
    auto t = r.grant(p, {Purpose::operations}, 1h);
    REQUIRE(t);
    REQUIRE(r.revoke(t.value().token_id));

    auto age = r.token_age(t.value().token_id);
    REQUIRE(age);
    CHECK(age.value().count() >= 0);
}

TEST_CASE("token_age: monotonic - older tokens age further than newer ones") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("age_mono");
    auto t1 = r.grant(p, {Purpose::triage}, 1h);
    REQUIRE(t1);
    std::this_thread::sleep_for(std::chrono::milliseconds{2});
    auto t2 = r.grant(p, {Purpose::triage}, 1h);
    REQUIRE(t2);

    auto a1 = r.token_age(t1.value().token_id);
    auto a2 = r.token_age(t2.value().token_id);
    REQUIRE(a1); REQUIRE(a2);
    CHECK(a1.value() >= a2.value());
}

// ---- distinct_patients_count -------------------------------------------

TEST_CASE("distinct_patients_count: empty registry returns 0") {
    ConsentRegistry r;
    CHECK(r.distinct_patients_count() == 0u);
}

TEST_CASE("distinct_patients_count: counts unique patients across multiple tokens") {
    ConsentRegistry r;
    auto a = PatientId::pseudonymous("dp_a");
    auto b = PatientId::pseudonymous("dp_b");
    auto c = PatientId::pseudonymous("dp_c");
    REQUIRE(r.grant(a, {Purpose::triage}, 1h));
    REQUIRE(r.grant(a, {Purpose::research}, 1h));   // same patient, second token
    REQUIRE(r.grant(b, {Purpose::operations}, 1h));
    REQUIRE(r.grant(c, {Purpose::medication_review}, 1h));
    CHECK(r.distinct_patients_count() == 3u);
}

TEST_CASE("distinct_patients_count: includes revoked and expired tokens") {
    ConsentRegistry r;
    auto a = PatientId::pseudonymous("dp_rev_a");
    auto b = PatientId::pseudonymous("dp_rev_b");
    auto t = r.grant(a, {Purpose::triage}, 1h);
    REQUIRE(t);
    REQUIRE(r.revoke(t.value().token_id));
    REQUIRE(r.grant(b, {Purpose::operations}, 1h));
    // Even though `a`'s only token is revoked, distinct_patients_count
    // still counts patients with any token on file.
    CHECK(r.distinct_patients_count() == 2u);
}

TEST_CASE("distinct_patients_count: agrees with patient_count synonym") {
    ConsentRegistry r;
    auto a = PatientId::pseudonymous("dp_syn_a");
    auto b = PatientId::pseudonymous("dp_syn_b");
    REQUIRE(r.grant(a, {Purpose::triage}, 1h));
    REQUIRE(r.grant(b, {Purpose::triage}, 1h));
    REQUIRE(r.grant(a, {Purpose::operations}, 1h));
    CHECK(r.distinct_patients_count() == r.patient_count());
}

// ---- find_token_granting_all -------------------------------------------

TEST_CASE("find_token_granting_all: finds a token covering all requested purposes") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("ga_all");
    auto t = r.grant(p, {Purpose::triage, Purpose::medication_review,
                         Purpose::operations}, 1h);
    REQUIRE(t);

    std::vector<Purpose> wanted = {Purpose::triage, Purpose::medication_review};
    auto hit = r.find_token_granting_all(p, std::span<const Purpose>{wanted});
    REQUIRE(hit);
    CHECK(hit.value().token_id == t.value().token_id);
}

TEST_CASE("find_token_granting_all: returns not_found if no single token covers all") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("ga_split");
    // Two tokens that, taken together, cover triage+research, but
    // neither one alone does.
    REQUIRE(r.grant(p, {Purpose::triage},   1h));
    REQUIRE(r.grant(p, {Purpose::research}, 1h));

    std::vector<Purpose> wanted = {Purpose::triage, Purpose::research};
    auto hit = r.find_token_granting_all(p, std::span<const Purpose>{wanted});
    REQUIRE(!hit);
    CHECK(hit.error().code() == ErrorCode::not_found);
}

TEST_CASE("find_token_granting_all: empty purposes -> invalid_argument") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("ga_empty");
    REQUIRE(r.grant(p, {Purpose::triage}, 1h));
    std::vector<Purpose> empty;
    auto hit = r.find_token_granting_all(p, std::span<const Purpose>{empty});
    REQUIRE(!hit);
    CHECK(hit.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("find_token_granting_all: ignores revoked tokens even if they cover everything") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("ga_rev");
    auto t = r.grant(p, {Purpose::triage, Purpose::operations}, 1h);
    REQUIRE(t);
    REQUIRE(r.revoke(t.value().token_id));

    std::vector<Purpose> wanted = {Purpose::triage, Purpose::operations};
    auto hit = r.find_token_granting_all(p, std::span<const Purpose>{wanted});
    REQUIRE(!hit);
    CHECK(hit.error().code() == ErrorCode::not_found);
}

TEST_CASE("find_token_granting_all: per-patient scoping") {
    ConsentRegistry r;
    auto alice = PatientId::pseudonymous("ga_alice");
    auto bob   = PatientId::pseudonymous("ga_bob");
    REQUIRE(r.grant(alice, {Purpose::triage, Purpose::operations}, 1h));

    std::vector<Purpose> wanted = {Purpose::triage, Purpose::operations};
    // alice has it, bob does not.
    auto a_hit = r.find_token_granting_all(alice, std::span<const Purpose>{wanted});
    REQUIRE(a_hit);
    auto b_hit = r.find_token_granting_all(bob,   std::span<const Purpose>{wanted});
    REQUIRE(!b_hit);
    CHECK(b_hit.error().code() == ErrorCode::not_found);
}

// ---- TokenLifecycle / token_lifecycle ----------------------------------

TEST_CASE("token_lifecycle: active token reports State::active") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("lc_active");
    auto t = r.grant(p, {Purpose::triage}, 1h);
    REQUIRE(t);

    auto lc = r.token_lifecycle(t.value().token_id);
    REQUIRE(lc);
    CHECK(lc.value().token_id == t.value().token_id);
    CHECK(lc.value().issued_at  == t.value().issued_at);
    CHECK(lc.value().expires_at == t.value().expires_at);
    CHECK(lc.value().revoked == false);
    CHECK(lc.value().state ==
          ConsentRegistry::TokenLifecycle::State::active);
}

TEST_CASE("token_lifecycle: revoked token reports State::revoked") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("lc_revoked");
    auto t = r.grant(p, {Purpose::operations}, 1h);
    REQUIRE(t);
    REQUIRE(r.revoke(t.value().token_id));

    auto lc = r.token_lifecycle(t.value().token_id);
    REQUIRE(lc);
    CHECK(lc.value().revoked == true);
    CHECK(lc.value().state ==
          ConsentRegistry::TokenLifecycle::State::revoked);
}

TEST_CASE("token_lifecycle: returns not_found for unknown token id") {
    ConsentRegistry r;
    auto lc = r.token_lifecycle("ct_no_such_token");
    REQUIRE(!lc);
    CHECK(lc.error().code() == ErrorCode::not_found);
}

TEST_CASE("token_lifecycle: revoked-and-expired prefers revoked over expired") {
    // Use ingest() to fabricate a token whose expires_at is in the past
    // AND whose revoked flag is true; the priority rule says revoked
    // wins.
    ConsentRegistry r;
    ConsentToken fab;
    fab.token_id   = "ct_lc_priority";
    fab.patient    = PatientId::pseudonymous("lc_prio");
    fab.purposes   = {Purpose::triage};
    fab.issued_at  = Time{0};
    fab.expires_at = Time{1};       // long expired
    fab.revoked    = true;
    REQUIRE(r.ingest(fab));

    auto lc = r.token_lifecycle("ct_lc_priority");
    REQUIRE(lc);
    CHECK(lc.value().state ==
          ConsentRegistry::TokenLifecycle::State::revoked);
}

TEST_CASE("to_string(TokenLifecycle::State) covers every enumerator") {
    using S = ConsentRegistry::TokenLifecycle::State;
    CHECK(std::string{to_string(S::active)}  == "active");
    CHECK(std::string{to_string(S::revoked)} == "revoked");
    CHECK(std::string{to_string(S::expired)} == "expired");
}

// ---- has_active_token --------------------------------------------------

TEST_CASE("has_active_token: true after a fresh grant") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("hat_fresh");
    REQUIRE(r.grant(p, {Purpose::triage}, 1h));
    CHECK(r.has_active_token(p) == true);
}

TEST_CASE("has_active_token: false for a patient we have never seen") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("hat_unknown");
    CHECK(r.has_active_token(p) == false);
}

TEST_CASE("has_active_token: false after the only token is revoked") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("hat_rev");
    auto t = r.grant(p, {Purpose::operations}, 1h);
    REQUIRE(t);
    CHECK(r.has_active_token(p) == true);
    REQUIRE(r.revoke(t.value().token_id));
    CHECK(r.has_active_token(p) == false);
}

TEST_CASE("has_active_token: still true if at least one of several tokens is active") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("hat_partial");
    auto t1 = r.grant(p, {Purpose::triage}, 1h);
    auto t2 = r.grant(p, {Purpose::operations}, 1h);
    REQUIRE(t1); REQUIRE(t2);
    REQUIRE(r.revoke(t1.value().token_id));
    CHECK(r.has_active_token(p) == true);
    REQUIRE(r.revoke(t2.value().token_id));
    CHECK(r.has_active_token(p) == false);
}

TEST_CASE("has_active_token: per-patient scoped") {
    ConsentRegistry r;
    auto alice = PatientId::pseudonymous("hat_alice");
    auto bob   = PatientId::pseudonymous("hat_bob");
    REQUIRE(r.grant(alice, {Purpose::triage}, 1h));
    CHECK(r.has_active_token(alice) == true);
    CHECK(r.has_active_token(bob)   == false);
}

// ---- most_recently_revoked_for_patient ---------------------------------

TEST_CASE("most_recently_revoked_for_patient: empty registry -> not_found") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("mrrp_empty");
    auto got = r.most_recently_revoked_for_patient(p);
    REQUIRE(!got);
    CHECK(got.error().code() == ErrorCode::not_found);
}

TEST_CASE("most_recently_revoked_for_patient: ignores active tokens") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("mrrp_active_only");
    REQUIRE(r.grant(p, {Purpose::triage}, 1h));
    REQUIRE(r.grant(p, {Purpose::operations}, 1h));
    // No revokes have happened: should report not_found.
    auto got = r.most_recently_revoked_for_patient(p);
    REQUIRE(!got);
    CHECK(got.error().code() == ErrorCode::not_found);
}

TEST_CASE("most_recently_revoked_for_patient: picks largest issued_at among revoked") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("mrrp_pick");

    // Fabricate three revoked tokens with explicit issued_at values via
    // ingest, so the "largest issued_at" choice is unambiguous.
    ConsentToken older;
    older.token_id   = "ct_mrrp_older";
    older.patient    = p;
    older.purposes   = {Purpose::triage};
    older.issued_at  = Time{1'000};
    older.expires_at = Time{2'000} +
        std::chrono::nanoseconds{std::chrono::hours{10}};
    older.revoked    = true;
    REQUIRE(r.ingest(older));

    ConsentToken middle = older;
    middle.token_id  = "ct_mrrp_middle";
    middle.issued_at = Time{2'000};
    REQUIRE(r.ingest(middle));

    ConsentToken newest = older;
    newest.token_id  = "ct_mrrp_newest";
    newest.issued_at = Time{3'000};
    REQUIRE(r.ingest(newest));

    auto got = r.most_recently_revoked_for_patient(p);
    REQUIRE(got);
    CHECK(got.value().token_id == "ct_mrrp_newest");
}

TEST_CASE("most_recently_revoked_for_patient: per-patient scoping") {
    ConsentRegistry r;
    auto alice = PatientId::pseudonymous("mrrp_alice");
    auto bob   = PatientId::pseudonymous("mrrp_bob");
    auto t_a = r.grant(alice, {Purpose::triage}, 1h);
    auto t_b = r.grant(bob,   {Purpose::operations}, 1h);
    REQUIRE(t_a); REQUIRE(t_b);
    REQUIRE(r.revoke(t_a.value().token_id));

    // alice has a revoked token; bob does not.
    auto a_got = r.most_recently_revoked_for_patient(alice);
    REQUIRE(a_got);
    CHECK(a_got.value().token_id == t_a.value().token_id);

    auto b_got = r.most_recently_revoked_for_patient(bob);
    REQUIRE(!b_got);
    CHECK(b_got.error().code() == ErrorCode::not_found);
}

// ---- age_of_oldest_active ----------------------------------------------

TEST_CASE("age_of_oldest_active: empty registry -> not_found") {
    ConsentRegistry r;
    auto got = r.age_of_oldest_active();
    REQUIRE(!got);
    CHECK(got.error().code() == ErrorCode::not_found);
}

TEST_CASE("age_of_oldest_active: only revoked / only expired -> not_found") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("aoa_no_active");

    // Revoked token.
    auto t_rev = r.grant(p, {Purpose::triage}, 1h);
    REQUIRE(t_rev);
    REQUIRE(r.revoke(t_rev.value().token_id));

    // Expired token via ingest with past expires_at.
    ConsentToken exp;
    exp.token_id   = "ct_aoa_expired";
    exp.patient    = p;
    exp.purposes   = {Purpose::operations};
    exp.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    exp.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    exp.revoked    = false;
    REQUIRE(r.ingest(exp));

    auto got = r.age_of_oldest_active();
    REQUIRE(!got);
    CHECK(got.error().code() == ErrorCode::not_found);
}

TEST_CASE("age_of_oldest_active: returns positive duration for a fresh grant") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("aoa_fresh");
    REQUIRE(r.grant(p, {Purpose::triage}, 1h));
    auto got = r.age_of_oldest_active();
    REQUIRE(got);
    // Now() advances past issued_at; age should be non-negative.
    CHECK(got.value().count() >= 0);
}

TEST_CASE("age_of_oldest_active: tracks the earliest issued_at across many tokens") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("aoa_pick");

    const auto now = Time::now();
    const auto far_future = now + std::chrono::nanoseconds{std::chrono::hours{10}};

    // Oldest active token: issued well in the past.
    ConsentToken oldest;
    oldest.token_id   = "ct_aoa_oldest";
    oldest.patient    = p;
    oldest.purposes   = {Purpose::triage};
    oldest.issued_at  = now - std::chrono::nanoseconds{std::chrono::hours{5}};
    oldest.expires_at = far_future;
    oldest.revoked    = false;
    REQUIRE(r.ingest(oldest));

    ConsentToken younger = oldest;
    younger.token_id  = "ct_aoa_younger";
    younger.issued_at = now - std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(younger));

    // A revoked token that is even older — must NOT be picked.
    ConsentToken ancient_revoked = oldest;
    ancient_revoked.token_id  = "ct_aoa_ancient_revoked";
    ancient_revoked.issued_at = now - std::chrono::nanoseconds{std::chrono::hours{100}};
    ancient_revoked.revoked   = true;
    REQUIRE(r.ingest(ancient_revoked));

    auto got = r.age_of_oldest_active();
    REQUIRE(got);
    // Age should be at least 5 hours (i.e. >= the oldest active issued_at gap).
    CHECK(got.value() >=
          std::chrono::nanoseconds{std::chrono::hours{5}});
    // And not as large as the revoked-100h gap.
    CHECK(got.value() <
          std::chrono::nanoseconds{std::chrono::hours{100}});
}

// ---- token_count_by_patient --------------------------------------------

TEST_CASE("token_count_by_patient: empty registry -> empty map") {
    ConsentRegistry r;
    auto m = r.token_count_by_patient();
    CHECK(m.empty());
}

TEST_CASE("token_count_by_patient: counts all states (active + revoked + expired)") {
    ConsentRegistry r;
    auto alice = PatientId::pseudonymous("tcbp_alice");
    auto bob   = PatientId::pseudonymous("tcbp_bob");

    // Two active for alice.
    REQUIRE(r.grant(alice, {Purpose::triage}, 1h));
    REQUIRE(r.grant(alice, {Purpose::operations}, 1h));

    // One active + one revoked for bob.
    REQUIRE(r.grant(bob, {Purpose::triage}, 1h));
    auto br = r.grant(bob, {Purpose::operations}, 1h);
    REQUIRE(br);
    REQUIRE(r.revoke(br.value().token_id));

    // One expired for bob via ingest.
    ConsentToken exp;
    exp.token_id   = "ct_tcbp_exp";
    exp.patient    = bob;
    exp.purposes   = {Purpose::triage};
    exp.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    exp.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(exp));

    auto m = r.token_count_by_patient();
    CHECK(m.size() == 2);
    CHECK(m[std::string{alice.str()}] == 2);
    CHECK(m[std::string{bob.str()}]   == 3);
}

TEST_CASE("token_count_by_patient: keys are raw patient.str() strings") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("tcbp_key");
    REQUIRE(r.grant(p, {Purpose::triage}, 1h));
    auto m = r.token_count_by_patient();
    // pseudonymous() prefixes with "pat:", so the key must reflect that
    // raw form rather than just the unprefixed token.
    CHECK(m.count(std::string{p.str()}) == 1);
    CHECK(m.count("tcbp_key") == 0);
}

TEST_CASE("token_count_by_patient: matches token_count_for_patient per key") {
    ConsentRegistry r;
    auto a = PatientId::pseudonymous("tcbp_match_a");
    auto b = PatientId::pseudonymous("tcbp_match_b");
    REQUIRE(r.grant(a, {Purpose::triage}, 1h));
    REQUIRE(r.grant(a, {Purpose::operations}, 1h));
    REQUIRE(r.grant(a, {Purpose::medication_review}, 1h));
    REQUIRE(r.grant(b, {Purpose::triage}, 1h));

    auto m = r.token_count_by_patient();
    CHECK(m[std::string{a.str()}] == r.token_count_for_patient(a));
    CHECK(m[std::string{b.str()}] == r.token_count_for_patient(b));
}

// ---- tokens_expiring_soon ----------------------------------------------

TEST_CASE("tokens_expiring_soon: zero / negative horizon -> 0") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("tes_horiz");
    REQUIRE(r.grant(p, {Purpose::triage}, 1h));
    CHECK(r.tokens_expiring_soon(std::chrono::seconds{0})  == 0);
    CHECK(r.tokens_expiring_soon(std::chrono::seconds{-1}) == 0);
}

TEST_CASE("tokens_expiring_soon: counts only active tokens within horizon") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("tes_window");

    // Active token with a long TTL — should NOT be inside a short window.
    REQUIRE(r.grant(p, {Purpose::triage}, std::chrono::hours{10}));

    // Active token with a short TTL — SHOULD be inside the window.
    REQUIRE(r.grant(p, {Purpose::operations}, std::chrono::seconds{30}));

    // 1-minute horizon: only the 30s grant counts.
    CHECK(r.tokens_expiring_soon(std::chrono::seconds{60}) == 1);
    // 100-hour horizon: both count.
    CHECK(r.tokens_expiring_soon(std::chrono::hours{100}) == 2);
}

TEST_CASE("tokens_expiring_soon: excludes already-expired and revoked tokens") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("tes_exclude");

    // Revoked token, otherwise within horizon.
    auto t = r.grant(p, {Purpose::triage}, std::chrono::seconds{30});
    REQUIRE(t);
    REQUIRE(r.revoke(t.value().token_id));

    // Already-expired token via ingest.
    ConsentToken exp;
    exp.token_id   = "ct_tes_expired";
    exp.patient    = p;
    exp.purposes   = {Purpose::operations};
    exp.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    exp.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(exp));

    // Real active grant inside horizon.
    REQUIRE(r.grant(p, {Purpose::operations}, std::chrono::seconds{30}));

    CHECK(r.tokens_expiring_soon(std::chrono::seconds{60}) == 1);
}

TEST_CASE("tokens_expiring_soon: empty registry -> 0") {
    ConsentRegistry r;
    CHECK(r.tokens_expiring_soon(std::chrono::hours{1}) == 0);
}

// ---- has_revoked_tokens ------------------------------------------------

TEST_CASE("has_revoked_tokens: empty registry -> false") {
    ConsentRegistry r;
    CHECK(r.has_revoked_tokens() == false);
}

TEST_CASE("has_revoked_tokens: only active tokens -> false") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("hrt_active_only");
    REQUIRE(r.grant(p, {Purpose::triage}, 1h));
    REQUIRE(r.grant(p, {Purpose::operations}, 1h));
    CHECK(r.has_revoked_tokens() == false);
}

TEST_CASE("has_revoked_tokens: true after a single revoke") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("hrt_one_revoke");
    auto t = r.grant(p, {Purpose::triage}, 1h);
    REQUIRE(t);
    CHECK(r.has_revoked_tokens() == false);
    REQUIRE(r.revoke(t.value().token_id));
    CHECK(r.has_revoked_tokens() == true);
}

TEST_CASE("has_revoked_tokens: true after ingesting a revoked token") {
    ConsentRegistry r;
    ConsentToken t;
    t.token_id   = "ct_hrt_ingest";
    t.patient    = PatientId::pseudonymous("hrt_ingest");
    t.purposes   = {Purpose::triage};
    t.issued_at  = Time::now();
    t.expires_at = t.issued_at + std::chrono::nanoseconds{std::chrono::hours{1}};
    t.revoked    = true;
    REQUIRE(r.ingest(t));
    CHECK(r.has_revoked_tokens() == true);
}

TEST_CASE("has_revoked_tokens: stays true even after the revoked token is removed") {
    ConsentRegistry r;
    auto p1 = PatientId::pseudonymous("hrt_persist_1");
    auto p2 = PatientId::pseudonymous("hrt_persist_2");
    auto t1 = r.grant(p1, {Purpose::triage}, 1h);
    auto t2 = r.grant(p2, {Purpose::operations}, 1h);
    REQUIRE(t1); REQUIRE(t2);
    REQUIRE(r.revoke(t1.value().token_id));
    REQUIRE(r.revoke(t2.value().token_id));
    CHECK(r.has_revoked_tokens() == true);
    REQUIRE(r.remove(t1.value().token_id));
    // Still has t2 revoked.
    CHECK(r.has_revoked_tokens() == true);
    REQUIRE(r.remove(t2.value().token_id));
    // Now empty: no revoked tokens left.
    CHECK(r.has_revoked_tokens() == false);
}

// ---- find_oldest_token_for_patient -------------------------------------

TEST_CASE("find_oldest_token_for_patient: returns smallest issued_at across all states") {
    ConsentRegistry r;
    auto pid = PatientId::pseudonymous("p_oldest_any");
    auto base = Time::now();

    ConsentToken t1;
    t1.token_id   = "ct_oldest_1";
    t1.patient    = pid;
    t1.purposes   = {Purpose::triage};
    t1.issued_at  = base - std::chrono::nanoseconds{std::chrono::minutes{30}};
    t1.expires_at = base + std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(t1));

    // Older than t1, but revoked — must still qualify.
    ConsentToken t2 = t1;
    t2.token_id  = "ct_oldest_2";
    t2.issued_at = base - std::chrono::nanoseconds{std::chrono::hours{5}};
    t2.revoked   = true;
    REQUIRE(r.ingest(t2));

    // Even older than t2, but expired — must qualify too.
    ConsentToken t3 = t1;
    t3.token_id   = "ct_oldest_3";
    t3.issued_at  = base - std::chrono::nanoseconds{std::chrono::hours{10}};
    t3.expires_at = base - std::chrono::nanoseconds{std::chrono::hours{9}};
    REQUIRE(r.ingest(t3));

    auto out = r.find_oldest_token_for_patient(pid);
    REQUIRE(out);
    CHECK(out.value().token_id == "ct_oldest_3");
}

TEST_CASE("find_oldest_token_for_patient: not_found when patient has no tokens") {
    ConsentRegistry r;
    auto pid = PatientId::pseudonymous("p_oldest_none");
    // Another patient has tokens; ours does not.
    REQUIRE(r.grant(PatientId::pseudonymous("someone_else"), {Purpose::triage}, 1h));
    auto out = r.find_oldest_token_for_patient(pid);
    CHECK(!out);
    CHECK(out.error().code() == ErrorCode::not_found);
}

TEST_CASE("find_oldest_token_for_patient: ignores other patients") {
    ConsentRegistry r;
    auto target = PatientId::pseudonymous("p_oldest_target");
    auto other  = PatientId::pseudonymous("p_oldest_other");
    auto base   = Time::now();

    // Other patient has a much older token — must NOT be returned.
    ConsentToken o;
    o.token_id   = "ct_oldest_other";
    o.patient    = other;
    o.purposes   = {Purpose::triage};
    o.issued_at  = base - std::chrono::nanoseconds{std::chrono::hours{100}};
    o.expires_at = base + std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(o));

    // Target's only token.
    ConsentToken t;
    t.token_id   = "ct_oldest_target";
    t.patient    = target;
    t.purposes   = {Purpose::operations};
    t.issued_at  = base - std::chrono::nanoseconds{std::chrono::minutes{5}};
    t.expires_at = base + std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(t));

    auto out = r.find_oldest_token_for_patient(target);
    REQUIRE(out);
    CHECK(out.value().token_id == "ct_oldest_target");
    CHECK(out.value().patient == target);
}

// ---- estimated_avg_ttl -------------------------------------------------

TEST_CASE("estimated_avg_ttl: empty registry -> not_found") {
    ConsentRegistry r;
    auto out = r.estimated_avg_ttl();
    CHECK(!out);
    CHECK(out.error().code() == ErrorCode::not_found);
}

TEST_CASE("estimated_avg_ttl: single token -> exactly that token's TTL") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("p_avg_one");
    REQUIRE(r.grant(p, {Purpose::triage}, std::chrono::hours{1}));
    auto out = r.estimated_avg_ttl();
    REQUIRE(out);
    // Allow a few ns of slop because grant() reads Time::now() twice
    // internally (once on issue, once in the test now+1h math).
    auto expected = std::chrono::nanoseconds{std::chrono::hours{1}};
    auto diff     = (out.value() - expected).count();
    CHECK(diff >= -1000);
    CHECK(diff <= 1000);
}

TEST_CASE("estimated_avg_ttl: averages across tokens of mixed states and TTLs") {
    ConsentRegistry r;
    auto pid = PatientId::pseudonymous("p_avg_mix");
    auto base = Time::now();

    // 1-hour active.
    ConsentToken a;
    a.token_id   = "ct_avg_a";
    a.patient    = pid;
    a.purposes   = {Purpose::triage};
    a.issued_at  = base;
    a.expires_at = base + std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(a));

    // 3-hour revoked — counted: averaging is over all states.
    ConsentToken b = a;
    b.token_id   = "ct_avg_b";
    b.expires_at = base + std::chrono::nanoseconds{std::chrono::hours{3}};
    b.revoked    = true;
    REQUIRE(r.ingest(b));

    // 5-hour expired — also counted.
    ConsentToken c = a;
    c.token_id   = "ct_avg_c";
    c.issued_at  = base - std::chrono::nanoseconds{std::chrono::hours{10}};
    c.expires_at = c.issued_at + std::chrono::nanoseconds{std::chrono::hours{5}};
    REQUIRE(r.ingest(c));

    auto out = r.estimated_avg_ttl();
    REQUIRE(out);
    // Mean(1h, 3h, 5h) = 3h.
    auto expected = std::chrono::nanoseconds{std::chrono::hours{3}};
    auto diff     = (out.value() - expected).count();
    CHECK(diff >= -1000);
    CHECK(diff <= 1000);
}

TEST_CASE("estimated_avg_ttl: zero-duration tokens are valid (mean drops)") {
    ConsentRegistry r;
    auto pid = PatientId::pseudonymous("p_avg_zero");
    auto base = Time::now();

    ConsentToken a;
    a.token_id   = "ct_avg_z1";
    a.patient    = pid;
    a.purposes   = {Purpose::triage};
    a.issued_at  = base;
    a.expires_at = base + std::chrono::nanoseconds{std::chrono::hours{2}};
    REQUIRE(r.ingest(a));

    // Zero-duration token: issued and expires at the same instant.
    ConsentToken b = a;
    b.token_id   = "ct_avg_z2";
    b.expires_at = b.issued_at;
    REQUIRE(r.ingest(b));

    auto out = r.estimated_avg_ttl();
    REQUIRE(out);
    // Mean(2h, 0) = 1h.
    auto expected = std::chrono::nanoseconds{std::chrono::hours{1}};
    auto diff     = (out.value() - expected).count();
    CHECK(diff >= -1000);
    CHECK(diff <= 1000);
}

// ---- find_longest_lived_for_patient ------------------------------------

TEST_CASE("find_longest_lived_for_patient: picks largest (expires_at - issued_at)") {
    ConsentRegistry r;
    auto pid = PatientId::pseudonymous("p_long_any");
    auto base = Time::now();

    ConsentToken short_tok;
    short_tok.token_id   = "ct_long_short";
    short_tok.patient    = pid;
    short_tok.purposes   = {Purpose::triage};
    short_tok.issued_at  = base;
    short_tok.expires_at = base + std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(short_tok));

    ConsentToken long_tok = short_tok;
    long_tok.token_id   = "ct_long_long";
    long_tok.expires_at = base + std::chrono::nanoseconds{std::chrono::hours{72}};
    REQUIRE(r.ingest(long_tok));

    ConsentToken mid_tok = short_tok;
    mid_tok.token_id   = "ct_long_mid";
    mid_tok.expires_at = base + std::chrono::nanoseconds{std::chrono::hours{12}};
    REQUIRE(r.ingest(mid_tok));

    auto out = r.find_longest_lived_for_patient(pid);
    REQUIRE(out);
    CHECK(out.value().token_id == "ct_long_long");
}

TEST_CASE("find_longest_lived_for_patient: revoked and expired tokens still qualify") {
    ConsentRegistry r;
    auto pid = PatientId::pseudonymous("p_long_states");
    auto base = Time::now();

    // Active 1-hour token.
    REQUIRE(r.grant(pid, {Purpose::triage}, std::chrono::hours{1}));

    // Revoked 100-hour token — must win on lifespan, state doesn't matter.
    ConsentToken rv;
    rv.token_id   = "ct_long_rv";
    rv.patient    = pid;
    rv.purposes   = {Purpose::operations};
    rv.issued_at  = base;
    rv.expires_at = base + std::chrono::nanoseconds{std::chrono::hours{100}};
    rv.revoked    = true;
    REQUIRE(r.ingest(rv));

    // Expired 50-hour token — second-longest by lifespan.
    ConsentToken ex;
    ex.token_id   = "ct_long_ex";
    ex.patient    = pid;
    ex.purposes   = {Purpose::research};
    ex.issued_at  = base - std::chrono::nanoseconds{std::chrono::hours{200}};
    ex.expires_at = ex.issued_at + std::chrono::nanoseconds{std::chrono::hours{50}};
    REQUIRE(r.ingest(ex));

    auto out = r.find_longest_lived_for_patient(pid);
    REQUIRE(out);
    CHECK(out.value().token_id == "ct_long_rv");
    CHECK(out.value().revoked == true);
}

TEST_CASE("find_longest_lived_for_patient: not_found when patient has no tokens") {
    ConsentRegistry r;
    auto pid = PatientId::pseudonymous("p_long_none");
    REQUIRE(r.grant(PatientId::pseudonymous("not_us"), {Purpose::triage}, 1h));
    auto out = r.find_longest_lived_for_patient(pid);
    CHECK(!out);
    CHECK(out.error().code() == ErrorCode::not_found);
}

TEST_CASE("find_longest_lived_for_patient: ignores other patients' longer grants") {
    ConsentRegistry r;
    auto target = PatientId::pseudonymous("p_long_target");
    auto other  = PatientId::pseudonymous("p_long_other");
    auto base   = Time::now();

    // Other patient has a much longer grant.
    ConsentToken o;
    o.token_id   = "ct_long_other";
    o.patient    = other;
    o.purposes   = {Purpose::triage};
    o.issued_at  = base;
    o.expires_at = base + std::chrono::nanoseconds{std::chrono::hours{1000}};
    REQUIRE(r.ingest(o));

    // Target has a comparatively short grant.
    ConsentToken t;
    t.token_id   = "ct_long_target_short";
    t.patient    = target;
    t.purposes   = {Purpose::operations};
    t.issued_at  = base;
    t.expires_at = base + std::chrono::nanoseconds{std::chrono::hours{2}};
    REQUIRE(r.ingest(t));

    auto out = r.find_longest_lived_for_patient(target);
    REQUIRE(out);
    CHECK(out.value().token_id == "ct_long_target_short");
}

// ---- tokens_with_purpose -----------------------------------------------

TEST_CASE("tokens_with_purpose: returns only active tokens containing the purpose") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("p_twp_active");

    auto t1 = r.grant(p, {Purpose::research, Purpose::triage}, 1h).value();
    auto t2 = r.grant(p, {Purpose::research}, 1h).value();
    REQUIRE(r.grant(p, {Purpose::operations}, 1h));  // unrelated purpose

    auto got = r.tokens_with_purpose(Purpose::research);
    CHECK(got.size() == 2);
    std::unordered_set<std::string> ids;
    for (const auto& tok : got) ids.insert(tok.token_id);
    CHECK(ids.count(t1.token_id) == 1);
    CHECK(ids.count(t2.token_id) == 1);
}

TEST_CASE("tokens_with_purpose: skips revoked tokens (distinct from tokens_for_purpose)") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("p_twp_rev");

    auto live = r.grant(p, {Purpose::research}, 1h).value();
    auto gone = r.grant(p, {Purpose::research}, 1h).value();
    REQUIRE(r.revoke(gone.token_id));

    // tokens_for_purpose returns ALL states — sees both.
    CHECK(r.tokens_for_purpose(Purpose::research).size() == 2);
    // tokens_with_purpose is active-only — sees only the live one.
    auto got = r.tokens_with_purpose(Purpose::research);
    REQUIRE(got.size() == 1);
    CHECK(got.front().token_id == live.token_id);
}

TEST_CASE("tokens_with_purpose: skips expired tokens") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("p_twp_expired");

    // Expired research token via ingest.
    ConsentToken expired;
    expired.token_id   = "ct_twp_exp";
    expired.patient    = p;
    expired.purposes   = {Purpose::research};
    expired.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    expired.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(expired));

    // Active research token.
    auto live = r.grant(p, {Purpose::research}, 1h).value();

    // tokens_for_purpose sees both; tokens_with_purpose sees only live.
    CHECK(r.tokens_for_purpose(Purpose::research).size() == 2);
    auto got = r.tokens_with_purpose(Purpose::research);
    REQUIRE(got.size() == 1);
    CHECK(got.front().token_id == live.token_id);
}

TEST_CASE("tokens_with_purpose: empty when no token grants the purpose") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("p_twp_none");
    REQUIRE(r.grant(p, {Purpose::triage, Purpose::operations}, 1h));
    auto got = r.tokens_with_purpose(Purpose::research);
    CHECK(got.empty());
}

// ---- has_any_active ----------------------------------------------------

TEST_CASE("has_any_active: empty registry -> false") {
    ConsentRegistry r;
    CHECK(r.has_any_active() == false);
}

TEST_CASE("has_any_active: true after a single grant, false after revoke") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("p_haa_one");
    auto t = r.grant(p, {Purpose::triage}, 1h);
    REQUIRE(t);
    CHECK(r.has_any_active() == true);
    REQUIRE(r.revoke(t.value().token_id));
    CHECK(r.has_any_active() == false);
}

TEST_CASE("has_any_active: false when only expired and revoked tokens exist") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("p_haa_dead");

    // Revoked.
    auto rv = r.grant(p, {Purpose::triage}, 1h);
    REQUIRE(rv);
    REQUIRE(r.revoke(rv.value().token_id));

    // Expired via ingest.
    ConsentToken ex;
    ex.token_id   = "ct_haa_ex";
    ex.patient    = p;
    ex.purposes   = {Purpose::operations};
    ex.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    ex.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(ex));

    CHECK(r.has_any_active() == false);
}

TEST_CASE("has_any_active: true when at least one patient has a live grant") {
    ConsentRegistry r;
    auto p1 = PatientId::pseudonymous("p_haa_alpha");
    auto p2 = PatientId::pseudonymous("p_haa_beta");
    // p1's grant is revoked; p2's is live — overall: true.
    auto t1 = r.grant(p1, {Purpose::triage}, 1h);
    REQUIRE(t1);
    REQUIRE(r.revoke(t1.value().token_id));
    REQUIRE(r.grant(p2, {Purpose::operations}, 1h));
    CHECK(r.has_any_active() == true);
}

// ---- summary_string ----------------------------------------------------

TEST_CASE("summary_string: empty registry produces all-zero line") {
    ConsentRegistry r;
    auto line = r.summary_string();
    CHECK(line == "total=0 active=0 revoked=0 expired=0 patients=0");
    // No trailing newline.
    CHECK(line.find('\n') == std::string::npos);
}

TEST_CASE("summary_string: counts match summary() field-for-field") {
    ConsentRegistry r;
    auto pa = PatientId::pseudonymous("ss_a");
    auto pb = PatientId::pseudonymous("ss_b");
    REQUIRE(r.grant(pa, {Purpose::triage}, 1h));
    REQUIRE(r.grant(pa, {Purpose::operations}, 1h));
    auto rv = r.grant(pb, {Purpose::ambient_documentation}, 1h).value();
    REQUIRE(r.revoke(rv.token_id));
    ConsentToken ex;
    ex.token_id   = "ct_ss_exp";
    ex.patient    = pb;
    ex.purposes   = {Purpose::medication_review};
    ex.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    ex.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(ex));

    auto s = r.summary();
    auto line = r.summary_string();
    // Every field must appear in the formatted string.
    CHECK(line.find("total=" + std::to_string(s.total)) != std::string::npos);
    CHECK(line.find("active=" + std::to_string(s.active)) != std::string::npos);
    CHECK(line.find("revoked=" + std::to_string(s.revoked)) != std::string::npos);
    CHECK(line.find("expired=" + std::to_string(s.expired)) != std::string::npos);
    CHECK(line.find("patients=" + std::to_string(s.patients)) != std::string::npos);
}

TEST_CASE("summary_string: format is single-line ASCII with documented order") {
    ConsentRegistry r;
    REQUIRE(r.grant(PatientId::pseudonymous("ss_one"), {Purpose::triage}, 1h));
    auto line = r.summary_string();
    // Documented field order: total, active, revoked, expired, patients.
    auto pos_total    = line.find("total=");
    auto pos_active   = line.find("active=");
    auto pos_revoked  = line.find("revoked=");
    auto pos_expired  = line.find("expired=");
    auto pos_patients = line.find("patients=");
    REQUIRE(pos_total    != std::string::npos);
    REQUIRE(pos_active   != std::string::npos);
    REQUIRE(pos_revoked  != std::string::npos);
    REQUIRE(pos_expired  != std::string::npos);
    REQUIRE(pos_patients != std::string::npos);
    CHECK(pos_total < pos_active);
    CHECK(pos_active < pos_revoked);
    CHECK(pos_revoked < pos_expired);
    CHECK(pos_expired < pos_patients);
    // ASCII only: no high bytes.
    for (char c : line) {
        CHECK(static_cast<unsigned char>(c) < 0x80u);
    }
}

TEST_CASE("summary_string: tracks mutations across grant/revoke/clear") {
    ConsentRegistry r;
    CHECK(r.summary_string() == "total=0 active=0 revoked=0 expired=0 patients=0");
    auto p = PatientId::pseudonymous("ss_mut");
    auto t = r.grant(p, {Purpose::triage}, 1h).value();
    CHECK(r.summary_string() ==
          "total=1 active=1 revoked=0 expired=0 patients=1");
    REQUIRE(r.revoke(t.token_id));
    CHECK(r.summary_string() ==
          "total=1 active=0 revoked=1 expired=0 patients=1");
    r.clear();
    CHECK(r.summary_string() == "total=0 active=0 revoked=0 expired=0 patients=0");
}

// ---- distinct_purposes_in_use ------------------------------------------

TEST_CASE("distinct_purposes_in_use: empty registry is empty") {
    ConsentRegistry r;
    CHECK(r.distinct_purposes_in_use().empty());
}

TEST_CASE("distinct_purposes_in_use: returns sorted distinct active purposes") {
    ConsentRegistry r;
    auto a = PatientId::pseudonymous("dpu_a");
    auto b = PatientId::pseudonymous("dpu_b");
    // Overlapping purpose lists across patients; expect deduped, sorted.
    REQUIRE(r.grant(a, {Purpose::research, Purpose::triage}, 1h));
    REQUIRE(r.grant(b, {Purpose::triage, Purpose::ambient_documentation}, 1h));
    auto out = r.distinct_purposes_in_use();
    REQUIRE(out.size() == 3);
    // Sorted by enum value: ambient_documentation(1) < triage(3) < research(7).
    CHECK(out[0] == Purpose::ambient_documentation);
    CHECK(out[1] == Purpose::triage);
    CHECK(out[2] == Purpose::research);
}

TEST_CASE("distinct_purposes_in_use: ignores revoked and expired tokens") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("dpu_dead");
    // Live grant: triage stays.
    REQUIRE(r.grant(p, {Purpose::triage}, 1h));
    // Revoked grant: research must NOT appear.
    auto rv = r.grant(p, {Purpose::research}, 1h).value();
    REQUIRE(r.revoke(rv.token_id));
    // Expired grant: operations must NOT appear.
    ConsentToken ex;
    ex.token_id   = "ct_dpu_exp";
    ex.patient    = p;
    ex.purposes   = {Purpose::operations};
    ex.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    ex.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(ex));

    auto out = r.distinct_purposes_in_use();
    REQUIRE(out.size() == 1);
    CHECK(out[0] == Purpose::triage);
}

TEST_CASE("distinct_purposes_in_use: distinct from 'every purpose ever granted'") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("dpu_revoke_only");
    // Grant research then revoke — historically granted but not in use now.
    auto t = r.grant(p, {Purpose::research}, 1h).value();
    REQUIRE(r.revoke(t.token_id));
    // tokens_for_purpose sees the revoked grant; distinct_purposes_in_use
    // does not surface it.
    CHECK(r.tokens_for_purpose(Purpose::research).size() == 1);
    CHECK(r.distinct_purposes_in_use().empty());
}

// ---- is_revoked --------------------------------------------------------

TEST_CASE("is_revoked: missing token id returns false") {
    ConsentRegistry r;
    CHECK(r.is_revoked("ct_nonexistent") == false);
    CHECK(r.is_revoked("") == false);
}

TEST_CASE("is_revoked: active token returns false, revoked token returns true") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("ir_one");
    auto t = r.grant(p, {Purpose::triage}, 1h).value();
    CHECK(r.is_revoked(t.token_id) == false);
    REQUIRE(r.revoke(t.token_id));
    CHECK(r.is_revoked(t.token_id) == true);
}

TEST_CASE("is_revoked: expired-but-not-revoked tokens still report false") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("ir_expired");
    ConsentToken ex;
    ex.token_id   = "ct_ir_exp";
    ex.patient    = p;
    ex.purposes   = {Purpose::operations};
    ex.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    ex.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(ex));
    // Expired but the revoked flag was never set: missing != revoked,
    // and expired != revoked.
    CHECK(r.is_revoked("ct_ir_exp") == false);
}

TEST_CASE("is_revoked: dual to is_token_active for revoked rows") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("ir_dual");
    auto t = r.grant(p, {Purpose::triage}, 1h).value();
    REQUIRE(r.revoke(t.token_id));
    CHECK(r.is_revoked(t.token_id) == true);
    CHECK(r.is_token_active(t.token_id) == false);
    // remove() drops the row entirely; both probes report the absence.
    REQUIRE(r.remove(t.token_id));
    CHECK(r.is_revoked(t.token_id) == false);
    CHECK(r.is_token_active(t.token_id) == false);
}

// ---- find_token_for_any_purpose ----------------------------------------

TEST_CASE("find_token_for_any_purpose: finds a token covering any requested purpose") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("fap_basic");
    auto t = r.grant(p, {Purpose::triage}, 1h).value();
    std::vector<Purpose> wanted = {Purpose::research, Purpose::triage};
    auto hit = r.find_token_for_any_purpose(p, std::span<const Purpose>{wanted});
    REQUIRE(hit);
    CHECK(hit.value().token_id == t.token_id);
}

TEST_CASE("find_token_for_any_purpose: returns not_found if no purpose matches") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("fap_miss");
    REQUIRE(r.grant(p, {Purpose::triage}, 1h));
    std::vector<Purpose> wanted = {Purpose::research, Purpose::operations};
    auto hit = r.find_token_for_any_purpose(p, std::span<const Purpose>{wanted});
    REQUIRE(!hit);
    CHECK(hit.error().code() == ErrorCode::not_found);
}

TEST_CASE("find_token_for_any_purpose: empty purposes -> invalid_argument") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("fap_empty");
    REQUIRE(r.grant(p, {Purpose::triage}, 1h));
    std::vector<Purpose> empty;
    auto hit = r.find_token_for_any_purpose(p, std::span<const Purpose>{empty});
    REQUIRE(!hit);
    CHECK(hit.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("find_token_for_any_purpose: ignores revoked and expired tokens") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("fap_dead");
    // Revoked grant covering triage — must NOT match.
    auto rv = r.grant(p, {Purpose::triage}, 1h).value();
    REQUIRE(r.revoke(rv.token_id));
    // Expired grant covering operations — must NOT match.
    ConsentToken ex;
    ex.token_id   = "ct_fap_exp";
    ex.patient    = p;
    ex.purposes   = {Purpose::operations};
    ex.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    ex.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(ex));

    std::vector<Purpose> wanted = {Purpose::triage, Purpose::operations};
    auto hit = r.find_token_for_any_purpose(p, std::span<const Purpose>{wanted});
    REQUIRE(!hit);
    CHECK(hit.error().code() == ErrorCode::not_found);
}

TEST_CASE("find_token_for_any_purpose: distinct from find_token_granting_all") {
    ConsentRegistry r;
    auto p = PatientId::pseudonymous("fap_distinct");
    // Two tokens that, taken together, cover triage+research, but
    // neither alone does.
    REQUIRE(r.grant(p, {Purpose::triage},   1h));
    REQUIRE(r.grant(p, {Purpose::research}, 1h));
    std::vector<Purpose> wanted = {Purpose::triage, Purpose::research};

    // ANY: the first token whose list intersects {triage, research}
    // satisfies — at least one matching token exists.
    auto any_hit = r.find_token_for_any_purpose(p, std::span<const Purpose>{wanted});
    REQUIRE(any_hit);

    // ALL: no single token covers BOTH purposes — so the all-form
    // returns not_found.
    auto all_hit = r.find_token_granting_all(p, std::span<const Purpose>{wanted});
    REQUIRE(!all_hit);
    CHECK(all_hit.error().code() == ErrorCode::not_found);
}

// ---- count_active_for_patients -----------------------------------------

TEST_CASE("count_active_for_patients: empty span returns 0") {
    ConsentRegistry r;
    REQUIRE(r.grant(PatientId::pseudonymous("cafp_x"), {Purpose::triage}, 1h));
    std::vector<PatientId> empty;
    CHECK(r.count_active_for_patients(std::span<const PatientId>{empty}) == 0);
}

TEST_CASE("count_active_for_patients: counts only patients with active tokens") {
    ConsentRegistry r;
    auto a = PatientId::pseudonymous("cafp_a");
    auto b = PatientId::pseudonymous("cafp_b");
    auto c = PatientId::pseudonymous("cafp_c");

    REQUIRE(r.grant(a, {Purpose::triage}, 1h));            // active
    auto rv = r.grant(b, {Purpose::operations}, 1h).value();
    REQUIRE(r.revoke(rv.token_id));                        // b: revoked-only
    // c: no tokens at all.

    std::vector<PatientId> cohort = {a, b, c};
    auto n = r.count_active_for_patients(std::span<const PatientId>{cohort});
    CHECK(n == 1);  // only a
}

TEST_CASE("count_active_for_patients: dedupes duplicate patient ids in input") {
    ConsentRegistry r;
    auto a = PatientId::pseudonymous("cafp_dup");
    REQUIRE(r.grant(a, {Purpose::triage}, 1h));
    // a appears three times in the input — it must still count as 1.
    std::vector<PatientId> cohort = {a, a, a};
    CHECK(r.count_active_for_patients(std::span<const PatientId>{cohort}) == 1);
}

TEST_CASE("count_active_for_patients: ignores expired-only patients") {
    ConsentRegistry r;
    auto live = PatientId::pseudonymous("cafp_live");
    auto dead = PatientId::pseudonymous("cafp_dead");

    REQUIRE(r.grant(live, {Purpose::triage}, 1h));
    // dead has only an expired token.
    ConsentToken ex;
    ex.token_id   = "ct_cafp_exp";
    ex.patient    = dead;
    ex.purposes   = {Purpose::operations};
    ex.issued_at  = Time::now() - std::chrono::nanoseconds{std::chrono::hours{2}};
    ex.expires_at = Time::now() - std::chrono::nanoseconds{std::chrono::hours{1}};
    REQUIRE(r.ingest(ex));

    std::vector<PatientId> cohort = {live, dead};
    CHECK(r.count_active_for_patients(std::span<const PatientId>{cohort}) == 1);
}

TEST_CASE("count_active_for_patients: result bounded by cohort size, ignores extra patients") {
    ConsentRegistry r;
    auto a = PatientId::pseudonymous("cafp_subset_a");
    auto b = PatientId::pseudonymous("cafp_subset_b");
    auto outside = PatientId::pseudonymous("cafp_subset_outside");

    REQUIRE(r.grant(a,       {Purpose::triage},    1h));
    REQUIRE(r.grant(b,       {Purpose::research},  1h));
    REQUIRE(r.grant(outside, {Purpose::operations}, 1h));

    // Cohort excludes 'outside' — even though it has an active token,
    // the count is bounded by the requested set.
    std::vector<PatientId> cohort = {a, b};
    CHECK(r.count_active_for_patients(std::span<const PatientId>{cohort}) == 2);
}
