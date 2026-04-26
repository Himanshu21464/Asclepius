// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include <doctest/doctest.h>

#include "asclepius/audit.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <filesystem>
#include <fstream>
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

TEST_CASE("Ledger subscription fires on append") {
    auto p   = tmp_db("subs");
    auto led = Ledger::open(p);
    REQUIRE(led);
    auto& l  = led.value();

    std::vector<std::uint64_t> seen;
    {
        auto sub = l.subscribe([&](const LedgerEntry& e) {
            seen.push_back(e.header.seq);
        });
        nlohmann::json b;
        REQUIRE(l.append("t.a", "sys", b));
        REQUIRE(l.append("t.b", "sys", b));
        REQUIRE(l.append("t.c", "sys", b));
    }  // sub destructor unsubscribes

    REQUIRE(seen.size() == 3);
    CHECK(seen[0] == 1);
    CHECK(seen[1] == 2);
    CHECK(seen[2] == 3);

    // After unsubscribe, no further callbacks.
    nlohmann::json b;
    REQUIRE(l.append("t.d", "sys", b));
    CHECK(seen.size() == 3);  // unchanged
}

TEST_CASE("Multiple subscribers fire in registration order") {
    auto p   = tmp_db("subs_multi");
    auto led = Ledger::open(p);
    REQUIRE(led);
    auto& l  = led.value();

    std::vector<int> order;
    auto s1 = l.subscribe([&](const LedgerEntry&) { order.push_back(1); });
    auto s2 = l.subscribe([&](const LedgerEntry&) { order.push_back(2); });
    auto s3 = l.subscribe([&](const LedgerEntry&) { order.push_back(3); });

    nlohmann::json b;
    REQUIRE(l.append("t.x", "sys", b));
    CHECK(order == std::vector<int>{1, 2, 3});
}

TEST_CASE("Subscriber exception does not break the chain") {
    auto p   = tmp_db("subs_throw");
    auto led = Ledger::open(p);
    REQUIRE(led);
    auto& l  = led.value();

    bool called_after_thrower = false;
    auto s1 = l.subscribe([&](const LedgerEntry&) { throw std::runtime_error("boom"); });
    auto s2 = l.subscribe([&](const LedgerEntry&) { called_after_thrower = true; });

    nlohmann::json b;
    auto r = l.append("t.x", "sys", b);
    REQUIRE(r);                           // append succeeded despite throwing sub
    CHECK(called_after_thrower);          // later subs still run
    CHECK(l.length() == 1);
    REQUIRE(l.verify());                  // chain still intact
}

TEST_CASE("Subscription handle moves correctly") {
    auto p   = tmp_db("subs_move");
    auto led = Ledger::open(p);
    REQUIRE(led);
    auto& l  = led.value();

    int hits = 0;
    Ledger::Subscription holder;
    {
        auto local = l.subscribe([&](const LedgerEntry&) { ++hits; });
        holder = std::move(local);  // move out — local should not unsubscribe
    }
    nlohmann::json b;
    REQUIRE(l.append("t.x", "sys", b));
    CHECK(hits == 1);  // holder still subscribed
}

TEST_CASE("Ledger checkpoint signs the head and verifies offline") {
    auto p   = tmp_db("checkpoint");
    auto led = Ledger::open(p);
    REQUIRE(led);
    nlohmann::json b;
    REQUIRE(led.value().append("t.x", "sys", b));
    REQUIRE(led.value().append("t.y", "sys", b));
    REQUIRE(led.value().append("t.z", "sys", b));

    auto cp = led.value().checkpoint();
    CHECK(cp.seq == 3);
    CHECK(cp.head_hash == led.value().head());
    CHECK(cp.key_id == led.value().key_id());

    // verify_checkpoint requires no ledger access — pure crypto.
    auto v = verify_checkpoint(cp);
    REQUIRE(v);
}

TEST_CASE("Checkpoint round-trips through JSON") {
    auto p   = tmp_db("cp_json");
    auto led = Ledger::open(p);
    REQUIRE(led);
    nlohmann::json b;
    REQUIRE(led.value().append("t.a", "sys", b));

    auto cp = led.value().checkpoint();
    auto serialized = cp.to_json();
    auto parsed     = LedgerCheckpoint::from_json(serialized);
    REQUIRE(parsed);
    CHECK(parsed.value().seq == cp.seq);
    CHECK(parsed.value().head_hash == cp.head_hash);
    CHECK(parsed.value().key_id == cp.key_id);
    CHECK(parsed.value().signature == cp.signature);
    CHECK(parsed.value().public_key == cp.public_key);
    REQUIRE(verify_checkpoint(parsed.value()));
}

