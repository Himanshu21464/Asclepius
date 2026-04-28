// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
//
// Tests for EmergencyOverride — DPDP Act § 7 break-glass primitive.
// Each TEST_CASE is named with the "emergency_override:" prefix so the
// suite is greppable in CI logs and matches the per-primitive naming
// convention the rest of the consent tests use.

#include <doctest/doctest.h>

#include "asclepius/consent.hpp"
#include "asclepius/identity.hpp"
#include "asclepius/core.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace asclepius;
using namespace std::chrono_literals;

namespace {

// Helper: synthesise a token whose deadline is already in the past by
// referencing Time::now() and subtracting. ingest() preserves the
// timestamps verbatim so this is the cleanest way to test
// is_overdue / overdue_backfills without sleeping.
EmergencyOverrideToken
make_overdue_token(std::string id,
                   std::string actor,
                   std::string patient,
                   std::string reason) {
    EmergencyOverrideToken t;
    t.token_id             = std::move(id);
    t.actor                = ActorId::clinician(std::move(actor));
    t.patient              = PatientId::pseudonymous(std::move(patient));
    t.reason               = std::move(reason);
    t.activated_at         = Time::now() - std::chrono::nanoseconds{2h};
    t.backfill_deadline    = Time::now() - std::chrono::nanoseconds{1h};
    t.backfilled           = false;
    t.backfill_evidence_id = "";
    return t;
}

}  // namespace

TEST_CASE("emergency_override: default constructor uses 72h backfill window") {
    EmergencyOverride eo;
    CHECK(eo.backfill_window() == std::chrono::hours(72));
}

TEST_CASE("emergency_override: custom window is honoured") {
    EmergencyOverride eo{std::chrono::hours(24)};
    CHECK(eo.backfill_window() == std::chrono::hours(24));

    EmergencyOverride eo2{std::chrono::seconds(60)};
    CHECK(eo2.backfill_window() == std::chrono::seconds(60));
}

