// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
//
// FamilyGraph — multi-party consent edges for elder/minor relations.
//
// Exercises every method of asclepius::FamilyGraph + the free functions
// to_string(FamilyRelation) and family_relation_from_string. Each TEST_CASE
// is independent (constructs its own graph) so failures are localised.

#include <doctest/doctest.h>

#include "asclepius/consent.hpp"
#include "asclepius/identity.hpp"
#include "asclepius/core.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

using namespace asclepius;
using namespace std::chrono_literals;

// ---- 1. enum & string helpers --------------------------------------------

TEST_CASE("family_graph: to_string produces the documented snake_case spelling") {
    CHECK(std::string_view{to_string(FamilyRelation::adult_child_for_elder_parent)}
          == "adult_child_for_elder_parent");
    CHECK(std::string_view{to_string(FamilyRelation::parent_for_minor)}
          == "parent_for_minor");
    CHECK(std::string_view{to_string(FamilyRelation::legal_guardian_for_ward)}
          == "legal_guardian_for_ward");
    CHECK(std::string_view{to_string(FamilyRelation::spouse_for_spouse)}
          == "spouse_for_spouse");
}

TEST_CASE("family_graph: family_relation_from_string round-trips every value") {
    for (auto r : {FamilyRelation::adult_child_for_elder_parent,
                   FamilyRelation::parent_for_minor,
                   FamilyRelation::legal_guardian_for_ward,
                   FamilyRelation::spouse_for_spouse}) {
        auto round = family_relation_from_string(to_string(r));
        REQUIRE(round);
        CHECK(round.value() == r);
    }
}

TEST_CASE("family_graph: family_relation_from_string rejects unknown strings") {
    auto e1 = family_relation_from_string("");
    auto e2 = family_relation_from_string("ADULT_CHILD");
    auto e3 = family_relation_from_string("nonexistent");
    auto e4 = family_relation_from_string("adult_child_for_elder_parent ");  // trailing space
    CHECK(!e1);
    CHECK(!e2);
    CHECK(!e3);
    CHECK(!e4);
    CHECK(e1.error().code() == ErrorCode::invalid_argument);
    CHECK(e2.error().code() == ErrorCode::invalid_argument);
}

// ---- 2. construction & empty state ---------------------------------------

TEST_CASE("family_graph: default-constructed graph is empty") {
    FamilyGraph g;
    CHECK(g.total_relations()  == 0);
    CHECK(g.distinct_proxies() == 0);
    CHECK(g.distinct_subjects() == 0);
    CHECK(g.snapshot().empty());
    CHECK(g.counts_by_relation().empty());
}

// ---- 3. record_relation --------------------------------------------------

TEST_CASE("family_graph: record_relation inserts a single edge") {
    FamilyGraph g;
    auto proxy   = PatientId::pseudonymous("daughter1");
    auto subject = PatientId::pseudonymous("elder1");
    auto rec = g.record_relation(proxy, subject,
                                 FamilyRelation::adult_child_for_elder_parent);
    REQUIRE(rec);
    CHECK(g.total_relations() == 1);

    auto rel = g.relation_between(proxy, subject);
    REQUIRE(rel);
    CHECK(rel.value() == FamilyRelation::adult_child_for_elder_parent);
}

TEST_CASE("family_graph: record_relation fires observer with `recorded` event") {
    FamilyGraph g;
    int                            recorded = 0;
    int                            removed  = 0;
    PatientId                      seen_proxy{""};
    PatientId                      seen_subject{""};
    FamilyRelation                 seen_rel = FamilyRelation::parent_for_minor;
    g.set_observer([&](FamilyGraph::Event ev, const FamilyGraph::Edge& e) {
        if (ev == FamilyGraph::Event::recorded) recorded++;
        if (ev == FamilyGraph::Event::removed)  removed++;
        seen_proxy   = e.proxy;
        seen_subject = e.subject;
        seen_rel     = e.relation;
    });
    auto proxy   = PatientId::pseudonymous("p");
    auto subject = PatientId::pseudonymous("s");
    REQUIRE(g.record_relation(proxy, subject,
                              FamilyRelation::spouse_for_spouse));
    CHECK(recorded == 1);
    CHECK(removed  == 0);
    CHECK(seen_proxy   == proxy);
    CHECK(seen_subject == subject);
    CHECK(seen_rel == FamilyRelation::spouse_for_spouse);
}

