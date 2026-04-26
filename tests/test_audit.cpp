// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include <doctest/doctest.h>

#include "asclepius/audit.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>

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

TEST_CASE("Ledger appends 100 entries and verifies clean") {
    auto p   = tmp_db("append_100");
    auto led = Ledger::open(p);
    REQUIRE(led);
    nlohmann::json b;
    for (int i = 0; i < 100; ++i) {
        b["i"] = i;
        REQUIRE(led.value().append("stress.t", "sys", b, ""));
    }
    CHECK(led.value().length() == 100);
    REQUIRE(led.value().verify());
}

TEST_CASE("Ledger seq is contiguous after 250 appends") {
    auto p   = tmp_db("seq_contig");
    auto led = Ledger::open(p);
    REQUIRE(led);
    nlohmann::json b;
    for (int i = 0; i < 250; ++i) REQUIRE(led.value().append("seq", "sys", b));
    auto all = led.value().range(1, 251);
    REQUIRE(all);
    REQUIRE(all.value().size() == 250);
    for (std::size_t i = 0; i < 250; ++i) {
        CHECK(all.value()[i].header.seq == i + 1);
    }
}

TEST_CASE("Ledger prev_hash chains correctly across 100 appends") {
    auto p   = tmp_db("prev_chain");
    auto led = Ledger::open(p);
    REQUIRE(led);
    nlohmann::json b;
    Hash prev = Hash::zero();
    for (int i = 0; i < 100; ++i) {
        auto e = led.value().append("chain", "sys", b);
        REQUIRE(e);
        CHECK(e.value().header.prev_hash == prev);
        prev = e.value().entry_hash();
    }
    CHECK(led.value().head() == prev);
}

TEST_CASE("Ledger reopens preserve length and head") {
    auto p   = tmp_db("reopen");
    Hash    expected_head;
    {
        auto led = Ledger::open(p);
        REQUIRE(led);
        nlohmann::json b;
        for (int i = 0; i < 30; ++i) REQUIRE(led.value().append("re", "sys", b));
        expected_head = led.value().head();
    }
    {
        auto led = Ledger::open(p);
        REQUIRE(led);
        CHECK(led.value().length() == 30);
        CHECK(led.value().head() == expected_head);
        REQUIRE(led.value().verify());
    }
}

TEST_CASE("Tenant filtering across many tenants is correct") {
    auto p   = tmp_db("tenant_many");
    auto led = Ledger::open(p);
    REQUIRE(led);
    nlohmann::json b;
    // 50 entries each across tenants alpha/beta/gamma/delta + 50 default.
    for (int i = 0; i < 50; ++i) {
        REQUIRE(led.value().append("e", "sys", b, "alpha"));
        REQUIRE(led.value().append("e", "sys", b, "beta"));
        REQUIRE(led.value().append("e", "sys", b, "gamma"));
        REQUIRE(led.value().append("e", "sys", b, "delta"));
        REQUIRE(led.value().append("e", "sys", b, ""));
    }
    CHECK(led.value().length() == 250);
    CHECK(led.value().tail_for_tenant("alpha", 100).value().size() == 50);
    CHECK(led.value().tail_for_tenant("beta",  100).value().size() == 50);
    CHECK(led.value().tail_for_tenant("gamma", 100).value().size() == 50);
    CHECK(led.value().tail_for_tenant("delta", 100).value().size() == 50);
    CHECK(led.value().tail_for_tenant("",      100).value().size() == 50);
    CHECK(led.value().tail_for_tenant("missing", 100).value().empty());

    auto alpha_range = led.value().range_for_tenant("alpha", 1, 251);
    REQUIRE(alpha_range);
    CHECK(alpha_range.value().size() == 50);
    for (const auto& e : alpha_range.value()) CHECK(e.header.tenant == "alpha");
}

TEST_CASE("Time-range query bounds are half-open") {
    auto p   = tmp_db("time_bounds");
    auto led = Ledger::open(p);
    REQUIRE(led);
    nlohmann::json b;
    Time t0 = Time::now();
    auto e1 = led.value().append("a", "sys", b); REQUIRE(e1);
    Time t1 = e1.value().header.ts;
    auto e2 = led.value().append("b", "sys", b); REQUIRE(e2);
    Time t2 = e2.value().header.ts;
    auto e3 = led.value().append("c", "sys", b); REQUIRE(e3);
    Time t3 = e3.value().header.ts;
    Time tEnd = Time::now() + std::chrono::seconds{1};

    auto all = led.value().range_by_time(t0, tEnd);
    REQUIRE(all);
    CHECK(all.value().size() == 3);

    // Half-open: [t1, t3) must include e1 and e2 but not e3.
    auto mid = led.value().range_by_time(t1, t3);
    REQUIRE(mid);
    CHECK(mid.value().size() == 2);
    CHECK(mid.value()[0].header.seq == 1);
    CHECK(mid.value()[1].header.seq == 2);
    (void)t2;
}

TEST_CASE("Subscriber receives every entry in order across 50 appends") {
    auto p   = tmp_db("sub_50");
    auto led = Ledger::open(p);
    REQUIRE(led);
    std::vector<std::uint64_t> seen;
    auto sub = led.value().subscribe([&](const LedgerEntry& e) {
        seen.push_back(e.header.seq);
    });
    nlohmann::json b;
    for (int i = 0; i < 50; ++i) REQUIRE(led.value().append("s", "sys", b));
    REQUIRE(seen.size() == 50);
    for (std::size_t i = 0; i < 50; ++i) CHECK(seen[i] == i + 1);
}

TEST_CASE("Checkpoint reflects current head after multiple appends") {
    auto p   = tmp_db("cp_multi");
    auto led = Ledger::open(p);
    REQUIRE(led);
    nlohmann::json b;

    auto cp0 = led.value().checkpoint();
    CHECK(cp0.seq == 0);
    CHECK(cp0.head_hash == Hash::zero());
    REQUIRE(verify_checkpoint(cp0));

    REQUIRE(led.value().append("a", "sys", b));
    auto cp1 = led.value().checkpoint();
    CHECK(cp1.seq == 1);
    CHECK(cp1.head_hash != cp0.head_hash);
    REQUIRE(verify_checkpoint(cp1));

    for (int i = 0; i < 9; ++i) REQUIRE(led.value().append("b", "sys", b));
    auto cp10 = led.value().checkpoint();
    CHECK(cp10.seq == 10);
    REQUIRE(verify_checkpoint(cp10));
}

TEST_CASE("Checkpoint key_id matches ledger key_id") {
    auto p   = tmp_db("cp_keyid");
    auto led = Ledger::open(p);
    REQUIRE(led);
    nlohmann::json b;
    REQUIRE(led.value().append("a", "sys", b));
    auto cp = led.value().checkpoint();
    CHECK(cp.key_id == led.value().key_id());
}

TEST_CASE("Checkpoint with tampered seq fails verify") {
    auto p   = tmp_db("cp_seq");
    auto led = Ledger::open(p);
    REQUIRE(led);
    nlohmann::json b;
    REQUIRE(led.value().append("a", "sys", b));
    auto cp = led.value().checkpoint();
    cp.seq += 1;
    CHECK(!verify_checkpoint(cp));
}