TEST_CASE("emergency_override: activate rejects empty reason") {
    EmergencyOverride eo;
    auto r = eo.activate(ActorId::clinician("alice"),
                         PatientId::pseudonymous("p1"),
                         "");
    REQUIRE(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("emergency_override: activate returns a populated token") {
    EmergencyOverride eo;
    auto actor   = ActorId::clinician("bob");
    auto patient = PatientId::pseudonymous("p2");
    auto r = eo.activate(actor, patient, "patient unconscious in ED");
    REQUIRE(r);
    const auto& t = r.value();
    CHECK(!t.token_id.empty());
    CHECK(t.actor == actor);
    CHECK(t.patient == patient);
    CHECK(t.reason == "patient unconscious in ED");
    CHECK(!t.backfilled);
    CHECK(t.backfill_evidence_id.empty());
}

TEST_CASE("emergency_override: backfill_deadline equals activated_at + window") {
    EmergencyOverride eo{std::chrono::hours(24)};
    auto r = eo.activate(ActorId::clinician("c1"),
                         PatientId::pseudonymous("p3"),
                         "code blue");
    REQUIRE(r);
    auto delta = r.value().backfill_deadline - r.value().activated_at;
    CHECK(delta == std::chrono::nanoseconds{std::chrono::hours(24)});
}

TEST_CASE("emergency_override: total_count increments per activation") {
    EmergencyOverride eo;
    CHECK(eo.total_count() == 0);
    REQUIRE(eo.activate(ActorId::clinician("a"),
                        PatientId::pseudonymous("p_a"), "r1"));
    CHECK(eo.total_count() == 1);
    REQUIRE(eo.activate(ActorId::clinician("b"),
                        PatientId::pseudonymous("p_b"), "r2"));
    CHECK(eo.total_count() == 2);
    REQUIRE(eo.activate(ActorId::clinician("c"),
                        PatientId::pseudonymous("p_c"), "r3"));
    CHECK(eo.total_count() == 3);
}

TEST_CASE("emergency_override: pending and overdue counts after activation") {
    EmergencyOverride eo;
    REQUIRE(eo.activate(ActorId::clinician("a"),
                        PatientId::pseudonymous("p1"), "r1"));
    REQUIRE(eo.activate(ActorId::clinician("b"),
                        PatientId::pseudonymous("p2"), "r2"));
    CHECK(eo.pending_count() == 2);
    CHECK(eo.overdue_count() == 0);
    CHECK(eo.completed_count() == 0);
}

TEST_CASE("emergency_override: is_pending_backfill flips after backfill") {
    EmergencyOverride eo;
    auto r = eo.activate(ActorId::clinician("alice"),
                         PatientId::pseudonymous("p"), "reason");
    REQUIRE(r);
    auto id = r.value().token_id;
    CHECK(eo.is_pending_backfill(id));
    REQUIRE(eo.backfill(id, "evidence-doc-1"));
    CHECK(!eo.is_pending_backfill(id));
    CHECK(!eo.is_pending_backfill("does-not-exist"));
}

TEST_CASE("emergency_override: backfill rejects empty evidence_id") {
    EmergencyOverride eo;
    auto r = eo.activate(ActorId::clinician("a"),
                         PatientId::pseudonymous("p"), "reason");
    REQUIRE(r);
    auto rb = eo.backfill(r.value().token_id, "");
    REQUIRE(!rb);
    CHECK(rb.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("emergency_override: backfill of unknown token is not_found") {
    EmergencyOverride eo;
    auto rb = eo.backfill("emo-does-not-exist", "evidence-1");
    REQUIRE(!rb);
    CHECK(rb.error().code() == ErrorCode::not_found);
}

TEST_CASE("emergency_override: backfill fires observer with backfilled event") {
    EmergencyOverride eo;
    std::vector<EmergencyOverride::Event> events;
    eo.set_observer([&](EmergencyOverride::Event e,
                        const EmergencyOverrideToken&) {
        events.push_back(e);
    });
    auto r = eo.activate(ActorId::clinician("a"),
                         PatientId::pseudonymous("p"), "reason");
    REQUIRE(r);
    REQUIRE(eo.backfill(r.value().token_id, "ev-1"));
    REQUIRE(events.size() == 2);
    CHECK(events[0] == EmergencyOverride::Event::activated);
    CHECK(events[1] == EmergencyOverride::Event::backfilled);
}

TEST_CASE("emergency_override: counts shift after backfill") {
    EmergencyOverride eo;
    auto r1 = eo.activate(ActorId::clinician("a"),
                          PatientId::pseudonymous("p1"), "r1");
    auto r2 = eo.activate(ActorId::clinician("b"),
                          PatientId::pseudonymous("p2"), "r2");
    REQUIRE(r1);
    REQUIRE(r2);
    REQUIRE(eo.backfill(r1.value().token_id, "ev-1"));
    CHECK(eo.pending_count() == 1);
    CHECK(eo.completed_count() == 1);
    CHECK(eo.total_count() == 2);
    CHECK(!eo.is_pending_backfill(r1.value().token_id));
    CHECK(eo.is_pending_backfill(r2.value().token_id));
}

TEST_CASE("emergency_override: backfill with same evidence_id is idempotent") {
    EmergencyOverride eo;
    int activated_count  = 0;
    int backfilled_count = 0;
    eo.set_observer([&](EmergencyOverride::Event e,
                        const EmergencyOverrideToken&) {
        if (e == EmergencyOverride::Event::activated)  activated_count++;
        if (e == EmergencyOverride::Event::backfilled) backfilled_count++;
    });
    auto r = eo.activate(ActorId::clinician("a"),
                         PatientId::pseudonymous("p"), "reason");
    REQUIRE(r);
    auto id = r.value().token_id;
    REQUIRE(eo.backfill(id, "evidence-1"));
    REQUIRE(eo.backfill(id, "evidence-1"));    // second call: silent no-op
    REQUIRE(eo.backfill(id, "evidence-1"));    // third call: also silent
    CHECK(activated_count == 1);
    CHECK(backfilled_count == 1);              // observer NOT re-fired
    CHECK(eo.completed_count() == 1);
}

TEST_CASE("emergency_override: backfill with different evidence_id conflicts") {
    EmergencyOverride eo;
    auto r = eo.activate(ActorId::clinician("a"),
                         PatientId::pseudonymous("p"), "reason");
    REQUIRE(r);
    auto id = r.value().token_id;
    REQUIRE(eo.backfill(id, "evidence-1"));
    auto rb2 = eo.backfill(id, "evidence-2");
    REQUIRE(!rb2);
    CHECK(rb2.error().code() == ErrorCode::conflict);
}

TEST_CASE("emergency_override: is_overdue true past deadline") {
    EmergencyOverride eo;
    // Inject a token via ingest() whose deadline is already in the
    // past — clean way to exercise the overdue branch without
    // sleeping in the test.
    auto t = make_overdue_token("emo-past-1", "alice", "patient-1",
                                "old break-glass");
    REQUIRE(eo.ingest(t));
    CHECK(eo.is_overdue("emo-past-1"));
    CHECK(eo.overdue_count() == 1);
    CHECK(!eo.is_overdue("emo-does-not-exist"));

    // Activating a token NOW with a normal window should NOT be
    // overdue immediately.
    auto r = eo.activate(ActorId::clinician("b"),
                         PatientId::pseudonymous("p2"), "fresh");
    REQUIRE(r);
    CHECK(!eo.is_overdue(r.value().token_id));

    // Backfilling the overdue token clears the overdue state.
    REQUIRE(eo.backfill("emo-past-1", "evidence-late"));
    CHECK(!eo.is_overdue("emo-past-1"));
}

TEST_CASE("emergency_override: pending_backfills sorted by activated_at") {
    EmergencyOverride eo;
    auto r1 = eo.activate(ActorId::clinician("a"),
                          PatientId::pseudonymous("p1"), "r1");
    // Tiny pause to ensure monotonic timestamps even on fast machines.
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    auto r2 = eo.activate(ActorId::clinician("b"),
                          PatientId::pseudonymous("p2"), "r2");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    auto r3 = eo.activate(ActorId::clinician("c"),
                          PatientId::pseudonymous("p3"), "r3");
    REQUIRE(r1);
    REQUIRE(r2);
    REQUIRE(r3);
    auto rows = eo.pending_backfills();
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].activated_at <= rows[1].activated_at);
    CHECK(rows[1].activated_at <= rows[2].activated_at);
    CHECK(rows[0].token_id == r1.value().token_id);
    CHECK(rows[2].token_id == r3.value().token_id);
}

TEST_CASE("emergency_override: overdue_backfills sorted by deadline ascending") {
    EmergencyOverride eo;
    // Three overdue tokens with different deadlines.
    auto t1 = make_overdue_token("emo-A", "a1", "pa", "ra");
    t1.backfill_deadline = Time::now() - std::chrono::nanoseconds{30min};
    auto t2 = make_overdue_token("emo-B", "a2", "pb", "rb");
    t2.backfill_deadline = Time::now() - std::chrono::nanoseconds{2h};
    auto t3 = make_overdue_token("emo-C", "a3", "pc", "rc");
    t3.backfill_deadline = Time::now() - std::chrono::nanoseconds{1h};
    REQUIRE(eo.ingest(t1));
    REQUIRE(eo.ingest(t2));
    REQUIRE(eo.ingest(t3));
    auto rows = eo.overdue_backfills();
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].token_id == "emo-B");
    CHECK(rows[1].token_id == "emo-C");
    CHECK(rows[2].token_id == "emo-A");
    CHECK(rows[0].backfill_deadline < rows[1].backfill_deadline);
    CHECK(rows[1].backfill_deadline < rows[2].backfill_deadline);
}

