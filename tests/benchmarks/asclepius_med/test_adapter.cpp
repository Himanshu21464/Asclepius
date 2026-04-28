// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
//
// L2-Medical conformance: four assertions against the offline fixture.
// See ../README.md and /site/conformance.html#l2-medical.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "adapter.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <random>
#include <set>
#include <string>

using namespace asclepius;
using namespace asclepius::bench::asclepius_med;
using namespace std::chrono_literals;

namespace {

std::filesystem::path tmp_db(const char* tag) {
    auto p = std::filesystem::temp_directory_path()
           / ("asc_l2m_" + std::string{tag} + "_"
              + std::to_string(std::random_device{}()) + ".db");
    std::filesystem::remove(p);
    std::filesystem::remove(std::filesystem::path{p}.replace_extension(".key"));
    return p;
}

std::string fixture_path() {
    // Build directory contains a copy of fixture.json next to the test
    // executable (see CMakeLists.txt).
    return (std::filesystem::path{ASCLEPIUS_BENCH_FIXTURE_DIR}
            / "fixture.json").string();
}

Result<std::string> echo_model(std::string s) {
    // A trivial deterministic model. The substrate doesn't care what
    // the model produces — only that one inference happens per item.
    return std::string{"answer: "} + s.substr(0, 32);
}

struct Harness {
    Runtime                rt;
    PatientId              patient;
    ActorId                actor;
    TenantId               tenant;
    ModelId                model;
    ConsentToken           token;
    std::vector<BenchItem> items;

    static Harness build() {
        auto rt_r = Runtime::open(tmp_db("h"));
        REQUIRE(rt_r);

        auto items_r = load_fixture(fixture_path());
        REQUIRE(items_r);
        REQUIRE(items_r.value().size() == 5);

        auto pid = PatientId::pseudonymous("p_l2m_demo");
        auto tok = rt_r.value().consent().grant(
            pid, {Purpose::diagnostic_suggestion}, 1h);
        REQUIRE(tok);

        return Harness{
            std::move(rt_r.value()),
            pid,
            ActorId::clinician("dr.kwon"),
            TenantId{"demo"},
            ModelId{"l2m-test-model", "v0"},
            tok.value(),
            std::move(items_r.value()),
        };
    }

    void drive_all() {
        for (auto& it : items) {
            auto r = drive_one(rt, it, token, patient, actor, tenant, model,
                               echo_model);
            REQUIRE(r);
        }
    }
};

// Walk the ledger and return only the inference.committed entries
// produced by the adapter (skips the consent.* preamble entries that
// Runtime::open / consent().grant() emit).
std::vector<LedgerEntry> bench_entries(const Ledger& l) {
    std::vector<LedgerEntry> out;
    for (std::uint64_t s = 1; s <= l.length(); ++s) {
        auto e = l.at(s);
        REQUIRE(e);
        if (e.value().header.event_type == "inference.committed") {
            out.push_back(std::move(e.value()));
        }
    }
    return out;
}

}  // namespace

TEST_CASE("L2-Medical · medical.bench.dev-set-coverage") {
    auto h = Harness::build();
    h.drive_all();

    auto rows = bench_entries(h.rt.ledger());
    CHECK(rows.size() == h.items.size());  // exactly one per item

    // No duplicate bench_item_id in the chain.
    std::set<std::string> seen;
    for (auto& e : rows) {
        auto body = nlohmann::json::parse(e.body_json);
        REQUIRE(body.contains("metadata"));
        REQUIRE(body["metadata"].contains("bench_item_id"));
        auto id = body["metadata"]["bench_item_id"].get<std::string>();
        CHECK(seen.insert(id).second);
    }
}

TEST_CASE("L2-Medical · medical.bench.purpose-bound") {
    auto h = Harness::build();
    h.drive_all();

    for (auto& e : bench_entries(h.rt.ledger())) {
        auto body = nlohmann::json::parse(e.body_json);
        REQUIRE(body.contains("purpose"));
        CHECK(body["purpose"].get<std::string>() == "diagnostic_suggestion");
    }
}

TEST_CASE("L2-Medical · medical.bench.specialty-tagged") {
    auto h = Harness::build();
    h.drive_all();

    auto rows = bench_entries(h.rt.ledger());
    REQUIRE(rows.size() == h.items.size());

    for (std::size_t i = 0; i < rows.size(); ++i) {
        auto body = nlohmann::json::parse(rows[i].body_json);
        REQUIRE(body.contains("metadata"));
        const auto& md = body["metadata"];
        REQUIRE(md.contains("specialty"));
        REQUIRE(md.contains("sub_task"));
        REQUIRE(md.contains("category"));
        REQUIRE(md.contains("benchmark"));
        CHECK(md["benchmark"].get<std::string>() == "asclepius-med");
        CHECK(md["specialty"].get<std::string>() == h.items[i].specialty);
        CHECK(md["sub_task"].get<std::string>()  == h.items[i].sub_task);
        CHECK(md["category"].get<std::string>()  == h.items[i].category);
    }
}

TEST_CASE("L2-Medical · medical.bench.bundle-reverifies") {
    auto h = Harness::build();
    h.drive_all();

    // The whole chain must verify against its public key — that's the
    // L1 floor that L2-Medical inherits. A real L2-Medical run would
    // also export a USTAR bundle and reverify offline; the bundle path
    // is exercised end-to-end in tests/test_evaluation.cpp under the
    // "Evidence bundle exports and verifies" case, so we verify the
    // chain here and rely on that case for bundle-export coverage.
    auto v = h.rt.ledger().verify();
    REQUIRE(v);
    CHECK(h.rt.ledger().length() >= h.items.size());
}