TEST_CASE("Checkpoint with tampered ts fails verify") {
    auto p   = tmp_db("cp_ts");
    auto led = Ledger::open(p);
    REQUIRE(led);
    nlohmann::json b;
    REQUIRE(led.value().append("a", "sys", b));
    auto cp = led.value().checkpoint();
    cp.ts = Time{cp.ts.nanos_since_epoch() + 1000};
    CHECK(!verify_checkpoint(cp));
}

TEST_CASE("Checkpoint with tampered key_id fails verify") {
    auto p   = tmp_db("cp_kid");
    auto led = Ledger::open(p);
    REQUIRE(led);
    nlohmann::json b;
    REQUIRE(led.value().append("a", "sys", b));
    auto cp = led.value().checkpoint();
    cp.key_id += "X";
    CHECK(!verify_checkpoint(cp));
}

TEST_CASE("Checkpoint from_json rejects malformed inputs") {
    CHECK(!LedgerCheckpoint::from_json(""));
    CHECK(!LedgerCheckpoint::from_json("{"));
    CHECK(!LedgerCheckpoint::from_json("{}"));
    CHECK(!LedgerCheckpoint::from_json(R"({"seq":1})"));
    CHECK(!LedgerCheckpoint::from_json(
        R"({"seq":1,"head_hash":"too-short","ts_ns":0,"key_id":"x","signature":"","public_key":""})"));
}

TEST_CASE("JSONL export of empty ledger writes zero entries") {
    auto p   = tmp_db("jsonl_empty");
    {
        auto led = Ledger::open(p);
        REQUIRE(led);
    }
    auto out = std::filesystem::temp_directory_path()
             / ("asc_jsonl_empty_" + std::to_string(std::random_device{}()) + ".jsonl");
    auto stats = LedgerJsonl::export_to(p.string(), out.string());
    REQUIRE(stats);
    CHECK(stats.value().entries_written == 0);
    CHECK(std::filesystem::exists(out));
    std::filesystem::remove(out);
}

TEST_CASE("JSONL import rejects malformed lines") {
    auto bad = std::filesystem::temp_directory_path()
             / ("asc_jsonl_bad_" + std::to_string(std::random_device{}()) + ".jsonl");
    {
        std::ofstream o(bad);
        o << "this is not json\n";
    }
    auto dst = tmp_db("jsonl_bad_dst");
    auto led = Ledger::open(dst);
    REQUIRE(led);
    // We need a key — generate any.
    auto key = KeyStore::generate();
    auto r = LedgerJsonl::import_to(bad.string(), dst.string(), std::move(key));
    CHECK(!r);
    std::filesystem::remove(bad);
}

TEST_CASE("Migrator refuses non-empty destination") {
    auto src = tmp_db("mig_src");
    auto dst = tmp_db("mig_dst");
    {
        auto sl = Ledger::open(src);
        REQUIRE(sl);
        nlohmann::json b;
        REQUIRE(sl.value().append("a", "sys", b));
    }
    {
        auto dl = Ledger::open(dst);
        REQUIRE(dl);
        nlohmann::json b;
        REQUIRE(dl.value().append("preexisting", "sys", b));  // make non-empty
    }
    auto src_key_path = src; src_key_path.replace_extension(".key");
    std::ifstream kf(src_key_path);
    std::string blob((std::istreambuf_iterator<char>(kf)), {});
    auto key = KeyStore::deserialize(blob);
    REQUIRE(key);
    auto r = LedgerMigrator::copy(src.string(), dst.string(), std::move(key.value()));
    CHECK(!r);
}

// ============== verify_parallel ==========================================

TEST_CASE("verify_parallel matches verify on a 600-entry chain") {
    auto p   = tmp_db("vp_600");
    auto led = Ledger::open(p);
    REQUIRE(led);
    std::vector<Ledger::AppendSpec> specs(600, {"e","sys",nlohmann::json{},""});
    REQUIRE(led.value().append_batch(std::move(specs)));
    REQUIRE(led.value().verify());
    REQUIRE(led.value().verify_parallel());          // hardware_concurrency
    REQUIRE(led.value().verify_parallel(1));         // single-thread fallback
    REQUIRE(led.value().verify_parallel(2));
    REQUIRE(led.value().verify_parallel(4));
    REQUIRE(led.value().verify_parallel(8));
}

TEST_CASE("verify_parallel falls back gracefully on tiny chains") {
    // <512 entries: implementation falls through to plain verify(),
    // so it should still succeed and return the same Result.
    auto p   = tmp_db("vp_small");
    auto led = Ledger::open(p);
    REQUIRE(led);
    nlohmann::json b;
    for (int i = 0; i < 50; ++i) REQUIRE(led.value().append("e","sys",b));
    REQUIRE(led.value().verify_parallel());
    REQUIRE(led.value().verify_parallel(8));
}

TEST_CASE("verify_parallel handles empty chain") {
    auto p   = tmp_db("vp_empty");
    auto led = Ledger::open(p);
    REQUIRE(led);
    REQUIRE(led.value().verify_parallel());
}

TEST_CASE("verify_parallel detects tampered body (1k entries, threads=4)") {
    auto p   = tmp_db("vp_tamper_body");
    {
        auto led = Ledger::open(p);
        REQUIRE(led);
        std::vector<Ledger::AppendSpec> specs(1000, {"e","sys",nlohmann::json{},""});
        REQUIRE(led.value().append_batch(std::move(specs)));
    }

    // Tamper with seq 500's body via raw SQLite.
    sqlite3* raw = nullptr;
    REQUIRE(sqlite3_open(p.string().c_str(), &raw) == SQLITE_OK);
    REQUIRE(sqlite3_exec(raw,
        "UPDATE asclepius_ledger SET body = body || ' ' WHERE seq = 500;",
        nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);

    auto led = Ledger::open(p);
    REQUIRE(led);
    auto r = led.value().verify_parallel(4);
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::integrity_failure);
}