TEST_CASE("emergency_override: completed_backfills returns only backfilled rows") {
    EmergencyOverride eo;
    auto r1 = eo.activate(ActorId::clinician("a"),
                          PatientId::pseudonymous("p1"), "r1");
    auto r2 = eo.activate(ActorId::clinician("b"),
                          PatientId::pseudonymous("p2"), "r2");
    auto r3 = eo.activate(ActorId::clinician("c"),
                          PatientId::pseudonymous("p3"), "r3");
    REQUIRE(r1); REQUIRE(r2); REQUIRE(r3);
    REQUIRE(eo.backfill(r1.value().token_id, "ev-1"));
    REQUIRE(eo.backfill(r3.value().token_id, "ev-3"));

    auto rows = eo.completed_backfills();
    REQUIRE(rows.size() == 2);
    for (const auto& t : rows) {
        CHECK(t.backfilled);
    }
    auto pending = eo.pending_backfills();
    REQUIRE(pending.size() == 1);
    CHECK(pending[0].token_id == r2.value().token_id);
}

TEST_CASE("emergency_override: patients() and actors() are distinct and sorted") {
    EmergencyOverride eo;
    REQUIRE(eo.activate(ActorId::clinician("zoe"),
                        PatientId::pseudonymous("p_z"), "r"));
    REQUIRE(eo.activate(ActorId::clinician("alice"),
                        PatientId::pseudonymous("p_a"), "r"));
    REQUIRE(eo.activate(ActorId::clinician("alice"),  // duplicate actor
                        PatientId::pseudonymous("p_b"), "r"));
    REQUIRE(eo.activate(ActorId::clinician("bob"),
                        PatientId::pseudonymous("p_a"), "r"));  // duplicate patient

    auto pats = eo.patients();
    REQUIRE(pats.size() == 3);  // p_a, p_b, p_z
    CHECK(pats[0].str() < pats[1].str());
    CHECK(pats[1].str() < pats[2].str());

    auto acts = eo.actors();
    REQUIRE(acts.size() == 3);  // alice, bob, zoe
    CHECK(acts[0].str() < acts[1].str());
    CHECK(acts[1].str() < acts[2].str());
    CHECK(std::string{acts[0].str()} == "clinician:alice");
    CHECK(std::string{acts[1].str()} == "clinician:bob");
    CHECK(std::string{acts[2].str()} == "clinician:zoe");
}

