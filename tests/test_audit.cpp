// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include <doctest/doctest.h>

#include "asclepius/audit.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <filesystem>
#include <random>

using namespace asclepius;

namespace {

std::filesystem::path tmp_db(const char* tag) {
    auto p = std::filesystem::temp_directory_path()
           / ("asclepius_test_" + std::string{tag} + "_"
              + std::to_string(std::random_device{}()) + ".db");
    std::filesystem::remove(p);
    std::filesystem::remove(std::filesystem::path{p}.replace_extension(".key"));
    return p;
}

}  // namespace

TEST_CASE("Ledger appends, then verify() succeeds") {
    auto p   = tmp_db("append");
    auto led = Ledger::open(p);
    REQUIRE(led);
    auto& l  = led.value();

    nlohmann::json a; a["msg"] = "first";
    auto e1 = l.append("test.first",  "system:test", a);
    REQUIRE(e1);
    nlohmann::json b; b["msg"] = "second";
    auto e2 = l.append("test.second", "system:test", b);
    REQUIRE(e2);

    CHECK(l.length() == 2);
    CHECK(l.head() == e2.value().entry_hash());

    auto v = l.verify();
    REQUIRE(v);
}

TEST_CASE("Ledger verify() detects tampered body") {
    auto p   = tmp_db("tamper_body");
    {
        auto led = Ledger::open(p);
        REQUIRE(led);
        auto& l  = led.value();
        nlohmann::json a; a["msg"] = "ok";
        REQUIRE(l.append("e", "actor", a));
    }

    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open(p.string().c_str(), &db) == SQLITE_OK);
    REQUIRE(sqlite3_exec(db,
                          "UPDATE asclepius_ledger SET body = '{\"msg\":\"tampered\"}' WHERE seq = 1;",
                          nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);

    auto led = Ledger::open(p);
    REQUIRE(led);
    auto v = led.value().verify();
    REQUIRE(!v);
    CHECK(v.error().code() == ErrorCode::integrity_failure);
}

TEST_CASE("Ledger verify() detects tampered prev_hash") {
    auto p = tmp_db("tamper_prev");
    {
        auto led = Ledger::open(p);
        REQUIRE(led);
        auto& l  = led.value();
        nlohmann::json a; a["x"] = 1;
        nlohmann::json b; b["x"] = 2;
        REQUIRE(l.append("e", "actor", a));
        REQUIRE(l.append("e", "actor", b));
    }

    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open(p.string().c_str(), &db) == SQLITE_OK);
    // Zero out the prev_hash on entry 2.
    REQUIRE(sqlite3_exec(db,
                          "UPDATE asclepius_ledger SET prev_hash = zeroblob(32) WHERE seq = 2;",
                          nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);

    auto led = Ledger::open(p);
    REQUIRE(led);
    auto v = led.value().verify();
    REQUIRE(!v);
    CHECK(v.error().code() == ErrorCode::integrity_failure);
}

TEST_CASE("Ledger persists across re-open") {
    auto p = tmp_db("persist");
    {
        auto led = Ledger::open(p);
        REQUIRE(led);
        auto& l = led.value();
        for (int i = 0; i < 10; ++i) {
            nlohmann::json b; b["i"] = i;
            REQUIRE(l.append("e", "actor", b));
        }
    }
    auto led2 = Ledger::open(p);
    REQUIRE(led2);
    auto& l2 = led2.value();
    CHECK(l2.length() == 10);
    REQUIRE(l2.verify());
}

TEST_CASE("KeyStore signs and verifies") {
    auto k   = KeyStore::generate();
    auto pk  = k.public_key();

    std::string msg = "audit-grade";
    auto sig = k.sign(Bytes{reinterpret_cast<const std::uint8_t*>(msg.data()), msg.size()});

    CHECK(KeyStore::verify(
        Bytes{reinterpret_cast<const std::uint8_t*>(msg.data()), msg.size()},
        std::span<const std::uint8_t, KeyStore::sig_bytes>{sig.data(), sig.size()},
        std::span<const std::uint8_t, KeyStore::pk_bytes>{pk.data(),  pk.size()}));

    // Tweak the message — verification fails.
    std::string bad = "audit-graded";
    CHECK(!KeyStore::verify(
        Bytes{reinterpret_cast<const std::uint8_t*>(bad.data()), bad.size()},
        std::span<const std::uint8_t, KeyStore::sig_bytes>{sig.data(), sig.size()},
        std::span<const std::uint8_t, KeyStore::pk_bytes>{pk.data(),  pk.size()}));
}

TEST_CASE("KeyStore serialize/deserialize round-trips") {
    auto k   = KeyStore::generate();
    auto enc = k.serialize();
    auto k2  = KeyStore::deserialize(enc);
    REQUIRE(k2);
    CHECK(k2.value().key_id() == k.key_id());
}

TEST_CASE("range_by_time covers entries within a window") {
    auto p   = tmp_db("time_range");
    auto led = Ledger::open(p);
    REQUIRE(led);
    auto& l  = led.value();

    Time t0 = Time::now() - std::chrono::seconds{1};
    nlohmann::json b;
    REQUIRE(l.append("t.a", "sys", b));
    REQUIRE(l.append("t.b", "sys", b));
    REQUIRE(l.append("t.c", "sys", b));
    Time t1 = Time::now() + std::chrono::seconds{1};

    auto in_window = l.range_by_time(t0, t1);
    REQUIRE(in_window);
    CHECK(in_window.value().size() == 3);

    auto future = l.range_by_time(t1 + std::chrono::hours{1}, t1 + std::chrono::hours{2});
    REQUIRE(future);
    CHECK(future.value().empty());
}

TEST_CASE("Tenant-scoped reads isolate per-tenant data") {
    auto p   = tmp_db("tenant_iso");
    auto led = Ledger::open(p);
    REQUIRE(led);
    auto& l  = led.value();

    nlohmann::json b;
    REQUIRE(l.append("t.a", "sys", b, "alpha"));
    REQUIRE(l.append("t.b", "sys", b, "alpha"));
    REQUIRE(l.append("t.c", "sys", b, "beta"));
    REQUIRE(l.append("t.d", "sys", b, ""));   // default tenant

    CHECK(l.length() == 4);

    auto alpha = l.tail_for_tenant("alpha", 100);
    REQUIRE(alpha);
    CHECK(alpha.value().size() == 2);
    for (const auto& e : alpha.value()) CHECK(e.header.tenant == "alpha");

    auto beta = l.tail_for_tenant("beta", 100);
    REQUIRE(beta);
    CHECK(beta.value().size() == 1);
    CHECK(beta.value()[0].header.tenant == "beta");

    auto def = l.tail_for_tenant("", 100);
    REQUIRE(def);
    CHECK(def.value().size() == 1);
    CHECK(def.value()[0].header.tenant == "");

    auto missing = l.tail_for_tenant("gamma", 100);
    REQUIRE(missing);
    CHECK(missing.value().empty());

    // range_for_tenant filters too.
    auto alpha_range = l.range_for_tenant("alpha", 1, 5);
    REQUIRE(alpha_range);
    CHECK(alpha_range.value().size() == 2);
    for (const auto& e : alpha_range.value()) CHECK(e.header.tenant == "alpha");
}