TEST_CASE("verify_parallel detects tampered signature") {
    auto p   = tmp_db("vp_tamper_sig");
    {
        auto led = Ledger::open(p);
        REQUIRE(led);
        std::vector<Ledger::AppendSpec> specs(800, {"e","sys",nlohmann::json{},""});
        REQUIRE(led.value().append_batch(std::move(specs)));
    }

    // XOR a byte of seq 600's signature.
    sqlite3* raw = nullptr;
    REQUIRE(sqlite3_open(p.string().c_str(), &raw) == SQLITE_OK);
    sqlite3_stmt* st = nullptr;
    REQUIRE(sqlite3_prepare_v2(raw,
        "UPDATE asclepius_ledger SET signature = "
        "  zeroblob(64) WHERE seq = 600;",
        -1, &st, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
    sqlite3_close(raw);

    auto led = Ledger::open(p);
    REQUIRE(led);
    CHECK(!led.value().verify_parallel(4));
}

TEST_CASE("verify_parallel single-threaded equals verify") {
    auto p   = tmp_db("vp_single_eq");
    auto led = Ledger::open(p);
    REQUIRE(led);
    std::vector<Ledger::AppendSpec> specs(700, {"e","sys",nlohmann::json{},""});
    REQUIRE(led.value().append_batch(std::move(specs)));

    auto a = led.value().verify();
    auto b = led.value().verify_parallel(1);
    CHECK(static_cast<bool>(a) == static_cast<bool>(b));
}

TEST_CASE("verify_parallel honors hardware_concurrency") {
    auto p   = tmp_db("vp_hw");
    auto led = Ledger::open(p);
    REQUIRE(led);
    std::vector<Ledger::AppendSpec> specs(550, {"e","sys",nlohmann::json{},""});
    REQUIRE(led.value().append_batch(std::move(specs)));
    REQUIRE(led.value().verify_parallel(0));  // 0 → hardware_concurrency
}

TEST_CASE("verify_parallel detects gap (ledger truncation simulation)") {
    auto p   = tmp_db("vp_gap");
    {
        auto led = Ledger::open(p);
        REQUIRE(led);
        std::vector<Ledger::AppendSpec> specs(700, {"e","sys",nlohmann::json{},""});
        REQUIRE(led.value().append_batch(std::move(specs)));
    }

    // Delete seq 350 to simulate truncation/gap.
    sqlite3* raw = nullptr;
    REQUIRE(sqlite3_open(p.string().c_str(), &raw) == SQLITE_OK);
    REQUIRE(sqlite3_exec(raw, "DELETE FROM asclepius_ledger WHERE seq = 350;",
                         nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);

    auto led = Ledger::open(p);
    REQUIRE(led);
    auto r = led.value().verify_parallel(4);
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::integrity_failure);
}

TEST_CASE("verify_parallel passes for a clean 2000-entry chain") {
    auto p   = tmp_db("vp_2k");
    auto led = Ledger::open(p);
    REQUIRE(led);
    std::vector<Ledger::AppendSpec> specs(2000, {"e","sys",nlohmann::json{},""});
    REQUIRE(led.value().append_batch(std::move(specs)));
    REQUIRE(led.value().verify_parallel());
    REQUIRE(led.value().verify_parallel(2));
    REQUIRE(led.value().verify_parallel(4));
}

TEST_CASE("verify_parallel detects the exact seq of a tampered entry") {
    auto p   = tmp_db("vp_pinpoint");
    {
        auto led = Ledger::open(p);
        REQUIRE(led);
        std::vector<Ledger::AppendSpec> specs(600, {"e","sys",nlohmann::json{},""});
        REQUIRE(led.value().append_batch(std::move(specs)));
    }

    sqlite3* raw = nullptr;
    REQUIRE(sqlite3_open(p.string().c_str(), &raw) == SQLITE_OK);
    REQUIRE(sqlite3_exec(raw,
        "UPDATE asclepius_ledger SET body = body || 'X' WHERE seq = 123;",
        nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);

    auto led = Ledger::open(p);
    REQUIRE(led);
    auto r = led.value().verify_parallel(4);
    REQUIRE(!r);
    CHECK(std::string{r.error().what()}.find("seq 123") != std::string::npos);
}

TEST_CASE("verify_parallel idempotent: running it twice returns same result") {
    auto p   = tmp_db("vp_idemp");
    auto led = Ledger::open(p);
    REQUIRE(led);
    std::vector<Ledger::AppendSpec> specs(600, {"e","sys",nlohmann::json{},""});
    REQUIRE(led.value().append_batch(std::move(specs)));
    REQUIRE(led.value().verify_parallel(4));
    REQUIRE(led.value().verify_parallel(4));
    REQUIRE(led.value().verify_parallel(4));
}

// ============== append_batch ==============================================

TEST_CASE("append_batch with empty input is a no-op") {
    auto p   = tmp_db("batch_empty");
    auto led = Ledger::open(p);
    REQUIRE(led);
    auto r = led.value().append_batch({});
    REQUIRE(r);
    CHECK(r.value().empty());
    CHECK(led.value().length() == 0);
    REQUIRE(led.value().verify());
}

TEST_CASE("append_batch with one entry equals append()") {
    auto p1 = tmp_db("batch_one_a");
    auto p2 = tmp_db("batch_one_b");
    auto l1 = Ledger::open(p1);
    auto l2 = Ledger::open(p2, [&]{
        auto kp = p1; kp.replace_extension(".key");
        std::ifstream kf(kp);
        std::string blob((std::istreambuf_iterator<char>(kf)), {});
        return KeyStore::deserialize(blob).value();
    }());
    REQUIRE(l1); REQUIRE(l2);

    nlohmann::json b; b["x"] = 1;
    auto e1 = l1.value().append("evt", "act", b, "");
    REQUIRE(e1);

    auto e2 = l2.value().append_batch({{"evt", "act", b, ""}});
    REQUIRE(e2);
    REQUIRE(e2.value().size() == 1);

    // Same key + same logical content + same seq=1 → same entry_hash.
    // (ts will differ but the test enforces structural equivalence.)
    CHECK(e1.value().header.seq == e2.value()[0].header.seq);
    CHECK(e1.value().body_json   == e2.value()[0].body_json);
    CHECK(e1.value().header.actor == e2.value()[0].header.actor);
}

TEST_CASE("append_batch returns entries in seq-ascending order") {
    auto p   = tmp_db("batch_order");
    auto led = Ledger::open(p);
    REQUIRE(led);
    std::vector<Ledger::AppendSpec> specs;
    nlohmann::json b;
    for (int i = 0; i < 10; ++i) {
        b["i"] = i;
        specs.push_back({"e", "sys", b, ""});
    }
    auto r = led.value().append_batch(std::move(specs));
    REQUIRE(r);
    REQUIRE(r.value().size() == 10);
    for (std::size_t i = 0; i < r.value().size(); ++i) {
        CHECK(r.value()[i].header.seq == i + 1);
    }
}

TEST_CASE("append_batch chains prev_hash correctly within the batch") {
    auto p   = tmp_db("batch_chain");
    auto led = Ledger::open(p);
    REQUIRE(led);
    std::vector<Ledger::AppendSpec> specs(20, {"e", "sys", nlohmann::json{}, ""});
    auto r = led.value().append_batch(std::move(specs));
    REQUIRE(r);
    REQUIRE(r.value().size() == 20);

    Hash expected_prev = Hash::zero();
    for (std::size_t i = 0; i < r.value().size(); ++i) {
        CHECK(r.value()[i].header.prev_hash == expected_prev);
        expected_prev = r.value()[i].entry_hash();
    }
    CHECK(led.value().head() == expected_prev);
}

TEST_CASE("append_batch + verify() end-to-end") {
    auto p   = tmp_db("batch_verify");
    auto led = Ledger::open(p);
    REQUIRE(led);
    std::vector<Ledger::AppendSpec> specs(50, {"e", "sys", nlohmann::json{}, ""});
    REQUIRE(led.value().append_batch(std::move(specs)));
    CHECK(led.value().length() == 50);
    REQUIRE(led.value().verify());
}

TEST_CASE("append_batch preserves chain across batch boundary") {
    auto p   = tmp_db("batch_boundary");
    auto led = Ledger::open(p);
    REQUIRE(led);

    nlohmann::json b;
    auto first = led.value().append("pre", "sys", b);
    REQUIRE(first);
    Hash h_after_first = first.value().entry_hash();

    std::vector<Ledger::AppendSpec> specs(10, {"e", "sys", b, ""});
    auto batch = led.value().append_batch(std::move(specs));
    REQUIRE(batch);

    // First batch entry's prev_hash MUST match the chain head before
    // append_batch was called (the single pre-existing entry's hash).
    CHECK(batch.value()[0].header.prev_hash == h_after_first);
    REQUIRE(led.value().verify());
    CHECK(led.value().length() == 11);
}

TEST_CASE("append_batch can interleave with single appends") {
    auto p   = tmp_db("batch_interleave");
    auto led = Ledger::open(p);
    REQUIRE(led);
    nlohmann::json b;

    REQUIRE(led.value().append("s1", "sys", b));
    REQUIRE(led.value().append_batch({{"b1","sys",b,""},{"b2","sys",b,""}}));
    REQUIRE(led.value().append("s2", "sys", b));
    REQUIRE(led.value().append_batch({{"b3","sys",b,""}}));

    CHECK(led.value().length() == 5);
    REQUIRE(led.value().verify());
    auto all = led.value().range(1, 6);
    REQUIRE(all);
    CHECK(all.value()[0].header.event_type == "s1");
    CHECK(all.value()[1].header.event_type == "b1");
    CHECK(all.value()[2].header.event_type == "b2");
    CHECK(all.value()[3].header.event_type == "s2");
    CHECK(all.value()[4].header.event_type == "b3");
}

TEST_CASE("append_batch with mixed tenants tags each entry correctly") {
    auto p   = tmp_db("batch_tenants");
    auto led = Ledger::open(p);
    REQUIRE(led);
    nlohmann::json b;
    auto r = led.value().append_batch({
        {"e","s",b,"alpha"}, {"e","s",b,"alpha"},
        {"e","s",b,"beta"},  {"e","s",b,""},
        {"e","s",b,"alpha"},
    });
    REQUIRE(r);
    REQUIRE(r.value().size() == 5);
    CHECK(r.value()[0].header.tenant == "alpha");
    CHECK(r.value()[1].header.tenant == "alpha");
    CHECK(r.value()[2].header.tenant == "beta");
    CHECK(r.value()[3].header.tenant == "");
    CHECK(r.value()[4].header.tenant == "alpha");

    // Tenant-scoped reads see the right counts.
    CHECK(led.value().tail_for_tenant("alpha", 100).value().size() == 3);
    CHECK(led.value().tail_for_tenant("beta",  100).value().size() == 1);
    CHECK(led.value().tail_for_tenant("",      100).value().size() == 1);
}

TEST_CASE("append_batch with mixed event types is preserved") {
    auto p   = tmp_db("batch_evtypes");
    auto led = Ledger::open(p);
    REQUIRE(led);
    nlohmann::json b;
    REQUIRE(led.value().append_batch({
        {"alpha", "s", b, ""},
        {"beta",  "s", b, ""},
        {"gamma", "s", b, ""},
    }));
    auto all = led.value().range(1, 4);
    REQUIRE(all);
    CHECK(all.value()[0].header.event_type == "alpha");
    CHECK(all.value()[1].header.event_type == "beta");
    CHECK(all.value()[2].header.event_type == "gamma");
}

TEST_CASE("append_batch fires subscribers in entry order, after commit") {
    auto p   = tmp_db("batch_sub");
    auto led = Ledger::open(p);
    REQUIRE(led);
    std::vector<std::uint64_t> seen;
    auto sub = led.value().subscribe([&](const LedgerEntry& e) {
        seen.push_back(e.header.seq);
    });
    nlohmann::json b;
    REQUIRE(led.value().append_batch({
        {"a","s",b,""}, {"b","s",b,""}, {"c","s",b,""}, {"d","s",b,""},
    }));
    REQUIRE(seen.size() == 4);
    CHECK(seen[0] == 1);
    CHECK(seen[1] == 2);
    CHECK(seen[2] == 3);
    CHECK(seen[3] == 4);
}

TEST_CASE("append_batch large stress: 500 entries + verify") {
    auto p   = tmp_db("batch_500");
    auto led = Ledger::open(p);
    REQUIRE(led);
    std::vector<Ledger::AppendSpec> specs;
    specs.reserve(500);
    nlohmann::json b;
    for (int i = 0; i < 500; ++i) {
        b["i"] = i;
        specs.push_back({"stress", "sys", b, ""});
    }
    auto r = led.value().append_batch(std::move(specs));
    REQUIRE(r);
    CHECK(r.value().size() == 500);
    CHECK(led.value().length() == 500);
    REQUIRE(led.value().verify());
}

TEST_CASE("append_batch survives ledger reopen") {
    auto p   = tmp_db("batch_reopen");
    Hash    expected;
    {
        auto led = Ledger::open(p);
        REQUIRE(led);
        std::vector<Ledger::AppendSpec> specs(40, {"e","s",nlohmann::json{},""});
        REQUIRE(led.value().append_batch(std::move(specs)));
        expected = led.value().head();
    }
    {
        auto led = Ledger::open(p);
        REQUIRE(led);
        CHECK(led.value().length() == 40);
        CHECK(led.value().head() == expected);
        REQUIRE(led.value().verify());
    }
}

TEST_CASE("append_batch entries each carry distinct entry_hash") {
    auto p   = tmp_db("batch_distinct");
    auto led = Ledger::open(p);
    REQUIRE(led);
    nlohmann::json b;
    auto r = led.value().append_batch({
        {"e","s",b,""}, {"e","s",b,""}, {"e","s",b,""},
    });
    REQUIRE(r);
    // Same event_type, same body, same actor — but ts differs, so seq+ts
    // makes prev_hash different, so entry_hash is different.
    CHECK(r.value()[0].entry_hash() != r.value()[1].entry_hash());
    CHECK(r.value()[1].entry_hash() != r.value()[2].entry_hash());
    CHECK(r.value()[0].entry_hash() != r.value()[2].entry_hash());
}

TEST_CASE("append_batch + range_by_time queries land within the batch window") {
    auto p   = tmp_db("batch_time");
    auto led = Ledger::open(p);
    REQUIRE(led);
    Time before = Time::now();
    std::vector<Ledger::AppendSpec> specs(15, {"t","s",nlohmann::json{},""});
    REQUIRE(led.value().append_batch(std::move(specs)));
    Time after = Time::now() + std::chrono::seconds{1};

    auto in_window = led.value().range_by_time(before, after);
    REQUIRE(in_window);
    CHECK(in_window.value().size() == 15);
}

TEST_CASE("append_batch interacts with checkpoint correctly") {
    auto p   = tmp_db("batch_cp");
    auto led = Ledger::open(p);
    REQUIRE(led);
    std::vector<Ledger::AppendSpec> specs(7, {"e","s",nlohmann::json{},""});
    REQUIRE(led.value().append_batch(std::move(specs)));
    auto cp = led.value().checkpoint();
    CHECK(cp.seq == 7);
    REQUIRE(verify_checkpoint(cp));
}

TEST_CASE("append_batch JSON-loaded body is canonicalized") {
    auto p   = tmp_db("batch_canon");
    auto led = Ledger::open(p);
    REQUIRE(led);
    // Two semantically-identical bodies, different key ordering. After
    // canonicalisation they should produce the same body_json string.
    nlohmann::json b1 = nlohmann::json::object({{"a", 1}, {"b", 2}, {"c", 3}});
    nlohmann::json b2 = nlohmann::json::object({{"c", 3}, {"a", 1}, {"b", 2}});
    auto r = led.value().append_batch({
        {"x","s",b1,""}, {"x","s",b2,""},
    });
    REQUIRE(r);
    CHECK(r.value()[0].body_json == r.value()[1].body_json);
}

TEST_CASE("append_batch + tenant filter is index-friendly") {
    auto p   = tmp_db("batch_tfilter");
    auto led = Ledger::open(p);
    REQUIRE(led);
    std::vector<Ledger::AppendSpec> specs;
    nlohmann::json b;
    for (int i = 0; i < 100; ++i) {
        specs.push_back({"e","s",b, (i % 3 == 0) ? "alpha" : "beta"});
    }
    REQUIRE(led.value().append_batch(std::move(specs)));
    int alpha_count = (100 + 2) / 3;  // ceil(100/3) = 34
    CHECK(led.value().tail_for_tenant("alpha", 1000).value().size()
          == static_cast<std::size_t>(alpha_count));
    CHECK(led.value().tail_for_tenant("beta", 1000).value().size()
          == static_cast<std::size_t>(100 - alpha_count));
}

TEST_CASE("append_batch with 1000 entries scales linearly in chain integrity") {
    auto p   = tmp_db("batch_1k");
    auto led = Ledger::open(p);
    REQUIRE(led);
    std::vector<Ledger::AppendSpec> specs;
    specs.reserve(1000);
    nlohmann::json b;
    for (int i = 0; i < 1000; ++i) specs.push_back({"e","s",b,""});
    auto r = led.value().append_batch(std::move(specs));
    REQUIRE(r);
    CHECK(r.value().size() == 1000);
    CHECK(led.value().length() == 1000);
    REQUIRE(led.value().verify());
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

// ============== Ledger::stats ============================================

TEST_CASE("Ledger::stats on empty ledger returns zeros and key id") {
    auto p = tmp_db("stats_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto s = l_.value().stats(); REQUIRE(s);
    CHECK(s.value().entry_count      == 0);
    CHECK(s.value().total_body_bytes == 0);
    CHECK(s.value().avg_body_bytes   == 0);
    CHECK(s.value().oldest_seq       == 0);
    CHECK(s.value().newest_seq       == 0);
    CHECK(!s.value().key_id.empty());
}

TEST_CASE("Ledger::stats counts and sums body bytes") {
    auto p = tmp_db("stats_count");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 10; i++) {
        REQUIRE(l.append("evt", "actor",
            nlohmann::json{{"i", i}, {"pad", std::string(50, 'x')}}, ""));
    }
    auto s = l.stats(); REQUIRE(s);
    CHECK(s.value().entry_count == 10);
    CHECK(s.value().oldest_seq  == 1);
    CHECK(s.value().newest_seq  == 10);
    CHECK(s.value().total_body_bytes > 500);  // 10 * (>50 bytes)
    CHECK(s.value().avg_body_bytes ==
          s.value().total_body_bytes / s.value().entry_count);
}

TEST_CASE("Ledger::stats head_hash matches Ledger::head") {
    auto p = tmp_db("stats_head");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 5; i++) {
        REQUIRE(l.append("evt", "actor", nlohmann::json{{"i", i}}, ""));
    }
    auto s = l.stats(); REQUIRE(s);
    CHECK(s.value().head_hash.hex() == l.head().hex());
}

TEST_CASE("Ledger::stats oldest_ts <= newest_ts") {
    auto p = tmp_db("stats_ts");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("a", "x", nlohmann::json::object(), ""));
    REQUIRE(l.append("b", "x", nlohmann::json::object(), ""));
    REQUIRE(l.append("c", "x", nlohmann::json::object(), ""));
    auto s = l.stats(); REQUIRE(s);
    CHECK(s.value().oldest_ts <= s.value().newest_ts);
}

TEST_CASE("Ledger::stats survives re-open") {
    auto p = tmp_db("stats_reopen");
    {
        auto l_ = Ledger::open(p); REQUIRE(l_);
        for (int i = 0; i < 7; i++) {
            REQUIRE(l_.value().append("e", "a",
                nlohmann::json{{"i", i}}, ""));
        }
    }
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto s = l_.value().stats(); REQUIRE(s);
    CHECK(s.value().entry_count == 7);
    CHECK(s.value().newest_seq  == 7);
    CHECK(s.value().total_body_bytes > 0);
}

TEST_CASE("Ledger::Stats::to_json round-trips through JSON parser") {
    auto p = tmp_db("stats_json");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("evt", "actor", nlohmann::json{{"k", "v"}}, "tnt"));
    auto s = l.stats(); REQUIRE(s);
    auto j = nlohmann::json::parse(s.value().to_json());
    CHECK(j.contains("entry_count"));
    CHECK(j.contains("head_hash"));
    CHECK(j.contains("oldest_seq"));
    CHECK(j.contains("newest_seq"));
    CHECK(j.contains("oldest_ts"));
    CHECK(j.contains("newest_ts"));
    CHECK(j.contains("total_body_bytes"));
    CHECK(j.contains("avg_body_bytes"));
    CHECK(j.contains("key_id"));
    CHECK(j["entry_count"].get<std::uint64_t>() == 1);
    CHECK(j["key_id"].get<std::string>() == s.value().key_id);
}

TEST_CASE("Ledger::stats key_id matches Ledger::key_id") {
    auto p = tmp_db("stats_keyid");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("evt", "x", nlohmann::json::object(), ""));
    auto s = l.stats(); REQUIRE(s);
    CHECK(s.value().key_id == l.key_id());
}