TEST_CASE("emergency_override: set_backfill_window only affects future activations") {
    EmergencyOverride eo{std::chrono::hours(72)};
    auto r1 = eo.activate(ActorId::clinician("a"),
                          PatientId::pseudonymous("p1"), "first");
    REQUIRE(r1);
    auto delta_old = r1.value().backfill_deadline - r1.value().activated_at;
    CHECK(delta_old == std::chrono::nanoseconds{std::chrono::hours(72)});

    eo.set_backfill_window(std::chrono::hours(12));
    CHECK(eo.backfill_window() == std::chrono::hours(12));

    auto r2 = eo.activate(ActorId::clinician("b"),
                          PatientId::pseudonymous("p2"), "second");
    REQUIRE(r2);
    auto delta_new = r2.value().backfill_deadline - r2.value().activated_at;
    CHECK(delta_new == std::chrono::nanoseconds{std::chrono::hours(12)});

    // The original token still has the old deadline.
    auto refetched = eo.get(r1.value().token_id);
    REQUIRE(refetched);
    auto delta_persisted =
        refetched.value().backfill_deadline - refetched.value().activated_at;
    CHECK(delta_persisted == std::chrono::nanoseconds{std::chrono::hours(72)});
}

TEST_CASE("emergency_override: ingest round-trips snapshot") {
    EmergencyOverride eo;
    REQUIRE(eo.activate(ActorId::clinician("a"),
                        PatientId::pseudonymous("p1"), "r1"));
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    auto r2 = eo.activate(ActorId::clinician("b"),
                          PatientId::pseudonymous("p2"), "r2");
    REQUIRE(r2);
    REQUIRE(eo.backfill(r2.value().token_id, "ev-2"));

    auto before = eo.snapshot();
    REQUIRE(before.size() == 2);

    EmergencyOverride other;
    for (const auto& t : before) {
        REQUIRE(other.ingest(t));
    }
    auto after = other.snapshot();
    REQUIRE(after.size() == before.size());
    for (std::size_t i = 0; i < before.size(); ++i) {
        CHECK(before[i].token_id          == after[i].token_id);
        CHECK(before[i].actor             == after[i].actor);
        CHECK(before[i].patient           == after[i].patient);
        CHECK(before[i].reason            == after[i].reason);
        CHECK(before[i].activated_at      == after[i].activated_at);
        CHECK(before[i].backfill_deadline == after[i].backfill_deadline);
        CHECK(before[i].backfilled        == after[i].backfilled);
        CHECK(before[i].backfill_evidence_id == after[i].backfill_evidence_id);
    }
}