TEST_CASE("family_graph: record_relation duplicate triple returns conflict & is no-op") {
    FamilyGraph g;
    auto proxy   = PatientId::pseudonymous("p");
    auto subject = PatientId::pseudonymous("s");
    REQUIRE(g.record_relation(proxy, subject,
                              FamilyRelation::parent_for_minor));
    auto again = g.record_relation(proxy, subject,
                                   FamilyRelation::parent_for_minor);
    CHECK(!again);
    CHECK(again.error().code() == ErrorCode::conflict);
    CHECK(g.total_relations() == 1);  // unchanged
}

TEST_CASE("family_graph: record_relation different relation updates in place") {
    FamilyGraph g;
    auto proxy   = PatientId::pseudonymous("p");
    auto subject = PatientId::pseudonymous("s");
    REQUIRE(g.record_relation(proxy, subject,
                              FamilyRelation::parent_for_minor));
    auto upd = g.record_relation(proxy, subject,
                                 FamilyRelation::legal_guardian_for_ward);
    REQUIRE(upd);
    CHECK(g.total_relations() == 1);  // updated, not appended
    auto rel = g.relation_between(proxy, subject);
    REQUIRE(rel);
    CHECK(rel.value() == FamilyRelation::legal_guardian_for_ward);
}

TEST_CASE("family_graph: record_relation update fires observer with new relation") {
    FamilyGraph g;
    int            recorded = 0;
    FamilyRelation seen     = FamilyRelation::parent_for_minor;
    g.set_observer([&](FamilyGraph::Event ev, const FamilyGraph::Edge& e) {
        if (ev == FamilyGraph::Event::recorded) recorded++;
        seen = e.relation;
    });
    auto proxy   = PatientId::pseudonymous("p");
    auto subject = PatientId::pseudonymous("s");
    REQUIRE(g.record_relation(proxy, subject,
                              FamilyRelation::parent_for_minor));
    REQUIRE(g.record_relation(proxy, subject,
                              FamilyRelation::adult_child_for_elder_parent));
    CHECK(recorded == 2);  // both insert + update fired
    CHECK(seen == FamilyRelation::adult_child_for_elder_parent);
}

TEST_CASE("family_graph: record_relation conflict does NOT fire observer") {
    FamilyGraph g;
    int recorded = 0;
    g.set_observer([&](FamilyGraph::Event ev, const FamilyGraph::Edge&) {
        if (ev == FamilyGraph::Event::recorded) recorded++;
    });
    auto proxy   = PatientId::pseudonymous("p");
    auto subject = PatientId::pseudonymous("s");
    REQUIRE(g.record_relation(proxy, subject,
                              FamilyRelation::parent_for_minor));
    REQUIRE(recorded == 1);
    auto dup = g.record_relation(proxy, subject,
                                 FamilyRelation::parent_for_minor);
    CHECK(!dup);
    CHECK(recorded == 1);  // no extra fire on conflict
}

// ---- 4. remove_relation --------------------------------------------------

TEST_CASE("family_graph: remove_relation drops the edge") {
    FamilyGraph g;
    auto proxy   = PatientId::pseudonymous("p");
    auto subject = PatientId::pseudonymous("s");
    REQUIRE(g.record_relation(proxy, subject,
                              FamilyRelation::spouse_for_spouse));
    REQUIRE(g.remove_relation(proxy, subject));
    CHECK(g.total_relations() == 0);
    CHECK(!g.can_consent_for(proxy, subject));
}