TEST_CASE("Ledger::stats handles 1000-entry chain") {
    auto p = tmp_db("stats_1k");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 1000; i++) {
        REQUIRE(l.append("e", "a", nlohmann::json{{"i", i}}, ""));
    }
    auto s = l.stats(); REQUIRE(s);
    CHECK(s.value().entry_count == 1000);
    CHECK(s.value().oldest_seq  == 1);
    CHECK(s.value().newest_seq  == 1000);
    CHECK(s.value().avg_body_bytes > 0);
}

// ============== Ledger::stats_for_tenant ================================

TEST_CASE("stats_for_tenant counts only the matching tenant") {
    auto p = tmp_db("stats_tenant_split");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 5; i++)
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, "alpha"));
    for (int i = 0; i < 7; i++)
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, "beta"));
    auto sa = l.stats_for_tenant("alpha"); REQUIRE(sa);
    CHECK(sa.value().entry_count == 5);
    auto sb = l.stats_for_tenant("beta"); REQUIRE(sb);
    CHECK(sb.value().entry_count == 7);
    auto se = l.stats_for_tenant("");      REQUIRE(se);
    CHECK(se.value().entry_count == 0);
}

TEST_CASE("stats_for_tenant: empty-tenant scope is its own bucket") {
    auto p = tmp_db("stats_tenant_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "x", nlohmann::json::object(), ""));
    REQUIRE(l.append("e", "x", nlohmann::json::object(), ""));
    REQUIRE(l.append("e", "x", nlohmann::json::object(), "alpha"));
    auto se = l.stats_for_tenant(""); REQUIRE(se);
    CHECK(se.value().entry_count == 2);
    auto sa = l.stats_for_tenant("alpha"); REQUIRE(sa);
    CHECK(sa.value().entry_count == 1);
}

TEST_CASE("stats_for_tenant: oldest/newest seq + ts are tenant-local") {
    auto p = tmp_db("stats_tenant_seq");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "x", nlohmann::json::object(), "alpha"));  // seq 1
    REQUIRE(l.append("e", "x", nlohmann::json::object(), "beta"));   // seq 2
    REQUIRE(l.append("e", "x", nlohmann::json::object(), "alpha"));  // seq 3
    REQUIRE(l.append("e", "x", nlohmann::json::object(), "beta"));   // seq 4
    auto sa = l.stats_for_tenant("alpha"); REQUIRE(sa);
    CHECK(sa.value().oldest_seq == 1);
    CHECK(sa.value().newest_seq == 3);
    auto sb = l.stats_for_tenant("beta"); REQUIRE(sb);
    CHECK(sb.value().oldest_seq == 2);
    CHECK(sb.value().newest_seq == 4);
}