TEST_CASE("emergency_override: ingest of identical token is silent no-op") {
    EmergencyOverride eo;
    EmergencyOverrideToken t;
    t.token_id          = "emo-replay-1";
    t.actor             = ActorId::clinician("alice");
    t.patient           = PatientId::pseudonymous("p1");
    t.reason            = "ED arrival";
    t.activated_at      = Time::now();
    t.backfill_deadline = t.activated_at + std::chrono::nanoseconds{72h};
    t.backfilled        = false;

    REQUIRE(eo.ingest(t));
    REQUIRE(eo.ingest(t));   // identical replay
    REQUIRE(eo.ingest(t));   // identical replay again
    CHECK(eo.total_count() == 1);
}

TEST_CASE("emergency_override: ingest of conflicting token returns conflict") {
    EmergencyOverride eo;
    EmergencyOverrideToken t1;
    t1.token_id          = "emo-clash";
    t1.actor             = ActorId::clinician("alice");
    t1.patient           = PatientId::pseudonymous("p1");
    t1.reason            = "first reason";
    t1.activated_at      = Time::now();
    t1.backfill_deadline = t1.activated_at + std::chrono::nanoseconds{72h};
    REQUIRE(eo.ingest(t1));

    EmergencyOverrideToken t2 = t1;
    t2.reason = "different reason";   // same id, different fields
    auto r = eo.ingest(t2);
    REQUIRE(!r);
    CHECK(r.error().code() == ErrorCode::conflict);
}

TEST_CASE("emergency_override: summary and summary_string reflect state") {
    EmergencyOverride eo;
    {
        auto s = eo.summary();
        CHECK(s.total == 0);
        CHECK(s.pending == 0);
        CHECK(s.overdue == 0);
        CHECK(s.completed == 0);
        CHECK(eo.summary_string() ==
              "total=0 pending=0 overdue=0 completed=0");
    }

    auto r1 = eo.activate(ActorId::clinician("a"),
                          PatientId::pseudonymous("p1"), "r1");
    auto r2 = eo.activate(ActorId::clinician("b"),
                          PatientId::pseudonymous("p2"), "r2");
    REQUIRE(r1); REQUIRE(r2);
    REQUIRE(eo.ingest(make_overdue_token("emo-late", "c", "p3", "late")));
    REQUIRE(eo.backfill(r1.value().token_id, "ev-1"));

    auto s = eo.summary();
    CHECK(s.total == 3);
    CHECK(s.completed == 1);   // r1 backfilled
    CHECK(s.pending == 2);     // r2 + emo-late are pending
    CHECK(s.overdue == 1);     // emo-late is past deadline

    CHECK(eo.summary_string() ==
          "total=3 pending=2 overdue=1 completed=1");
}

TEST_CASE("emergency_override: clear drops all tokens silently") {
    EmergencyOverride eo;
    int observer_calls = 0;
    eo.set_observer([&](EmergencyOverride::Event,
                        const EmergencyOverrideToken&) {
        observer_calls++;
    });
    REQUIRE(eo.activate(ActorId::clinician("a"),
                        PatientId::pseudonymous("p1"), "r1"));
    REQUIRE(eo.activate(ActorId::clinician("b"),
                        PatientId::pseudonymous("p2"), "r2"));
    CHECK(observer_calls == 2);   // two `activated` events fired
    eo.clear();
    CHECK(eo.total_count() == 0);
    CHECK(eo.pending_count() == 0);
    CHECK(observer_calls == 2);   // clear does not fire the observer
}

TEST_CASE("emergency_override: get returns not_found for missing token") {
    EmergencyOverride eo;
    auto r = eo.get("emo-never-was");
    REQUIRE(!r);
    CHECK(r.error().code() == ErrorCode::not_found);
}