TEST_CASE("Checkpoint with tampered head_hash fails verify") {
    auto p   = tmp_db("cp_tamper");
    auto led = Ledger::open(p);
    REQUIRE(led);
    nlohmann::json b;
    REQUIRE(led.value().append("t.a", "sys", b));

    auto cp = led.value().checkpoint();
    REQUIRE(verify_checkpoint(cp));

    // Flip a byte in head_hash — signature no longer matches.
    cp.head_hash.bytes[0] ^= 0x42;
    auto v = verify_checkpoint(cp);
    CHECK(!v);
}

TEST_CASE("Checkpoint with tampered signature fails verify") {
    auto p   = tmp_db("cp_sig");
    auto led = Ledger::open(p);
    REQUIRE(led);
    nlohmann::json b;
    REQUIRE(led.value().append("t.a", "sys", b));

    auto cp = led.value().checkpoint();
    cp.signature[0] ^= 0xFF;
    auto v = verify_checkpoint(cp);
    CHECK(!v);
}

TEST_CASE("JSONL export + import round-trips a chain") {
    auto src_path = tmp_db("jsonl_src");
    auto dst_path = tmp_db("jsonl_dst");

    // Build a 5-entry chain in src.
    {
        auto led = Ledger::open(src_path);
        REQUIRE(led);
        nlohmann::json b;
        for (int i = 0; i < 5; ++i) {
            REQUIRE(led.value().append("t.x", "sys", b, i % 2 ? "alpha" : ""));
        }
        REQUIRE(led.value().verify());
    }

    // Export to JSONL.
    auto jsonl = std::filesystem::temp_directory_path()
               / ("asc_jsonl_" + std::to_string(std::random_device{}()) + ".jsonl");
    auto exp = LedgerJsonl::export_to(src_path.string(), jsonl.string());
    REQUIRE(exp);
    CHECK(exp.value().entries_written == 5);
    CHECK(std::filesystem::exists(jsonl));
    CHECK(std::filesystem::file_size(jsonl) > 0);

    // Read the source's signing key — destination needs the same one to verify.
    auto src_key = src_path; src_key.replace_extension(".key");
    std::ifstream kf(src_key);
    std::string blob((std::istreambuf_iterator<char>(kf)), {});
    auto key = KeyStore::deserialize(blob);
    REQUIRE(key);

    // Import into a fresh dst ledger.
    {
        auto led = Ledger::open(dst_path, KeyStore::deserialize(blob).value());
        REQUIRE(led);
    }
    auto imp = LedgerJsonl::import_to(jsonl.string(), dst_path.string(), std::move(key.value()));
    REQUIRE(imp);
    CHECK(imp.value().entries_imported == 5);
    CHECK(imp.value().dest_head.bytes == exp.value().last_entry_hash.bytes);

    // Verify the destination chain with the source's key.
    auto k2 = KeyStore::deserialize(blob);
    REQUIRE(k2);
    auto led2 = Ledger::open(dst_path, std::move(k2.value()));
    REQUIRE(led2);
    REQUIRE(led2.value().verify());
    CHECK(led2.value().length() == 5);

    std::filesystem::remove(jsonl);
}

TEST_CASE("JSONL import refuses non-empty destinations") {
    auto src = tmp_db("jsonl_src2");
    {
        auto led = Ledger::open(src);
        REQUIRE(led);
        nlohmann::json b;
        REQUIRE(led.value().append("t.x", "sys", b));
    }
    auto jsonl = std::filesystem::temp_directory_path()
               / ("asc_jsonl_" + std::to_string(std::random_device{}()) + ".jsonl");
    REQUIRE(LedgerJsonl::export_to(src.string(), jsonl.string()));

    auto src_key = src; src_key.replace_extension(".key");
    std::ifstream kf(src_key);
    std::string blob((std::istreambuf_iterator<char>(kf)), {});
    auto key = KeyStore::deserialize(blob);
    REQUIRE(key);

    auto dst = tmp_db("jsonl_dst2");
    {
        auto led = Ledger::open(dst);
        REQUIRE(led);
        nlohmann::json b;
        REQUIRE(led.value().append("t.x", "sys", b));  // non-empty dst
    }
    auto imp = LedgerJsonl::import_to(jsonl.string(), dst.string(), std::move(key.value()));
    CHECK(!imp);  // refuses
    std::filesystem::remove(jsonl);
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