TEST_CASE("stats_for_tenant on missing tenant returns zero-count Stats") {
    auto p = tmp_db("stats_tenant_miss");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "x", nlohmann::json::object(), "alpha"));
    auto sg = l.stats_for_tenant("gamma"); REQUIRE(sg);
    CHECK(sg.value().entry_count      == 0);
    CHECK(sg.value().total_body_bytes == 0);
    CHECK(sg.value().avg_body_bytes   == 0);
    CHECK(!sg.value().key_id.empty());  // key_id always populated
}

TEST_CASE("stats_for_tenant: per-tenant counts sum to global stats.entry_count") {
    auto p = tmp_db("stats_tenant_sum");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 13; i++)
        REQUIRE(l.append("e", "x", nlohmann::json::object(), "alpha"));
    for (int i = 0; i < 17; i++)
        REQUIRE(l.append("e", "x", nlohmann::json::object(), "beta"));
    for (int i = 0; i < 4; i++)
        REQUIRE(l.append("e", "x", nlohmann::json::object(), ""));
    auto sa = l.stats_for_tenant("alpha"); REQUIRE(sa);
    auto sb = l.stats_for_tenant("beta");  REQUIRE(sb);
    auto se = l.stats_for_tenant("");      REQUIRE(se);
    auto g  = l.stats();                   REQUIRE(g);
    CHECK(sa.value().entry_count + sb.value().entry_count + se.value().entry_count
          == g.value().entry_count);
}