TEST_CASE("family_graph: remove_relation fires observer with `removed` event") {
    FamilyGraph g;
    int            recorded = 0;
    int            removed  = 0;
    FamilyRelation seen     = FamilyRelation::parent_for_minor;
    g.set_observer([&](FamilyGraph::Event ev, const FamilyGraph::Edge& e) {
        if (ev == FamilyGraph::Event::recorded) recorded++;
        if (ev == FamilyGraph::Event::removed)  removed++;
        if (ev == FamilyGraph::Event::removed)  seen = e.relation;
    });
    auto proxy   = PatientId::pseudonymous("p");
    auto subject = PatientId::pseudonymous("s");
    REQUIRE(g.record_relation(proxy, subject,
                              FamilyRelation::legal_guardian_for_ward));
    REQUIRE(g.remove_relation(proxy, subject));
    CHECK(recorded == 1);
    CHECK(removed  == 1);
    CHECK(seen == FamilyRelation::legal_guardian_for_ward);
}

TEST_CASE("family_graph: remove_relation on missing edge returns not_found") {
    FamilyGraph g;
    auto proxy   = PatientId::pseudonymous("p");
    auto subject = PatientId::pseudonymous("s");
    auto r = g.remove_relation(proxy, subject);
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::not_found);
}

TEST_CASE("family_graph: remove_relation on missing edge does NOT fire observer") {
    FamilyGraph g;
    int events = 0;
    g.set_observer([&](FamilyGraph::Event, const FamilyGraph::Edge&) { events++; });
    auto proxy   = PatientId::pseudonymous("p");
    auto subject = PatientId::pseudonymous("s");
    auto r = g.remove_relation(proxy, subject);
    CHECK(!r);
    CHECK(events == 0);
}

// ---- 5. can_consent_for --------------------------------------------------

TEST_CASE("family_graph: can_consent_for true for an inserted edge, else false") {
    FamilyGraph g;
    auto proxy    = PatientId::pseudonymous("guardian");
    auto subject  = PatientId::pseudonymous("ward");
    auto stranger = PatientId::pseudonymous("stranger");
    REQUIRE(g.record_relation(proxy, subject,
                              FamilyRelation::legal_guardian_for_ward));
    CHECK(g.can_consent_for(proxy,    subject));
    CHECK(!g.can_consent_for(stranger, subject));
    CHECK(!g.can_consent_for(proxy,    stranger));
    CHECK(!g.can_consent_for(subject,  proxy));  // direction is significant
}

TEST_CASE("family_graph: can_consent_for is noexcept") {
    FamilyGraph     g;
    const PatientId p = PatientId::pseudonymous("p");
    const PatientId s = PatientId::pseudonymous("s");
    static_assert(noexcept(g.can_consent_for(p, s)),
                  "can_consent_for must be noexcept");
    CHECK(!g.can_consent_for(p, s));
}

// ---- 6. subjects_for_proxy / proxies_for_subject -------------------------

TEST_CASE("family_graph: subjects_for_proxy returns sorted distinct subjects") {
    FamilyGraph g;
    auto proxy   = PatientId::pseudonymous("caregiver");
    auto s1 = PatientId::pseudonymous("zeta");
    auto s2 = PatientId::pseudonymous("alpha");
    auto s3 = PatientId::pseudonymous("mu");
    REQUIRE(g.record_relation(proxy, s1,
                              FamilyRelation::adult_child_for_elder_parent));
    REQUIRE(g.record_relation(proxy, s2,
                              FamilyRelation::parent_for_minor));
    REQUIRE(g.record_relation(proxy, s3,
                              FamilyRelation::legal_guardian_for_ward));

    auto out = g.subjects_for_proxy(proxy);
    REQUIRE(out.size() == 3);
    CHECK(out[0].str() < out[1].str());
    CHECK(out[1].str() < out[2].str());

    std::vector<std::string> bodies;
    for (const auto& p : out) bodies.emplace_back(p.str());
    std::vector<std::string> expected = {std::string{s1.str()},
                                         std::string{s2.str()},
                                         std::string{s3.str()}};
    std::sort(expected.begin(), expected.end());
    CHECK(bodies == expected);
}

TEST_CASE("family_graph: subjects_for_proxy returns empty vector for unknown proxy") {
    FamilyGraph g;
    auto proxy   = PatientId::pseudonymous("p");
    auto subject = PatientId::pseudonymous("s");
    REQUIRE(g.record_relation(proxy, subject,
                              FamilyRelation::parent_for_minor));
    auto orphan = PatientId::pseudonymous("orphan");
    CHECK(g.subjects_for_proxy(orphan).empty());
}