TEST_CASE("emergency_override: get returns a populated token for a known id") {
    EmergencyOverride eo;
    auto r = eo.activate(ActorId::clinician("alice"),
                         PatientId::pseudonymous("p-known"),
                         "ED triage");
    REQUIRE(r);
    auto fetched = eo.get(r.value().token_id);
    REQUIRE(fetched);
    CHECK(fetched.value().token_id == r.value().token_id);
    CHECK(fetched.value().actor   == r.value().actor);
    CHECK(fetched.value().patient == r.value().patient);
    CHECK(fetched.value().reason  == "ED triage");
    CHECK(!fetched.value().backfilled);
}

TEST_CASE("emergency_override: activate with empty reason does NOT fire observer") {
    EmergencyOverride eo;
    int calls = 0;
    eo.set_observer([&](EmergencyOverride::Event,
                        const EmergencyOverrideToken&) {
        calls++;
    });
    auto r = eo.activate(ActorId::clinician("a"),
                         PatientId::pseudonymous("p"), "");
    REQUIRE(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
    CHECK(calls == 0);          // observer must NOT have been touched
    CHECK(eo.total_count() == 0);
}

TEST_CASE("emergency_override: activate fires observer with activated event") {
    EmergencyOverride eo;
    std::vector<EmergencyOverride::Event>   events;
    std::vector<std::string>                seen_ids;
    eo.set_observer([&](EmergencyOverride::Event ev,
                        const EmergencyOverrideToken& t) {
        events.push_back(ev);
        seen_ids.push_back(t.token_id);
    });
    auto r = eo.activate(ActorId::clinician("alice"),
                         PatientId::pseudonymous("p"), "trauma bay 4");
    REQUIRE(r);
    REQUIRE(events.size() == 1);
    CHECK(events[0] == EmergencyOverride::Event::activated);
    CHECK(seen_ids[0] == r.value().token_id);
}

TEST_CASE("emergency_override: ingest does NOT fire the observer") {
    EmergencyOverride eo;
    int calls = 0;
    eo.set_observer([&](EmergencyOverride::Event,
                        const EmergencyOverrideToken&) {
        calls++;
    });
    EmergencyOverrideToken t;
    t.token_id          = "emo-replay-x";
    t.actor             = ActorId::clinician("alice");
    t.patient           = PatientId::pseudonymous("p1");
    t.reason            = "ledger replay";
    t.activated_at      = Time::now();
    t.backfill_deadline = t.activated_at + std::chrono::nanoseconds{72h};
    REQUIRE(eo.ingest(t));
    CHECK(calls == 0);          // ingest is silent

    // A backfilled token replayed from the ledger also stays silent.
    EmergencyOverrideToken t2;
    t2.token_id             = "emo-replay-done";
    t2.actor                = ActorId::clinician("bob");
    t2.patient              = PatientId::pseudonymous("p2");
    t2.reason               = "post-hoc";
    t2.activated_at         = Time::now();
    t2.backfill_deadline    = t2.activated_at + std::chrono::nanoseconds{72h};
    t2.backfilled           = true;
    t2.backfill_evidence_id = "ev-old";
    REQUIRE(eo.ingest(t2));
    CHECK(calls == 0);
    CHECK(eo.total_count() == 2);
    CHECK(eo.completed_count() == 1);
}

// ---- active_for_patient -------------------------------------------------

TEST_CASE("emergency_override: active_for_patient returns only this patient's pending") {
    EmergencyOverride eo;
    auto pa = PatientId::pseudonymous("pa");
    auto pb = PatientId::pseudonymous("pb");
    REQUIRE(eo.activate(ActorId::clinician("c1"), pa, "ED arrival a1"));
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    REQUIRE(eo.activate(ActorId::clinician("c2"), pb, "ED arrival b1"));
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    REQUIRE(eo.activate(ActorId::clinician("c3"), pa, "ED arrival a2"));

    auto rows = eo.active_for_patient(pa);
    REQUIRE(rows.size() == 2);
    for (const auto& t : rows) {
        CHECK(t.patient == pa);
        CHECK(!t.backfilled);
    }
    // Sorted by activated_at ascending.
    CHECK(rows[0].activated_at <= rows[1].activated_at);

    auto rows_b = eo.active_for_patient(pb);
    REQUIRE(rows_b.size() == 1);
    CHECK(rows_b[0].patient == pb);
}

TEST_CASE("emergency_override: active_for_patient excludes already-backfilled tokens") {
    EmergencyOverride eo;
    auto pa = PatientId::pseudonymous("pa");
    auto r1 = eo.activate(ActorId::clinician("c1"), pa, "first");
    REQUIRE(r1);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    auto r2 = eo.activate(ActorId::clinician("c2"), pa, "second");
    REQUIRE(r2);
    REQUIRE(eo.backfill(r1.value().token_id, "ev-1"));

    auto rows = eo.active_for_patient(pa);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].token_id == r2.value().token_id);
    CHECK(!rows[0].backfilled);
}