TEST_CASE("stats_for_tenant: large multi-chunk scan still correct") {
    auto p = tmp_db("stats_tenant_chunk");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // > kChunk (1024) entries to exercise the pagination loop.
    for (int i = 0; i < 1500; i++) {
        REQUIRE(l.append("e", "x",
            nlohmann::json{{"i", i}},
            (i % 3 == 0) ? "alpha" : "beta"));
    }
    auto sa = l.stats_for_tenant("alpha"); REQUIRE(sa);
    auto sb = l.stats_for_tenant("beta");  REQUIRE(sb);
    CHECK(sa.value().entry_count + sb.value().entry_count == 1500);
    // every third (0,3,6,...) goes to alpha → 500 entries.
    CHECK(sa.value().entry_count == 500);
    CHECK(sb.value().entry_count == 1000);
}

// ============== Ledger::count_by_event_type =============================

TEST_CASE("count_by_event_type tallies one type") {
    auto p = tmp_db("count_one");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 7; i++) {
        REQUIRE(l.append("evt_a", "x", nlohmann::json::object(), ""));
    }
    auto c = l.count_by_event_type(); REQUIRE(c);
    CHECK(c.value().size() == 1);
    CHECK(c.value().at("evt_a") == 7);
}

TEST_CASE("count_by_event_type tallies multiple types separately") {
    auto p = tmp_db("count_multi");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 5; i++)
        REQUIRE(l.append("alpha", "x", nlohmann::json::object(), ""));
    for (int i = 0; i < 3; i++)
        REQUIRE(l.append("beta", "x", nlohmann::json::object(), ""));
    for (int i = 0; i < 9; i++)
        REQUIRE(l.append("gamma", "x", nlohmann::json::object(), ""));
    auto c = l.count_by_event_type(); REQUIRE(c);
    CHECK(c.value().size() == 3);
    CHECK(c.value().at("alpha") == 5);
    CHECK(c.value().at("beta")  == 3);
    CHECK(c.value().at("gamma") == 9);
}

TEST_CASE("count_by_event_type on empty ledger returns empty map") {
    auto p = tmp_db("count_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto c = l_.value().count_by_event_type(); REQUIRE(c);
    CHECK(c.value().empty());
}

TEST_CASE("count_by_event_type sums to entry_count") {
    auto p = tmp_db("count_sum");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 10; i++)
        REQUIRE(l.append("evt_" + std::to_string(i % 3),
                         "x", nlohmann::json::object(), ""));
    auto c = l.count_by_event_type(); REQUIRE(c);
    std::uint64_t sum = 0;
    for (const auto& [_, n] : c.value()) sum += n;
    CHECK(sum == l.length());
}

TEST_CASE("count_by_event_type survives reopen") {
    auto p = tmp_db("count_reopen");
    {
        auto l_ = Ledger::open(p); REQUIRE(l_);
        for (int i = 0; i < 4; i++)
            REQUIRE(l_.value().append("a", "x", nlohmann::json::object(), ""));
        for (int i = 0; i < 6; i++)
            REQUIRE(l_.value().append("b", "x", nlohmann::json::object(), ""));
    }
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto c = l_.value().count_by_event_type(); REQUIRE(c);
    CHECK(c.value().at("a") == 4);
    CHECK(c.value().at("b") == 6);
}

// ============== Ledger::tail_by_actor ===================================