TEST_CASE("family_graph: proxies_for_subject returns sorted distinct proxies") {
    FamilyGraph g;
    auto subject = PatientId::pseudonymous("elder");
    auto p1 = PatientId::pseudonymous("zoe");
    auto p2 = PatientId::pseudonymous("alex");
    auto p3 = PatientId::pseudonymous("morgan");
    REQUIRE(g.record_relation(p1, subject,
                              FamilyRelation::adult_child_for_elder_parent));
    REQUIRE(g.record_relation(p2, subject,
                              FamilyRelation::adult_child_for_elder_parent));
    REQUIRE(g.record_relation(p3, subject,
                              FamilyRelation::spouse_for_spouse));

    auto out = g.proxies_for_subject(subject);
    REQUIRE(out.size() == 3);
    CHECK(out[0].str() < out[1].str());
    CHECK(out[1].str() < out[2].str());
}

TEST_CASE("family_graph: proxies_for_subject returns empty vector for unknown subject") {
    FamilyGraph g;
    auto proxy   = PatientId::pseudonymous("p");
    auto subject = PatientId::pseudonymous("s");
    REQUIRE(g.record_relation(proxy, subject,
                              FamilyRelation::parent_for_minor));
    auto orphan = PatientId::pseudonymous("orphan");
    CHECK(g.proxies_for_subject(orphan).empty());
}

// ---- 7. relation_between -------------------------------------------------

TEST_CASE("family_graph: relation_between returns the stored relation") {
    FamilyGraph g;
    auto proxy   = PatientId::pseudonymous("parent");
    auto subject = PatientId::pseudonymous("child");
    REQUIRE(g.record_relation(proxy, subject,
                              FamilyRelation::parent_for_minor));
    auto r = g.relation_between(proxy, subject);
    REQUIRE(r);
    CHECK(r.value() == FamilyRelation::parent_for_minor);
}

TEST_CASE("family_graph: relation_between returns not_found when absent") {
    FamilyGraph g;
    auto proxy   = PatientId::pseudonymous("p");
    auto subject = PatientId::pseudonymous("s");
    auto r = g.relation_between(proxy, subject);
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::not_found);
}

// ---- 8. counts -----------------------------------------------------------

TEST_CASE("family_graph: distinct_proxies / distinct_subjects / total_relations track ops") {
    FamilyGraph g;
    auto pa = PatientId::pseudonymous("pa");
    auto pb = PatientId::pseudonymous("pb");
    auto sa = PatientId::pseudonymous("sa");
    auto sb = PatientId::pseudonymous("sb");
    auto sc = PatientId::pseudonymous("sc");
    REQUIRE(g.record_relation(pa, sa,
                              FamilyRelation::adult_child_for_elder_parent));
    REQUIRE(g.record_relation(pa, sb,
                              FamilyRelation::adult_child_for_elder_parent));
    REQUIRE(g.record_relation(pb, sa,
                              FamilyRelation::adult_child_for_elder_parent));
    REQUIRE(g.record_relation(pb, sc,
                              FamilyRelation::parent_for_minor));
    CHECK(g.total_relations()   == 4);
    CHECK(g.distinct_proxies()  == 2);
    CHECK(g.distinct_subjects() == 3);

    REQUIRE(g.remove_relation(pb, sc));
    CHECK(g.total_relations()   == 3);
    CHECK(g.distinct_proxies()  == 2);  // pb still appears as proxy of sa
    CHECK(g.distinct_subjects() == 2);  // sc gone
}

TEST_CASE("family_graph: total/distinct counts are noexcept") {
    FamilyGraph g;
    static_assert(noexcept(g.total_relations()),   "total_relations must be noexcept");
    static_assert(noexcept(g.distinct_proxies()),  "distinct_proxies must be noexcept");
    static_assert(noexcept(g.distinct_subjects()), "distinct_subjects must be noexcept");
    CHECK(g.total_relations()   == 0);
    CHECK(g.distinct_proxies()  == 0);
    CHECK(g.distinct_subjects() == 0);
}