TEST_CASE("emergency_override: active_for_patient empty for unknown patient") {
    EmergencyOverride eo;
    REQUIRE(eo.activate(ActorId::clinician("c1"),
                        PatientId::pseudonymous("known"),
                        "real reason"));
    auto rows = eo.active_for_patient(PatientId::pseudonymous("ghost"));
    CHECK(rows.empty());
}

TEST_CASE("emergency_override: active_for_patient ascending order matches pending_backfills") {
    EmergencyOverride eo;
    auto pa = PatientId::pseudonymous("pa");
    auto t1 = eo.activate(ActorId::clinician("c1"), pa, "r1");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    auto t2 = eo.activate(ActorId::clinician("c2"), pa, "r2");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    auto t3 = eo.activate(ActorId::clinician("c3"), pa, "r3");
    REQUIRE(t1); REQUIRE(t2); REQUIRE(t3);
    auto rows = eo.active_for_patient(pa);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].token_id == t1.value().token_id);
    CHECK(rows[1].token_id == t2.value().token_id);
    CHECK(rows[2].token_id == t3.value().token_id);
}

// ---- active_for_actor ---------------------------------------------------

TEST_CASE("emergency_override: active_for_actor returns only this actor's pending") {
    EmergencyOverride eo;
    auto alice = ActorId::clinician("alice");
    auto bob   = ActorId::clinician("bob");
    REQUIRE(eo.activate(alice, PatientId::pseudonymous("p1"), "r1"));
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    REQUIRE(eo.activate(bob,   PatientId::pseudonymous("p2"), "r2"));
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    REQUIRE(eo.activate(alice, PatientId::pseudonymous("p3"), "r3"));

    auto rows = eo.active_for_actor(alice);
    REQUIRE(rows.size() == 2);
    for (const auto& t : rows) {
        CHECK(t.actor == alice);
        CHECK(!t.backfilled);
    }
    CHECK(rows[0].activated_at <= rows[1].activated_at);

    auto rows_bob = eo.active_for_actor(bob);
    REQUIRE(rows_bob.size() == 1);
    CHECK(rows_bob[0].actor == bob);
}

TEST_CASE("emergency_override: active_for_actor excludes already-backfilled tokens") {
    EmergencyOverride eo;
    auto alice = ActorId::clinician("alice");
    auto r1 = eo.activate(alice, PatientId::pseudonymous("p1"), "r1");
    REQUIRE(r1);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    auto r2 = eo.activate(alice, PatientId::pseudonymous("p2"), "r2");
    REQUIRE(r2);
    REQUIRE(eo.backfill(r1.value().token_id, "ev-1"));

    auto rows = eo.active_for_actor(alice);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].token_id == r2.value().token_id);
    CHECK(!rows[0].backfilled);
}

TEST_CASE("emergency_override: active_for_actor empty for unknown actor") {
    EmergencyOverride eo;
    REQUIRE(eo.activate(ActorId::clinician("alice"),
                        PatientId::pseudonymous("p1"), "r1"));
    auto rows = eo.active_for_actor(ActorId::clinician("ghost"));
    CHECK(rows.empty());
}

TEST_CASE("emergency_override: active_for_actor ascending order matches activated_at") {
    EmergencyOverride eo;
    auto alice = ActorId::clinician("alice");
    auto t1 = eo.activate(alice, PatientId::pseudonymous("p1"), "r1");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    auto t2 = eo.activate(alice, PatientId::pseudonymous("p2"), "r2");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    auto t3 = eo.activate(alice, PatientId::pseudonymous("p3"), "r3");
    REQUIRE(t1); REQUIRE(t2); REQUIRE(t3);
    auto rows = eo.active_for_actor(alice);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].token_id == t1.value().token_id);
    CHECK(rows[1].token_id == t2.value().token_id);
    CHECK(rows[2].token_id == t3.value().token_id);
}