TEST_CASE("tail_by_actor returns matches most-recent first") {
    auto p = tmp_db("actor_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "alice", nlohmann::json{{"i", 1}}, ""));
    REQUIRE(l.append("e", "bob",   nlohmann::json{{"i", 2}}, ""));
    REQUIRE(l.append("e", "alice", nlohmann::json{{"i", 3}}, ""));
    REQUIRE(l.append("e", "alice", nlohmann::json{{"i", 4}}, ""));
    auto r = l.tail_by_actor("alice", 5); REQUIRE(r);
    REQUIRE(r.value().size() == 3);
    auto b0 = nlohmann::json::parse(r.value()[0].body_json);
    auto b1 = nlohmann::json::parse(r.value()[1].body_json);
    auto b2 = nlohmann::json::parse(r.value()[2].body_json);
    CHECK(b0["i"] == 4);  // most recent
    CHECK(b1["i"] == 3);
    CHECK(b2["i"] == 1);
}

TEST_CASE("tail_by_actor caps at n entries") {
    auto p = tmp_db("actor_cap");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 50; i++)
        REQUIRE(l.append("e", "alice", nlohmann::json{{"i", i}}, ""));
    auto r = l.tail_by_actor("alice", 10); REQUIRE(r);
    CHECK(r.value().size() == 10);
    auto b0 = nlohmann::json::parse(r.value()[0].body_json);
    CHECK(b0["i"] == 49);  // most recent
    auto b9 = nlohmann::json::parse(r.value()[9].body_json);
    CHECK(b9["i"] == 40);  // 10th most recent
}

TEST_CASE("tail_by_actor: unknown actor returns empty vector, not error") {
    auto p = tmp_db("actor_miss");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "alice", nlohmann::json::object(), ""));
    auto r = l.tail_by_actor("ghost", 5); REQUIRE(r);
    CHECK(r.value().empty());
}

TEST_CASE("tail_by_actor: empty actor returns invalid_argument") {
    auto p = tmp_db("actor_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto r = l_.value().tail_by_actor("", 5);
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("tail_by_actor: n=0 returns empty vector") {
    auto p = tmp_db("actor_zero");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "alice", nlohmann::json::object(), ""));
    auto r = l.tail_by_actor("alice", 0); REQUIRE(r);
    CHECK(r.value().empty());
}

TEST_CASE("tail_by_actor: matches system: actors") {
    auto p = tmp_db("actor_system");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "alice",                   nlohmann::json::object(), ""));
    REQUIRE(l.append("a", "system:drift_monitor",     nlohmann::json::object(), ""));
    REQUIRE(l.append("b", "system:consent_registry",  nlohmann::json::object(), ""));
    REQUIRE(l.append("c", "system:drift_monitor",     nlohmann::json::object(), ""));
    auto r = l.tail_by_actor("system:drift_monitor", 10); REQUIRE(r);
    CHECK(r.value().size() == 2);
    for (const auto& e : r.value())
        CHECK(e.header.actor == "system:drift_monitor");
}

TEST_CASE("tail_by_actor: large chain n-cap ring buffer correctness") {
    auto p = tmp_db("actor_ring");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 2000; i++) {
        REQUIRE(l.append("e", (i % 3 == 0) ? "alice" : "bob",
                         nlohmann::json{{"i", i}}, ""));
    }
    auto r = l.tail_by_actor("alice", 5); REQUIRE(r);
    CHECK(r.value().size() == 5);
    // Last alice entry: i=1998 (since 1998 % 3 == 0). Entries before:
    // 1995, 1992, 1989, 1986.
    auto b0 = nlohmann::json::parse(r.value()[0].body_json);
    auto b4 = nlohmann::json::parse(r.value()[4].body_json);
    CHECK(b0["i"] == 1998);
    CHECK(b4["i"] == 1986);
}

// ============== Ledger::find_by_inference_id ============================

TEST_CASE("find_by_inference_id locates the matching entry") {
    auto p = tmp_db("find_inf");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 20; i++) {
        REQUIRE(l.append("evt", "a",
            nlohmann::json{{"inference_id", "inf_" + std::to_string(i)},
                           {"i", i}}, ""));
    }
    auto e = l.find_by_inference_id("inf_13"); REQUIRE(e);
    auto body = nlohmann::json::parse(e.value().body_json);
    CHECK(body["inference_id"] == "inf_13");
    CHECK(body["i"]            == 13);
    CHECK(e.value().header.seq == 14);  // 1-indexed
}

TEST_CASE("find_by_inference_id returns not_found for unknown id") {
    auto p = tmp_db("find_miss");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("evt", "a", nlohmann::json{{"inference_id", "x1"}}, ""));
    auto r = l.find_by_inference_id("never_existed");
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::not_found);
}

TEST_CASE("find_by_inference_id rejects empty id") {
    auto p = tmp_db("find_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto r = l_.value().find_by_inference_id("");
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("find_by_inference_id ignores entries without inference_id field") {
    auto p = tmp_db("find_nokey");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("a", "x", nlohmann::json{{"some_other_field", "abc123"}}, ""));
    REQUIRE(l.append("a", "x", nlohmann::json{{"inference_id", "abc123"}}, ""));
    auto e = l.find_by_inference_id("abc123"); REQUIRE(e);
    auto body = nlohmann::json::parse(e.value().body_json);
    CHECK(body.contains("inference_id"));
    CHECK(body["inference_id"] == "abc123");
}

TEST_CASE("find_by_inference_id requires exact match, not substring") {
    auto p = tmp_db("find_exact");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("evt", "a", nlohmann::json{{"inference_id", "inf_abc"}}, ""));
    REQUIRE(l.append("evt", "a", nlohmann::json{{"inference_id", "inf_abcdef"}}, ""));
    auto e = l.find_by_inference_id("inf_abc"); REQUIRE(e);
    auto body = nlohmann::json::parse(e.value().body_json);
    CHECK(body["inference_id"] == "inf_abc");
}

TEST_CASE("find_by_inference_id returns first match if duplicates exist") {
    auto p = tmp_db("find_dup");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("evt", "a",
        nlohmann::json{{"inference_id", "dup"}, {"v", 1}}, ""));
    REQUIRE(l.append("evt", "a",
        nlohmann::json{{"inference_id", "dup"}, {"v", 2}}, ""));
    auto e = l.find_by_inference_id("dup"); REQUIRE(e);
    auto body = nlohmann::json::parse(e.value().body_json);
    CHECK(body["v"] == 1);  // first/oldest match
}

TEST_CASE("find_by_inference_id ignores non-string inference_id values") {
    auto p = tmp_db("find_typed");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("evt", "a",
        nlohmann::json{{"inference_id", 42}, {"i", 0}}, ""));
    REQUIRE(l.append("evt", "a",
        nlohmann::json{{"inference_id", "real_42"}, {"i", 1}}, ""));
    auto e = l.find_by_inference_id("real_42"); REQUIRE(e);
    auto body = nlohmann::json::parse(e.value().body_json);
    CHECK(body["i"] == 1);
}

TEST_CASE("find_by_inference_id survives reopen") {
    auto p = tmp_db("find_reopen");
    {
        auto l_ = Ledger::open(p); REQUIRE(l_);
        for (int i = 0; i < 50; i++) {
            REQUIRE(l_.value().append("evt", "a",
                nlohmann::json{{"inference_id", "inf_" + std::to_string(i)}}, ""));
        }
    }
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto e = l_.value().find_by_inference_id("inf_42"); REQUIRE(e);
    auto body = nlohmann::json::parse(e.value().body_json);
    CHECK(body["inference_id"] == "inf_42");
}

// ============== Ledger::range_by_event_type =============================