TEST_CASE("family_graph: counts_by_relation reports only present relations") {
    FamilyGraph g;
    auto pa = PatientId::pseudonymous("pa");
    auto pb = PatientId::pseudonymous("pb");
    auto sa = PatientId::pseudonymous("sa");
    auto sb = PatientId::pseudonymous("sb");
    auto sc = PatientId::pseudonymous("sc");
    REQUIRE(g.record_relation(pa, sa,
                              FamilyRelation::adult_child_for_elder_parent));
    REQUIRE(g.record_relation(pa, sb,
                              FamilyRelation::adult_child_for_elder_parent));
    REQUIRE(g.record_relation(pb, sc,
                              FamilyRelation::parent_for_minor));

    auto counts = g.counts_by_relation();
    CHECK(counts.size() == 2);
    CHECK(counts[FamilyRelation::adult_child_for_elder_parent] == 2);
    CHECK(counts[FamilyRelation::parent_for_minor]             == 1);
    // Zero-count relations are absent.
    CHECK(counts.find(FamilyRelation::legal_guardian_for_ward) == counts.end());
    CHECK(counts.find(FamilyRelation::spouse_for_spouse)       == counts.end());
}

TEST_CASE("family_graph: counts_by_relation on empty graph is empty map") {
    FamilyGraph g;
    auto counts = g.counts_by_relation();
    CHECK(counts.empty());
}

// ---- 9. snapshot ---------------------------------------------------------

TEST_CASE("family_graph: snapshot order is lexicographic by (proxy, subject)") {
    FamilyGraph g;
    // Insert in deliberately non-sorted order to verify the sort.
    auto pz = PatientId::pseudonymous("zz_proxy");
    auto pa = PatientId::pseudonymous("aa_proxy");
    auto pm = PatientId::pseudonymous("mm_proxy");
    auto sx = PatientId::pseudonymous("xx_subj");
    auto sy = PatientId::pseudonymous("yy_subj");
    REQUIRE(g.record_relation(pz, sy,
                              FamilyRelation::adult_child_for_elder_parent));
    REQUIRE(g.record_relation(pa, sx,
                              FamilyRelation::parent_for_minor));
    REQUIRE(g.record_relation(pm, sy,
                              FamilyRelation::spouse_for_spouse));
    REQUIRE(g.record_relation(pa, sy,
                              FamilyRelation::legal_guardian_for_ward));

    auto snap = g.snapshot();
    REQUIRE(snap.size() == 4);
    for (std::size_t i = 1; i < snap.size(); ++i) {
        const auto& a = snap[i - 1];
        const auto& b = snap[i];
        bool in_order =
            (a.proxy.str() < b.proxy.str()) ||
            (a.proxy.str() == b.proxy.str() &&
             a.subject.str() < b.subject.str());
        CHECK(in_order);
    }
}

TEST_CASE("family_graph: snapshot edges carry recorded_at populated by record_relation") {
    FamilyGraph g;
    auto before = Time::now();
    auto proxy   = PatientId::pseudonymous("p");
    auto subject = PatientId::pseudonymous("s");
    REQUIRE(g.record_relation(proxy, subject,
                              FamilyRelation::parent_for_minor));
    auto after = Time::now() + 1s;
    auto snap  = g.snapshot();
    REQUIRE(snap.size() == 1);
    // recorded_at is sometime around the call; check it falls in a sane window.
    CHECK(snap[0].recorded_at >= before - 1s);
    CHECK(snap[0].recorded_at <= after);
}

// ---- 10. ingest ----------------------------------------------------------