// ---- oldest_pending -----------------------------------------------------

TEST_CASE("emergency_override: oldest_pending returns the smallest activated_at") {
    EmergencyOverride eo;
    auto t1 = eo.activate(ActorId::clinician("c1"),
                          PatientId::pseudonymous("p1"), "first");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    auto t2 = eo.activate(ActorId::clinician("c2"),
                          PatientId::pseudonymous("p2"), "second");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    auto t3 = eo.activate(ActorId::clinician("c3"),
                          PatientId::pseudonymous("p3"), "third");
    REQUIRE(t1); REQUIRE(t2); REQUIRE(t3);
    auto r = eo.oldest_pending();
    REQUIRE(r);
    CHECK(r.value().token_id == t1.value().token_id);
}

TEST_CASE("emergency_override: oldest_pending returns not_found on empty registry") {
    EmergencyOverride eo;
    auto r = eo.oldest_pending();
    REQUIRE(!r);
    CHECK(r.error().code() == ErrorCode::not_found);
}

TEST_CASE("emergency_override: oldest_pending returns not_found when all are backfilled") {
    EmergencyOverride eo;
    auto r1 = eo.activate(ActorId::clinician("c1"),
                          PatientId::pseudonymous("p1"), "r1");
    auto r2 = eo.activate(ActorId::clinician("c2"),
                          PatientId::pseudonymous("p2"), "r2");
    REQUIRE(r1); REQUIRE(r2);
    REQUIRE(eo.backfill(r1.value().token_id, "ev-1"));
    REQUIRE(eo.backfill(r2.value().token_id, "ev-2"));
    auto r = eo.oldest_pending();
    REQUIRE(!r);
    CHECK(r.error().code() == ErrorCode::not_found);
}

TEST_CASE("emergency_override: oldest_pending advances to next pending after head is backfilled") {
    EmergencyOverride eo;
    auto t1 = eo.activate(ActorId::clinician("c1"),
                          PatientId::pseudonymous("p1"), "first");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    auto t2 = eo.activate(ActorId::clinician("c2"),
                          PatientId::pseudonymous("p2"), "second");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    auto t3 = eo.activate(ActorId::clinician("c3"),
                          PatientId::pseudonymous("p3"), "third");
    REQUIRE(t1); REQUIRE(t2); REQUIRE(t3);

    auto first = eo.oldest_pending();
    REQUIRE(first);
    CHECK(first.value().token_id == t1.value().token_id);

    REQUIRE(eo.backfill(t1.value().token_id, "ev-1"));
    auto second = eo.oldest_pending();
    REQUIRE(second);
    CHECK(second.value().token_id == t2.value().token_id);

    REQUIRE(eo.backfill(t2.value().token_id, "ev-2"));
    auto third = eo.oldest_pending();
    REQUIRE(third);
    CHECK(third.value().token_id == t3.value().token_id);
}

TEST_CASE("emergency_override: snapshot is sorted by activated_at ascending") {
    EmergencyOverride eo;
    auto r1 = eo.activate(ActorId::clinician("a"),
                          PatientId::pseudonymous("p1"), "first");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    auto r2 = eo.activate(ActorId::clinician("b"),
                          PatientId::pseudonymous("p2"), "second");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    auto r3 = eo.activate(ActorId::clinician("c"),
                          PatientId::pseudonymous("p3"), "third");
    REQUIRE(r1); REQUIRE(r2); REQUIRE(r3);

    // Backfilling the middle row must NOT change snapshot ordering —
    // snapshot is keyed on activated_at, not on completion state.
    REQUIRE(eo.backfill(r2.value().token_id, "ev-mid"));

    auto rows = eo.snapshot();
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].activated_at <= rows[1].activated_at);
    CHECK(rows[1].activated_at <= rows[2].activated_at);
    CHECK(rows[0].token_id == r1.value().token_id);
    CHECK(rows[1].token_id == r2.value().token_id);
    CHECK(rows[2].token_id == r3.value().token_id);
}