TEST_CASE("range_by_event_type returns matching entries in seq order") {
    auto p = tmp_db("ret_seq");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("alpha", "x", nlohmann::json{{"i", 1}}, ""));
    REQUIRE(l.append("beta",  "x", nlohmann::json{{"i", 2}}, ""));
    REQUIRE(l.append("alpha", "x", nlohmann::json{{"i", 3}}, ""));
    REQUIRE(l.append("alpha", "x", nlohmann::json{{"i", 4}}, ""));
    auto r = l.range_by_event_type("alpha"); REQUIRE(r);
    CHECK(r.value().size() == 3);
    CHECK(r.value()[0].header.seq == 1);
    CHECK(r.value()[1].header.seq == 3);
    CHECK(r.value()[2].header.seq == 4);
}

TEST_CASE("range_by_event_type empty event_type rejected") {
    auto p = tmp_db("ret_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto r = l_.value().range_by_event_type("");
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("range_by_event_type unknown type returns empty vector") {
    auto p = tmp_db("ret_unknown");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("alpha", "x", nlohmann::json::object(), ""));
    auto r = l.range_by_event_type("ghost"); REQUIRE(r);
    CHECK(r.value().empty());
}

// ============== Ledger::head_at_time ====================================

TEST_CASE("head_at_time on empty chain returns zero") {
    auto p = tmp_db("hat_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto h = l_.value().head_at_time(Time::now()); REQUIRE(h);
    CHECK(h.value().seq == 0);
}

TEST_CASE("head_at_time returns last entry seq for now") {
    auto p = tmp_db("hat_now");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 10; i++) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
    }
    auto h = l.head_at_time(Time::now() + std::chrono::nanoseconds{std::chrono::seconds{60}});
    REQUIRE(h);
    CHECK(h.value().seq == 10);
    CHECK(h.value().head_hash.hex() == l.head().hex());
}

TEST_CASE("head_at_time before chain start returns zero") {
    auto p = tmp_db("hat_before");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "x", nlohmann::json::object(), ""));
    // Time well before any entry.
    auto h = l.head_at_time(Time{0}); REQUIRE(h);
    CHECK(h.value().seq == 0);
}

TEST_CASE("head_at_time returns mid-chain head for in-between time") {
    auto p = tmp_db("hat_mid");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 5; i++) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    // Sleep, then more entries.
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    auto cutoff = Time::now();
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    for (int i = 5; i < 8; i++) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
    }
    auto h = l.head_at_time(cutoff); REQUIRE(h);
    CHECK(h.value().seq == 5);
}

// ============== Ledger::tail_by_event_type ==============================

TEST_CASE("tail_by_event_type returns last n matches most-recent first") {
    auto p = tmp_db("tbet_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 20; i++)
        REQUIRE(l.append(i % 2 ? "alpha" : "beta",
                         "x", nlohmann::json{{"i", i}}, ""));
    auto r = l.tail_by_event_type("alpha", 3); REQUIRE(r);
    CHECK(r.value().size() == 3);
    auto b0 = nlohmann::json::parse(r.value()[0].body_json);
    CHECK(b0["i"] == 19);
}

TEST_CASE("tail_by_event_type rejects empty type, n=0 returns empty") {
    auto p = tmp_db("tbet_edge");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "x", nlohmann::json::object(), ""));
    auto r1 = l.tail_by_event_type("", 5);
    CHECK(!r1);
    CHECK(r1.error().code() == ErrorCode::invalid_argument);
    auto r2 = l.tail_by_event_type("e", 0); REQUIRE(r2);
    CHECK(r2.value().empty());
}

// ============== Ledger::verify_range ====================================

TEST_CASE("verify_range succeeds on a valid sub-range") {
    auto p = tmp_db("vr_ok");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 10; i++)
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
    REQUIRE(l.verify_range(3, 8));
}

TEST_CASE("verify_range rejects bad bounds") {
    auto p = tmp_db("vr_bad");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "x", nlohmann::json::object(), ""));
    auto r1 = l.verify_range(0, 5);
    CHECK(!r1);
    CHECK(r1.error().code() == ErrorCode::invalid_argument);
    auto r2 = l.verify_range(5, 3);
    CHECK(!r2);
    auto r3 = l.verify_range(1, 100);
    CHECK(!r3);
}

TEST_CASE("verify_range matches verify() on full chain") {
    auto p = tmp_db("vr_full");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 5; i++)
        REQUIRE(l.append("e", "x", nlohmann::json::object(), ""));
    REQUIRE(l.verify_range(1, l.length() + 1));
    REQUIRE(l.verify());
}

// ============== Ledger::head_at_seq =====================================

TEST_CASE("head_at_seq returns entry_hash at the given seq") {
    auto p = tmp_db("has_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 5; i++)
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
    auto h = l.head_at_seq(3); REQUIRE(h);
    CHECK(h.value().seq == 3);
    auto e = l.at(3); REQUIRE(e);
    CHECK(h.value().head_hash.hex() == e.value().entry_hash().hex());
}

TEST_CASE("head_at_seq: out-of-range rejected") {
    auto p = tmp_db("has_oor");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "x", nlohmann::json::object(), ""));
    CHECK(!l.head_at_seq(0));
    CHECK(!l.head_at_seq(99));
}

TEST_CASE("KeyStore::fingerprint is stable across keystore copies") {
    auto k1 = KeyStore::generate();
    auto fp1 = k1.fingerprint();
    auto serialized = k1.serialize();
    auto k2 = KeyStore::deserialize(serialized).value();
    CHECK(k2.fingerprint() == fp1);
    CHECK(fp1.size() == 16);  // 8 bytes hex
}

TEST_CASE("KeyStore::fingerprint differs from key_id") {
    auto k = KeyStore::generate();
    CHECK(k.fingerprint() != k.key_id());
}

TEST_CASE("range_by_patient finds inferences for one patient only") {
    auto p = tmp_db("rbp_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("inference.committed", "x",
        nlohmann::json{{"inference_id", "i1"}, {"patient", "pat:alice"}}, ""));
    REQUIRE(l.append("inference.committed", "x",
        nlohmann::json{{"inference_id", "i2"}, {"patient", "pat:bob"}}, ""));
    REQUIRE(l.append("inference.committed", "x",
        nlohmann::json{{"inference_id", "i3"}, {"patient", "pat:alice"}}, ""));
    auto r = l.range_by_patient("pat:alice"); REQUIRE(r);
    CHECK(r.value().size() == 2);
    auto b0 = nlohmann::json::parse(r.value()[0].body_json);
    auto b1 = nlohmann::json::parse(r.value()[1].body_json);
    CHECK(b0["inference_id"] == "i1");
    CHECK(b1["inference_id"] == "i3");
}

TEST_CASE("range_by_patient skips non-inference events") {
    auto p = tmp_db("rbp_skip");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("consent.granted", "x",
        nlohmann::json{{"patient", "pat:alice"}}, ""));
    REQUIRE(l.append("inference.committed", "x",
        nlohmann::json{{"inference_id", "i1"}, {"patient", "pat:alice"}}, ""));
    auto r = l.range_by_patient("pat:alice"); REQUIRE(r);
    CHECK(r.value().size() == 1);
}

TEST_CASE("range_by_patient empty patient rejected") {
    auto p = tmp_db("rbp_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto r = l_.value().range_by_patient("");
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
}