TEST_CASE("family_graph: ingest restores edges verbatim (preserves recorded_at)") {
    FamilyGraph g;
    auto pa = PatientId::pseudonymous("alpha");
    auto pb = PatientId::pseudonymous("beta");
    auto sa = PatientId::pseudonymous("gamma");
    auto sb = PatientId::pseudonymous("delta");
    REQUIRE(g.record_relation(pa, sa,
                              FamilyRelation::parent_for_minor));
    REQUIRE(g.record_relation(pa, sb,
                              FamilyRelation::adult_child_for_elder_parent));
    REQUIRE(g.record_relation(pb, sa,
                              FamilyRelation::spouse_for_spouse));

    auto original = g.snapshot();
    REQUIRE(original.size() == 3);
    g.clear();
    REQUIRE(g.total_relations() == 0);

    for (const auto& e : original) REQUIRE(g.ingest(e));

    auto rebuilt = g.snapshot();
    REQUIRE(rebuilt.size() == original.size());
    for (std::size_t i = 0; i < rebuilt.size(); ++i) {
        CHECK(rebuilt[i].proxy       == original[i].proxy);
        CHECK(rebuilt[i].subject     == original[i].subject);
        CHECK(rebuilt[i].relation    == original[i].relation);
        CHECK(rebuilt[i].recorded_at == original[i].recorded_at);
    }
}

TEST_CASE("family_graph: ingest does NOT fire the observer (ledger is source of truth)") {
    FamilyGraph g;
    int events = 0;
    g.set_observer([&](FamilyGraph::Event, const FamilyGraph::Edge&) { events++; });
    FamilyGraph::Edge e;
    e.proxy       = PatientId::pseudonymous("p");
    e.subject     = PatientId::pseudonymous("s");
    e.relation    = FamilyRelation::parent_for_minor;
    e.recorded_at = Time::now();
    REQUIRE(g.ingest(e));
    CHECK(events == 0);
    CHECK(g.total_relations() == 1);
}

TEST_CASE("family_graph: ingest rejects exact duplicate edges with conflict") {
    FamilyGraph g;
    FamilyGraph::Edge e;
    e.proxy       = PatientId::pseudonymous("p");
    e.subject     = PatientId::pseudonymous("s");
    e.relation    = FamilyRelation::parent_for_minor;
    e.recorded_at = Time::now();
    REQUIRE(g.ingest(e));
    auto dup = g.ingest(e);
    CHECK(!dup);
    CHECK(dup.error().code() == ErrorCode::conflict);
    CHECK(g.total_relations() == 1);
}

TEST_CASE("family_graph: ingest with different relation silently updates (no observer)") {
    FamilyGraph g;
    int events = 0;
    g.set_observer([&](FamilyGraph::Event, const FamilyGraph::Edge&) { events++; });
    FamilyGraph::Edge e;
    e.proxy       = PatientId::pseudonymous("p");
    e.subject     = PatientId::pseudonymous("s");
    e.relation    = FamilyRelation::parent_for_minor;
    e.recorded_at = Time::now();
    REQUIRE(g.ingest(e));
    e.relation = FamilyRelation::legal_guardian_for_ward;
    auto upd = g.ingest(e);
    REQUIRE(upd);
    CHECK(g.total_relations() == 1);
    auto rel = g.relation_between(e.proxy, e.subject);
    REQUIRE(rel);
    CHECK(rel.value() == FamilyRelation::legal_guardian_for_ward);
    CHECK(events == 0);  // ingest is silent
}

// ---- 11. clear -----------------------------------------------------------

TEST_CASE("family_graph: clear drops every edge and zeroes the counters") {
    FamilyGraph g;
    auto pa = PatientId::pseudonymous("a");
    auto sa = PatientId::pseudonymous("b");
    auto sb = PatientId::pseudonymous("c");
    REQUIRE(g.record_relation(pa, sa,
                              FamilyRelation::parent_for_minor));
    REQUIRE(g.record_relation(pa, sb,
                              FamilyRelation::parent_for_minor));
    CHECK(g.total_relations() == 2);
    g.clear();
    CHECK(g.total_relations()   == 0);
    CHECK(g.distinct_proxies()  == 0);
    CHECK(g.distinct_subjects() == 0);
    CHECK(g.snapshot().empty());
    CHECK(g.counts_by_relation().empty());
}

TEST_CASE("family_graph: clear does NOT fire the observer") {
    FamilyGraph g;
    int events = 0;
    g.set_observer([&](FamilyGraph::Event, const FamilyGraph::Edge&) { events++; });
    auto proxy   = PatientId::pseudonymous("p");
    auto subject = PatientId::pseudonymous("s");
    REQUIRE(g.record_relation(proxy, subject,
                              FamilyRelation::parent_for_minor));
    REQUIRE(events == 1);  // record fired once
    g.clear();
    CHECK(events == 1);    // clear must NOT fire
    CHECK(g.total_relations() == 0);
}

// ---- 12. summary_string --------------------------------------------------

TEST_CASE("family_graph: summary_string format matches contract") {
    FamilyGraph g;
    CHECK(g.summary_string() == "edges=0 proxies=0 subjects=0");
    auto pa = PatientId::pseudonymous("pa");
    auto pb = PatientId::pseudonymous("pb");
    auto sa = PatientId::pseudonymous("sa");
    REQUIRE(g.record_relation(pa, sa,
                              FamilyRelation::parent_for_minor));
    REQUIRE(g.record_relation(pb, sa,
                              FamilyRelation::adult_child_for_elder_parent));
    CHECK(g.summary_string() == "edges=2 proxies=2 subjects=1");

    REQUIRE(g.remove_relation(pa, sa));
    CHECK(g.summary_string() == "edges=1 proxies=1 subjects=1");

    g.clear();
    CHECK(g.summary_string() == "edges=0 proxies=0 subjects=0");
}

// ---- 13. integration / scenario ------------------------------------------

TEST_CASE("family_graph: multi-proxy/multi-subject scenario keeps lookups consistent") {
    // Scenario:
    //   * `elder` has two adult-child proxies: `daughter` and `son`.
    //   * `adult` is themselves both
    //       - the child of `elder_parent` (adult → elder_parent)
    //       - the parent of `minor_child` (adult → minor_child)
    FamilyGraph g;
    auto elder        = PatientId::pseudonymous("elder");
    auto daughter     = PatientId::pseudonymous("daughter");
    auto son          = PatientId::pseudonymous("son");

    auto adult        = PatientId::pseudonymous("adult");
    auto elder_parent = PatientId::pseudonymous("elder_parent");
    auto minor_child  = PatientId::pseudonymous("minor_child");

    REQUIRE(g.record_relation(daughter, elder,
                              FamilyRelation::adult_child_for_elder_parent));
    REQUIRE(g.record_relation(son, elder,
                              FamilyRelation::adult_child_for_elder_parent));
    REQUIRE(g.record_relation(adult, elder_parent,
                              FamilyRelation::adult_child_for_elder_parent));
    REQUIRE(g.record_relation(adult, minor_child,
                              FamilyRelation::parent_for_minor));

    CHECK(g.total_relations()   == 4);
    CHECK(g.distinct_proxies()  == 3);  // daughter, son, adult
    CHECK(g.distinct_subjects() == 3);  // elder, elder_parent, minor_child

    auto elder_proxies = g.proxies_for_subject(elder);
    REQUIRE(elder_proxies.size() == 2);
    CHECK(elder_proxies[0].str() < elder_proxies[1].str());

    auto adult_subjects = g.subjects_for_proxy(adult);
    REQUIRE(adult_subjects.size() == 2);
    CHECK(adult_subjects[0].str() < adult_subjects[1].str());

    auto counts = g.counts_by_relation();
    CHECK(counts[FamilyRelation::adult_child_for_elder_parent] == 3);
    CHECK(counts[FamilyRelation::parent_for_minor]             == 1);
    CHECK(counts.find(FamilyRelation::legal_guardian_for_ward) == counts.end());
    CHECK(counts.find(FamilyRelation::spouse_for_spouse)       == counts.end());

    CHECK(g.can_consent_for(daughter, elder));
    CHECK(g.can_consent_for(son,      elder));
    CHECK(!g.can_consent_for(elder,   daughter));   // direction matters
    CHECK(g.can_consent_for(adult, elder_parent));
    CHECK(g.can_consent_for(adult, minor_child));
    CHECK(!g.can_consent_for(adult, elder));        // unrelated edge

    auto rel = g.relation_between(adult, minor_child);
    REQUIRE(rel);
    CHECK(rel.value() == FamilyRelation::parent_for_minor);

    CHECK(g.summary_string() == "edges=4 proxies=3 subjects=3");
}
