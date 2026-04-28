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

// ============== Ledger::tail_in_window ==================================

TEST_CASE("tail_in_window returns last n entries in [from, to) most-recent first") {
    auto p = tmp_db("tiw_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 5; i++) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    auto from = Time::now();
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
    for (int i = 5; i < 12; i++) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    auto to = Time::now() + std::chrono::nanoseconds{std::chrono::seconds{60}};
    auto r = l.tail_in_window(from, to, 3); REQUIRE(r);
    CHECK(r.value().size() == 3);
    auto b0 = nlohmann::json::parse(r.value()[0].body_json);
    CHECK(b0["i"] == 11);  // most-recent first
    auto b2 = nlohmann::json::parse(r.value()[2].body_json);
    CHECK(b2["i"] == 9);
}

TEST_CASE("tail_in_window edge cases: from > to and n=0") {
    auto p = tmp_db("tiw_edge");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "x", nlohmann::json::object(), ""));
    auto t1 = Time::now();
    auto t0 = t1 - std::chrono::nanoseconds{std::chrono::seconds{1}};
    auto r1 = l.tail_in_window(t1, t0, 5);
    CHECK(!r1);
    CHECK(r1.error().code() == ErrorCode::invalid_argument);
    auto r2 = l.tail_in_window(t0, t1, 0); REQUIRE(r2);
    CHECK(r2.value().empty());
}

TEST_CASE("tail_in_window respects half-open interval and chain integrity") {
    auto p = tmp_db("tiw_chain");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 8; i++) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    // Window covering everything from epoch up through the future.
    auto everything = l.tail_in_window(
        Time{0},
        Time::now() + std::chrono::nanoseconds{std::chrono::seconds{60}},
        100);
    REQUIRE(everything);
    CHECK(everything.value().size() == 8);
    // Half-open: an exclusive upper bound equal to an entry's ts must
    // exclude that entry. Use the third entry's ts as `to`; the result
    // should not include it.
    auto e3 = l.at(3); REQUIRE(e3);
    auto win = l.tail_in_window(Time{0}, e3.value().header.ts, 100);
    REQUIRE(win);
    for (const auto& e : win.value()) {
        CHECK(e.header.seq < 3);
    }
    // Chain still verifies after the window queries.
    REQUIRE(l.verify());
}

// ============== Ledger::has_entry =======================================

TEST_CASE("has_entry returns true for valid seqs and false otherwise") {
    auto p = tmp_db("he_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 4; i++)
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
    CHECK(l.has_entry(1));
    CHECK(l.has_entry(4));
    CHECK_FALSE(l.has_entry(5));
    CHECK_FALSE(l.has_entry(999));
}

TEST_CASE("has_entry: seq=0 is always false, including on empty chain") {
    auto p = tmp_db("he_zero");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    CHECK_FALSE(l.has_entry(0));
    CHECK_FALSE(l.has_entry(1));   // empty chain
    REQUIRE(l.append("e", "x", nlohmann::json::object(), ""));
    CHECK_FALSE(l.has_entry(0));   // still false post-append
    CHECK(l.has_entry(1));
}

TEST_CASE("has_entry tracks length() across appends and reopen") {
    auto p = tmp_db("he_chain");
    {
        auto l_ = Ledger::open(p); REQUIRE(l_);
        auto& l = l_.value();
        for (int i = 0; i < 3; i++)
            REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
        CHECK(l.has_entry(3));
        CHECK_FALSE(l.has_entry(4));
    }
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    CHECK(l.has_entry(3));
    CHECK_FALSE(l.has_entry(4));
    REQUIRE(l.append("e", "x", nlohmann::json::object(), ""));
    CHECK(l.has_entry(4));
}

// ============== Ledger::attest ==========================================

TEST_CASE("attest returns length, head, key_id, fingerprint") {
    auto p = tmp_db("att_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 3; i++)
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
    auto a = l.attest();
    CHECK(a.length == 3);
    CHECK(a.head.hex() == l.head().hex());
    CHECK(a.key_id == l.key_id());
    CHECK(a.fingerprint.size() == 16);
    CHECK(a.fingerprint != a.key_id);
}

TEST_CASE("attest on empty chain returns zero head") {
    auto p = tmp_db("att_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    auto a = l.attest();
    CHECK(a.length == 0);
    CHECK(a.head.hex() == Hash::zero().hex());
    CHECK_FALSE(a.key_id.empty());
    CHECK_FALSE(a.fingerprint.empty());
    auto j = nlohmann::json::parse(a.to_json());
    CHECK(j["length"] == 0);
    CHECK(j["head"] == Hash::zero().hex());
    CHECK(j["key_id"] == a.key_id);
    CHECK(j["fingerprint"] == a.fingerprint);
}

TEST_CASE("attest mirrors checkpoint length/head and persists across reopen") {
    auto p = tmp_db("att_chain");
    std::string fp_before;
    {
        auto l_ = Ledger::open(p); REQUIRE(l_);
        auto& l = l_.value();
        for (int i = 0; i < 4; i++)
            REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
        auto a  = l.attest();
        auto cp = l.checkpoint();
        CHECK(a.length == cp.seq);
        CHECK(a.head.hex() == cp.head_hash.hex());
        CHECK(a.key_id == cp.key_id);
        fp_before = a.fingerprint;
    }
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto a2 = l_.value().attest();
    CHECK(a2.length == 4);
    CHECK(a2.fingerprint == fp_before);
}

// ============== Ledger::tenants =========================================

TEST_CASE("tenants returns distinct tenant strings sorted alphabetically") {
    auto p = tmp_db("tenants_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "x", nlohmann::json::object(), "tenant:zulu"));
    REQUIRE(l.append("e", "x", nlohmann::json::object(), "tenant:alpha"));
    REQUIRE(l.append("e", "x", nlohmann::json::object(), "tenant:mike"));
    REQUIRE(l.append("e", "x", nlohmann::json::object(), "tenant:alpha"));  // dup
    auto r = l.tenants(); REQUIRE(r);
    REQUIRE(r.value().size() == 3);
    CHECK(r.value()[0] == "tenant:alpha");
    CHECK(r.value()[1] == "tenant:mike");
    CHECK(r.value()[2] == "tenant:zulu");
}

TEST_CASE("tenants includes the empty tenant when present") {
    auto p = tmp_db("tenants_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "x", nlohmann::json::object(), ""));
    REQUIRE(l.append("e", "x", nlohmann::json::object(), "tenant:b"));
    auto r = l.tenants(); REQUIRE(r);
    REQUIRE(r.value().size() == 2);
    CHECK(r.value()[0] == "");        // empty sorts first
    CHECK(r.value()[1] == "tenant:b");
}

TEST_CASE("tenants on empty ledger returns empty vector") {
    auto p = tmp_db("tenants_blank");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto r = l_.value().tenants(); REQUIRE(r);
    CHECK(r.value().empty());
}

// ============== Ledger::actors ==========================================

TEST_CASE("actors returns distinct actor strings sorted alphabetically") {
    auto p = tmp_db("actors_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "system:drift_monitor", nlohmann::json::object(), ""));
    REQUIRE(l.append("e", "clinician:42",         nlohmann::json::object(), ""));
    REQUIRE(l.append("e", "system:drift_monitor", nlohmann::json::object(), ""));
    REQUIRE(l.append("e", "system:consent",       nlohmann::json::object(), ""));
    auto r = l.actors(); REQUIRE(r);
    REQUIRE(r.value().size() == 3);
    CHECK(r.value()[0] == "clinician:42");
    CHECK(r.value()[1] == "system:consent");
    CHECK(r.value()[2] == "system:drift_monitor");
}

TEST_CASE("actors on empty ledger returns empty vector") {
    auto p = tmp_db("actors_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto r = l_.value().actors(); REQUIRE(r);
    CHECK(r.value().empty());
}

TEST_CASE("actors survives reopen and stays consistent with chain verify") {
    auto p = tmp_db("actors_chain");
    {
        auto l_ = Ledger::open(p); REQUIRE(l_);
        auto& l = l_.value();
        REQUIRE(l.append("e", "alice", nlohmann::json::object(), ""));
        REQUIRE(l.append("e", "bob",   nlohmann::json::object(), ""));
        REQUIRE(l.append("e", "alice", nlohmann::json::object(), ""));
    }
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    auto r = l.actors(); REQUIRE(r);
    REQUIRE(r.value().size() == 2);
    CHECK(r.value()[0] == "alice");
    CHECK(r.value()[1] == "bob");
    REQUIRE(l.verify());
}

// ============== Ledger::range_by_model ==================================

TEST_CASE("range_by_model finds inferences for one model only") {
    auto p = tmp_db("rbm_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("inference.committed", "x",
        nlohmann::json{{"inference_id", "i1"}, {"model", "model:scribe-v1"}}, ""));
    REQUIRE(l.append("inference.committed", "x",
        nlohmann::json{{"inference_id", "i2"}, {"model", "model:triage-v2"}}, ""));
    REQUIRE(l.append("inference.committed", "x",
        nlohmann::json{{"inference_id", "i3"}, {"model", "model:scribe-v1"}}, ""));
    auto r = l.range_by_model("model:scribe-v1"); REQUIRE(r);
    REQUIRE(r.value().size() == 2);
    auto b0 = nlohmann::json::parse(r.value()[0].body_json);
    auto b1 = nlohmann::json::parse(r.value()[1].body_json);
    CHECK(b0["inference_id"] == "i1");
    CHECK(b1["inference_id"] == "i3");
}

TEST_CASE("range_by_model empty model_id rejected") {
    auto p = tmp_db("rbm_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto r = l_.value().range_by_model("");
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("range_by_model skips non-inference events that mention the model") {
    auto p = tmp_db("rbm_nonmatch");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // A drift event that mentions the model_id in its body should not match;
    // only inference.committed events carry the canonical "model" field.
    REQUIRE(l.append("drift.crossed", "system:drift_monitor",
        nlohmann::json{{"note", "model:scribe-v1 saw a PSI cross"}}, ""));
    REQUIRE(l.append("inference.committed", "x",
        nlohmann::json{{"inference_id", "i1"}, {"model", "model:scribe-v1"}}, ""));
    REQUIRE(l.append("inference.committed", "x",
        nlohmann::json{{"inference_id", "i2"}, {"model", "model:other"}}, ""));
    auto r = l.range_by_model("model:scribe-v1"); REQUIRE(r);
    REQUIRE(r.value().size() == 1);
    auto b = nlohmann::json::parse(r.value()[0].body_json);
    CHECK(b["inference_id"] == "i1");
}

// ============== Ledger::count_in_window =================================

TEST_CASE("count_in_window counts entries in [from, to)") {
    auto p = tmp_db("ciw_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 4; i++) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    auto from = Time::now();
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
    for (int i = 4; i < 10; i++) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    auto to = Time::now() + std::chrono::nanoseconds{std::chrono::seconds{60}};
    auto r = l.count_in_window(from, to); REQUIRE(r);
    CHECK(r.value() == 6u);
}

TEST_CASE("count_in_window respects half-open interval and edge cases") {
    auto p = tmp_db("ciw_edge");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 5; i++) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    // Window everything: [epoch, far future) covers all 5.
    auto everything = l.count_in_window(
        Time{0},
        Time::now() + std::chrono::nanoseconds{std::chrono::seconds{60}});
    REQUIRE(everything);
    CHECK(everything.value() == 5u);

    // Half-open: an exclusive upper bound at entry 3's ts must exclude it.
    auto e3 = l.at(3); REQUIRE(e3);
    auto r = l.count_in_window(Time{0}, e3.value().header.ts);
    REQUIRE(r);
    CHECK(r.value() == 2u);  // entries 1 and 2 only

    // from == to → empty window.
    auto t = Time::now();
    auto r0 = l.count_in_window(t, t); REQUIRE(r0);
    CHECK(r0.value() == 0u);
}

TEST_CASE("count_in_window on empty ledger returns 0") {
    auto p = tmp_db("ciw_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    auto r = l.count_in_window(
        Time{0},
        Time::now() + std::chrono::nanoseconds{std::chrono::seconds{60}});
    REQUIRE(r);
    CHECK(r.value() == 0u);
    // And after a single append the count goes to 1.
    REQUIRE(l.append("e", "x", nlohmann::json::object(), ""));
    auto r2 = l.count_in_window(
        Time{0},
        Time::now() + std::chrono::nanoseconds{std::chrono::seconds{60}});
    REQUIRE(r2);
    CHECK(r2.value() == 1u);
}

// ============== Ledger::range_by_actor ===================================

TEST_CASE("range_by_actor returns matches in seq-ascending order") {
    auto p = tmp_db("rba_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "alice", nlohmann::json{{"i", 1}}, ""));
    REQUIRE(l.append("e", "bob",   nlohmann::json{{"i", 2}}, ""));
    REQUIRE(l.append("e", "alice", nlohmann::json{{"i", 3}}, ""));
    REQUIRE(l.append("e", "alice", nlohmann::json{{"i", 4}}, ""));
    auto r = l.range_by_actor("alice"); REQUIRE(r);
    REQUIRE(r.value().size() == 3);
    auto b0 = nlohmann::json::parse(r.value()[0].body_json);
    auto b1 = nlohmann::json::parse(r.value()[1].body_json);
    auto b2 = nlohmann::json::parse(r.value()[2].body_json);
    CHECK(b0["i"] == 1);  // oldest first
    CHECK(b1["i"] == 3);
    CHECK(b2["i"] == 4);
    // seq-ascending invariant.
    CHECK(r.value()[0].header.seq < r.value()[1].header.seq);
    CHECK(r.value()[1].header.seq < r.value()[2].header.seq);
}

TEST_CASE("range_by_actor: empty actor returns invalid_argument") {
    auto p = tmp_db("rba_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto r = l_.value().range_by_actor("");
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("range_by_actor: unknown actor returns empty vector, not error") {
    auto p = tmp_db("rba_miss");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "alice", nlohmann::json::object(), ""));
    auto r = l.range_by_actor("ghost"); REQUIRE(r);
    CHECK(r.value().empty());
}

TEST_CASE("range_by_actor: complement of tail_by_actor — same set, opposite order") {
    auto p = tmp_db("rba_complement");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 6; i++) {
        REQUIRE(l.append("e", (i % 2 == 0) ? "alice" : "bob",
                         nlohmann::json{{"i", i}}, ""));
    }
    auto rr = l.range_by_actor("alice"); REQUIRE(rr);
    auto tt = l.tail_by_actor("alice", 100); REQUIRE(tt);
    REQUIRE(rr.value().size() == tt.value().size());
    REQUIRE(rr.value().size() == 3);
    // Oldest-first vs. most-recent-first: reversed seqs match.
    for (std::size_t i = 0; i < rr.value().size(); ++i) {
        CHECK(rr.value()[i].header.seq
              == tt.value()[rr.value().size() - 1 - i].header.seq);
    }
}

// ============== Ledger::oldest_n =========================================

TEST_CASE("oldest_n returns first n entries seq-ascending") {
    auto p = tmp_db("oldn_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 10; i++)
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
    auto r = l.oldest_n(3); REQUIRE(r);
    REQUIRE(r.value().size() == 3);
    CHECK(r.value()[0].header.seq == 1);
    CHECK(r.value()[1].header.seq == 2);
    CHECK(r.value()[2].header.seq == 3);
}

TEST_CASE("oldest_n: n=0 returns empty vector") {
    auto p = tmp_db("oldn_zero");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "x", nlohmann::json::object(), ""));
    auto r = l.oldest_n(0); REQUIRE(r);
    CHECK(r.value().empty());
}

TEST_CASE("oldest_n: n larger than chain returns whole chain") {
    auto p = tmp_db("oldn_over");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 4; i++)
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
    auto r = l.oldest_n(100); REQUIRE(r);
    CHECK(r.value().size() == 4);
    CHECK(r.value()[0].header.seq == 1);
    CHECK(r.value()[3].header.seq == 4);
}

TEST_CASE("oldest_n: complement of tail — disjoint ends of chain") {
    auto p = tmp_db("oldn_complement");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 20; i++)
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
    auto o = l.oldest_n(5);  REQUIRE(o);
    auto t = l.tail(5);       REQUIRE(t);
    REQUIRE(o.value().size() == 5);
    REQUIRE(t.value().size() == 5);
    CHECK(o.value().front().header.seq == 1);
    CHECK(o.value().back().header.seq  == 5);
    // tail() is most-recent-first.
    CHECK(t.value().front().header.seq == 20);
    CHECK(t.value().back().header.seq  == 16);
}

TEST_CASE("oldest_n on empty ledger returns empty vector") {
    auto p = tmp_db("oldn_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto r = l_.value().oldest_n(5); REQUIRE(r);
    CHECK(r.value().empty());
}

// ============== Ledger::filter ===========================================

TEST_CASE("filter: matches event_type AND tenant") {
    auto p = tmp_db("filter_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("alpha", "x", nlohmann::json{{"i", 1}}, "t1"));
    REQUIRE(l.append("alpha", "x", nlohmann::json{{"i", 2}}, "t2"));
    REQUIRE(l.append("beta",  "x", nlohmann::json{{"i", 3}}, "t1"));
    REQUIRE(l.append("alpha", "x", nlohmann::json{{"i", 4}}, "t1"));
    auto r = l.filter("alpha", "t1"); REQUIRE(r);
    REQUIRE(r.value().size() == 2);
    auto b0 = nlohmann::json::parse(r.value()[0].body_json);
    auto b1 = nlohmann::json::parse(r.value()[1].body_json);
    CHECK(b0["i"] == 1);
    CHECK(b1["i"] == 4);
    for (const auto& e : r.value()) {
        CHECK(e.header.event_type == "alpha");
        CHECK(e.header.tenant     == "t1");
    }
}

TEST_CASE("filter: empty event_type returns invalid_argument") {
    auto p = tmp_db("filter_empty_evt");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto r = l_.value().filter("", "t1");
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("filter: empty tenant is its own scope") {
    auto p = tmp_db("filter_empty_tenant");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("alpha", "x", nlohmann::json::object(), ""));
    REQUIRE(l.append("alpha", "x", nlohmann::json::object(), "t1"));
    REQUIRE(l.append("alpha", "x", nlohmann::json::object(), ""));
    auto r = l.filter("alpha", ""); REQUIRE(r);
    CHECK(r.value().size() == 2);
    auto rt = l.filter("alpha", "t1"); REQUIRE(rt);
    CHECK(rt.value().size() == 1);
}

TEST_CASE("filter: no matches returns empty vector") {
    auto p = tmp_db("filter_miss");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("alpha", "x", nlohmann::json::object(), "t1"));
    auto r = l.filter("beta", "t1"); REQUIRE(r);
    CHECK(r.value().empty());
    auto r2 = l.filter("alpha", "t2"); REQUIRE(r2);
    CHECK(r2.value().empty());
}

// ============== Ledger::byte_size_for_tenant =============================

TEST_CASE("byte_size_for_tenant sums body sizes for matching tenant") {
    auto p = tmp_db("bsft_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "x", nlohmann::json{{"k", "aaa"}}, "alpha"));
    REQUIRE(l.append("e", "x", nlohmann::json{{"k", "bb"}},  "beta"));
    REQUIRE(l.append("e", "x", nlohmann::json{{"k", "cccc"}}, "alpha"));

    // Cross-check against stats_for_tenant.total_body_bytes.
    auto sa = l.stats_for_tenant("alpha"); REQUIRE(sa);
    auto sb = l.stats_for_tenant("beta");  REQUIRE(sb);

    auto ba = l.byte_size_for_tenant("alpha"); REQUIRE(ba);
    auto bb = l.byte_size_for_tenant("beta");  REQUIRE(bb);
    CHECK(ba.value() == sa.value().total_body_bytes);
    CHECK(bb.value() == sb.value().total_body_bytes);
    CHECK(ba.value() > 0);
    CHECK(bb.value() > 0);
}

TEST_CASE("byte_size_for_tenant: empty tenant is its own scope") {
    auto p = tmp_db("bsft_empty_scope");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "x", nlohmann::json{{"k", "z"}}, ""));
    REQUIRE(l.append("e", "x", nlohmann::json{{"k", "z"}}, "alpha"));
    auto be = l.byte_size_for_tenant(""); REQUIRE(be);
    auto ba = l.byte_size_for_tenant("alpha"); REQUIRE(ba);
    CHECK(be.value() > 0);
    CHECK(ba.value() > 0);
    // Each scope counts only its own bodies.
    auto se = l.stats_for_tenant(""); REQUIRE(se);
    CHECK(be.value() == se.value().total_body_bytes);
}

TEST_CASE("byte_size_for_tenant on empty ledger returns 0") {
    auto p = tmp_db("bsft_empty_chain");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto r = l_.value().byte_size_for_tenant("alpha"); REQUIRE(r);
    CHECK(r.value() == 0u);
}

TEST_CASE("byte_size_for_tenant: unknown tenant returns 0") {
    auto p = tmp_db("bsft_miss");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "x", nlohmann::json{{"k", "z"}}, "alpha"));
    auto r = l.byte_size_for_tenant("gamma"); REQUIRE(r);
    CHECK(r.value() == 0u);
}

TEST_CASE("byte_size_for_tenant: large multi-chunk scan still correct") {
    auto p = tmp_db("bsft_chunk");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // > kChunk (1024) entries to exercise the pagination loop.
    for (int i = 0; i < 1500; i++) {
        REQUIRE(l.append("e", "x",
            nlohmann::json{{"i", i}},
            (i % 3 == 0) ? "alpha" : "beta"));
    }
    auto ba = l.byte_size_for_tenant("alpha"); REQUIRE(ba);
    auto bb = l.byte_size_for_tenant("beta");  REQUIRE(bb);
    auto sa = l.stats_for_tenant("alpha"); REQUIRE(sa);
    auto sb = l.stats_for_tenant("beta");  REQUIRE(sb);
    CHECK(ba.value() == sa.value().total_body_bytes);
    CHECK(bb.value() == sb.value().total_body_bytes);
}

// ============== Ledger::find_first_by_event_type =========================

TEST_CASE("find_first_by_event_type returns oldest matching entry") {
    auto p = tmp_db("ffbet_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("beta",  "x", nlohmann::json{{"i", 1}}, ""));
    REQUIRE(l.append("alpha", "x", nlohmann::json{{"i", 2}}, ""));
    REQUIRE(l.append("alpha", "x", nlohmann::json{{"i", 3}}, ""));
    REQUIRE(l.append("beta",  "x", nlohmann::json{{"i", 4}}, ""));
    auto r = l.find_first_by_event_type("alpha"); REQUIRE(r);
    CHECK(r.value().header.seq == 2);
    auto body = nlohmann::json::parse(r.value().body_json);
    CHECK(body["i"] == 2);
}

TEST_CASE("find_first_by_event_type empty type rejected, missing returns not_found") {
    auto p = tmp_db("ffbet_edge");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("alpha", "x", nlohmann::json::object(), ""));
    auto r1 = l.find_first_by_event_type("");
    CHECK(!r1);
    CHECK(r1.error().code() == ErrorCode::invalid_argument);
    auto r2 = l.find_first_by_event_type("ghost");
    CHECK(!r2);
    CHECK(r2.error().code() == ErrorCode::not_found);
}

TEST_CASE("find_first_by_event_type on empty ledger returns not_found") {
    auto p = tmp_db("ffbet_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto r = l_.value().find_first_by_event_type("anything");
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::not_found);
}

TEST_CASE("find_first_by_event_type matches range_by_event_type[0]") {
    auto p = tmp_db("ffbet_consistent");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 12; i++) {
        REQUIRE(l.append(i % 3 == 0 ? "alpha" : "beta",
                         "x", nlohmann::json{{"i", i}}, ""));
    }
    auto rng = l.range_by_event_type("alpha"); REQUIRE(rng);
    REQUIRE(!rng.value().empty());
    auto first = l.find_first_by_event_type("alpha"); REQUIRE(first);
    CHECK(first.value().header.seq == rng.value().front().header.seq);
}

// ============== Ledger::byte_size_per_tenant =============================

TEST_CASE("byte_size_per_tenant sums per tenant across the chain") {
    auto p = tmp_db("bspt_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    auto e1 = l.append("e", "x", nlohmann::json{{"k", "a"}}, "alpha"); REQUIRE(e1);
    auto e2 = l.append("e", "x", nlohmann::json{{"k", "bb"}}, "alpha"); REQUIRE(e2);
    auto e3 = l.append("e", "x", nlohmann::json{{"k", "ccc"}}, "beta");  REQUIRE(e3);
    auto r = l.byte_size_per_tenant(); REQUIRE(r);
    const auto& m = r.value();
    REQUIRE(m.count("alpha") == 1);
    REQUIRE(m.count("beta")  == 1);
    CHECK(m.at("alpha") == e1.value().body_json.size() + e2.value().body_json.size());
    CHECK(m.at("beta")  == e3.value().body_json.size());
}

TEST_CASE("byte_size_per_tenant: empty tenant gets its own bucket") {
    auto p = tmp_db("bspt_empty_bucket");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    auto e1 = l.append("e", "x", nlohmann::json{{"k", "a"}}, "");      REQUIRE(e1);
    auto e2 = l.append("e", "x", nlohmann::json{{"k", "bb"}}, "alpha"); REQUIRE(e2);
    auto r = l.byte_size_per_tenant(); REQUIRE(r);
    REQUIRE(r.value().count("") == 1);
    REQUIRE(r.value().count("alpha") == 1);
    CHECK(r.value().at("")      == e1.value().body_json.size());
    CHECK(r.value().at("alpha") == e2.value().body_json.size());
}

TEST_CASE("byte_size_per_tenant on empty ledger returns empty map") {
    auto p = tmp_db("bspt_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto r = l_.value().byte_size_per_tenant(); REQUIRE(r);
    CHECK(r.value().empty());
}

TEST_CASE("byte_size_per_tenant agrees with byte_size_for_tenant per key") {
    auto p = tmp_db("bspt_consistent");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 50; i++) {
        REQUIRE(l.append("e", "x",
            nlohmann::json{{"i", i}},
            (i % 3 == 0) ? "alpha" : ((i % 3 == 1) ? "beta" : "")));
    }
    auto agg = l.byte_size_per_tenant(); REQUIRE(agg);
    for (const auto& [tenant, total] : agg.value()) {
        auto solo = l.byte_size_for_tenant(tenant); REQUIRE(solo);
        CHECK(solo.value() == total);
    }
}

// ============== Ledger::most_active_actors ===============================

TEST_CASE("most_active_actors returns top n by count desc") {
    auto p = tmp_db("maa_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // alice: 5, bob: 3, carol: 1
    for (int i = 0; i < 5; i++) REQUIRE(l.append("e", "alice", nlohmann::json::object(), ""));
    for (int i = 0; i < 3; i++) REQUIRE(l.append("e", "bob",   nlohmann::json::object(), ""));
    REQUIRE(l.append("e", "carol", nlohmann::json::object(), ""));
    auto r = l.most_active_actors(2); REQUIRE(r);
    REQUIRE(r.value().size() == 2);
    CHECK(r.value()[0].first  == "alice");
    CHECK(r.value()[0].second == 5u);
    CHECK(r.value()[1].first  == "bob");
    CHECK(r.value()[1].second == 3u);
}

TEST_CASE("most_active_actors: n=0 returns empty, n>distinct returns all") {
    auto p = tmp_db("maa_edge");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "alice", nlohmann::json::object(), ""));
    REQUIRE(l.append("e", "bob",   nlohmann::json::object(), ""));
    auto r0 = l.most_active_actors(0); REQUIRE(r0);
    CHECK(r0.value().empty());
    auto r99 = l.most_active_actors(99); REQUIRE(r99);
    CHECK(r99.value().size() == 2);
}

TEST_CASE("most_active_actors on empty ledger returns empty vector") {
    auto p = tmp_db("maa_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto r = l_.value().most_active_actors(5); REQUIRE(r);
    CHECK(r.value().empty());
}

TEST_CASE("most_active_actors ties break alphabetically") {
    auto p = tmp_db("maa_ties");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // bob and alice both at 2 — alice should come first alphabetically.
    REQUIRE(l.append("e", "bob",   nlohmann::json::object(), ""));
    REQUIRE(l.append("e", "alice", nlohmann::json::object(), ""));
    REQUIRE(l.append("e", "bob",   nlohmann::json::object(), ""));
    REQUIRE(l.append("e", "alice", nlohmann::json::object(), ""));
    auto r = l.most_active_actors(2); REQUIRE(r);
    REQUIRE(r.value().size() == 2);
    CHECK(r.value()[0].first == "alice");
    CHECK(r.value()[1].first == "bob");
}

// ============== Ledger::has_event_type ===================================

TEST_CASE("has_event_type true when event present") {
    auto p = tmp_db("het_present");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("alpha", "x", nlohmann::json::object(), ""));
    REQUIRE(l.append("beta",  "x", nlohmann::json::object(), ""));
    CHECK(l.has_event_type("alpha"));
    CHECK(l.has_event_type("beta"));
    CHECK_FALSE(l.has_event_type("ghost"));
}

TEST_CASE("has_event_type: empty string and empty ledger return false") {
    auto p = tmp_db("het_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    CHECK_FALSE(l.has_event_type(""));
    CHECK_FALSE(l.has_event_type("anything"));
    REQUIRE(l.append("alpha", "x", nlohmann::json::object(), ""));
    CHECK_FALSE(l.has_event_type(""));
}

TEST_CASE("has_event_type agrees with count_by_event_type presence") {
    auto p = tmp_db("het_consistent");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 30; i++) {
        REQUIRE(l.append(i % 4 == 0 ? "alpha" : "beta",
                         "x", nlohmann::json{{"i", i}}, ""));
    }
    auto counts = l.count_by_event_type(); REQUIRE(counts);
    CHECK(l.has_event_type("alpha") == (counts.value().count("alpha") > 0));
    CHECK(l.has_event_type("beta")  == (counts.value().count("beta")  > 0));
    CHECK(l.has_event_type("gamma") == (counts.value().count("gamma") > 0));
}

// ============== Ledger::tail_for_patient ================================

TEST_CASE("tail_for_patient returns last n inferences most-recent first") {
    auto p = tmp_db("tfp_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 6; i++) {
        REQUIRE(l.append("inference.committed", "x",
            nlohmann::json{{"inference_id", "i" + std::to_string(i)},
                           {"patient", "pat:alice"}}, ""));
    }
    REQUIRE(l.append("inference.committed", "x",
        nlohmann::json{{"inference_id", "ib"}, {"patient", "pat:bob"}}, ""));
    auto r = l.tail_for_patient("pat:alice", 3); REQUIRE(r);
    REQUIRE(r.value().size() == 3);
    auto b0 = nlohmann::json::parse(r.value()[0].body_json);
    auto b1 = nlohmann::json::parse(r.value()[1].body_json);
    auto b2 = nlohmann::json::parse(r.value()[2].body_json);
    CHECK(b0["inference_id"] == "i5");  // most recent alice
    CHECK(b1["inference_id"] == "i4");
    CHECK(b2["inference_id"] == "i3");
}

TEST_CASE("tail_for_patient: empty patient + n=0 edge cases") {
    auto p = tmp_db("tfp_edge");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("inference.committed", "x",
        nlohmann::json{{"inference_id", "i1"}, {"patient", "pat:alice"}}, ""));
    auto r1 = l.tail_for_patient("", 5);
    CHECK(!r1);
    CHECK(r1.error().code() == ErrorCode::invalid_argument);
    auto r2 = l.tail_for_patient("pat:alice", 0); REQUIRE(r2);
    CHECK(r2.value().empty());
    auto r3 = l.tail_for_patient("pat:ghost", 10); REQUIRE(r3);
    CHECK(r3.value().empty());
}

TEST_CASE("tail_for_patient skips non-inference events and is complement of range_by_patient") {
    auto p = tmp_db("tfp_complement");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("consent.granted", "x",
        nlohmann::json{{"patient", "pat:alice"}}, ""));
    for (int i = 0; i < 4; i++) {
        REQUIRE(l.append("inference.committed", "x",
            nlohmann::json{{"inference_id", "i" + std::to_string(i)},
                           {"patient", "pat:alice"}}, ""));
    }
    auto rr = l.range_by_patient("pat:alice"); REQUIRE(rr);
    auto tt = l.tail_for_patient("pat:alice", 100); REQUIRE(tt);
    REQUIRE(rr.value().size() == 4);
    REQUIRE(tt.value().size() == 4);
    // Same set, opposite order.
    for (std::size_t i = 0; i < rr.value().size(); ++i) {
        CHECK(rr.value()[i].header.seq
              == tt.value()[rr.value().size() - 1 - i].header.seq);
    }
}

// ============== Ledger::range_for_patient_in_window =====================

TEST_CASE("range_for_patient_in_window returns matches in [from, to) seq-asc") {
    auto p = tmp_db("rfpw_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 3; i++) {
        REQUIRE(l.append("inference.committed", "x",
            nlohmann::json{{"inference_id", "i" + std::to_string(i)},
                           {"patient", "pat:alice"}}, ""));
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    auto from = Time::now();
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
    for (int i = 3; i < 6; i++) {
        REQUIRE(l.append("inference.committed", "x",
            nlohmann::json{{"inference_id", "i" + std::to_string(i)},
                           {"patient", "pat:alice"}}, ""));
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    auto to = Time::now() + std::chrono::nanoseconds{std::chrono::seconds{60}};
    auto r = l.range_for_patient_in_window("pat:alice", from, to); REQUIRE(r);
    REQUIRE(r.value().size() == 3);
    // seq-ascending invariant.
    CHECK(r.value()[0].header.seq < r.value()[1].header.seq);
    CHECK(r.value()[1].header.seq < r.value()[2].header.seq);
    auto b0 = nlohmann::json::parse(r.value()[0].body_json);
    CHECK(b0["inference_id"] == "i3");
}

TEST_CASE("range_for_patient_in_window edge cases: empty patient, from > to") {
    auto p = tmp_db("rfpw_edge");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("inference.committed", "x",
        nlohmann::json{{"inference_id", "i1"}, {"patient", "pat:alice"}}, ""));
    auto t1 = Time::now();
    auto t0 = t1 - std::chrono::nanoseconds{std::chrono::seconds{1}};
    auto r1 = l.range_for_patient_in_window("", t0, t1);
    CHECK(!r1);
    CHECK(r1.error().code() == ErrorCode::invalid_argument);
    auto r2 = l.range_for_patient_in_window("pat:alice", t1, t0);
    CHECK(!r2);
    CHECK(r2.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("range_for_patient_in_window filters non-matching patients and event types") {
    auto p = tmp_db("rfpw_filter");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    auto t0 = Time::now() - std::chrono::nanoseconds{std::chrono::seconds{1}};
    REQUIRE(l.append("consent.granted", "x",
        nlohmann::json{{"patient", "pat:alice"}}, ""));
    REQUIRE(l.append("inference.committed", "x",
        nlohmann::json{{"inference_id", "i1"}, {"patient", "pat:alice"}}, ""));
    REQUIRE(l.append("inference.committed", "x",
        nlohmann::json{{"inference_id", "i2"}, {"patient", "pat:bob"}}, ""));
    REQUIRE(l.append("inference.committed", "x",
        nlohmann::json{{"inference_id", "i3"}, {"patient", "pat:alice"}}, ""));
    auto t1 = Time::now() + std::chrono::nanoseconds{std::chrono::seconds{60}};
    auto r = l.range_for_patient_in_window("pat:alice", t0, t1); REQUIRE(r);
    REQUIRE(r.value().size() == 2);
    auto b0 = nlohmann::json::parse(r.value()[0].body_json);
    auto b1 = nlohmann::json::parse(r.value()[1].body_json);
    CHECK(b0["inference_id"] == "i1");
    CHECK(b1["inference_id"] == "i3");
}

// ============== Ledger::events_after_seq ================================

TEST_CASE("events_after_seq returns the tail of the chain after a cursor") {
    auto p = tmp_db("eas_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 10; i++)
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
    auto r = l.events_after_seq(7); REQUIRE(r);
    REQUIRE(r.value().size() == 3);
    CHECK(r.value()[0].header.seq == 8);
    CHECK(r.value()[1].header.seq == 9);
    CHECK(r.value()[2].header.seq == 10);
}

TEST_CASE("events_after_seq edge cases: 0, at-tail, past-tail") {
    auto p = tmp_db("eas_edge");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 5; i++)
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
    // 0 → entire chain.
    auto r0 = l.events_after_seq(0); REQUIRE(r0);
    CHECK(r0.value().size() == 5);
    CHECK(r0.value()[0].header.seq == 1);
    // At length → empty (caller is caught up).
    auto r5 = l.events_after_seq(5); REQUIRE(r5);
    CHECK(r5.value().empty());
    // Past length → empty (no error).
    auto r99 = l.events_after_seq(99); REQUIRE(r99);
    CHECK(r99.value().empty());
}

TEST_CASE("events_after_seq supports incremental follower replay") {
    auto p = tmp_db("eas_follower");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 8; i++)
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
    // Follower sees 0..3 already, then catches up.
    std::uint64_t cursor = 3;
    auto delta1 = l.events_after_seq(cursor); REQUIRE(delta1);
    CHECK(delta1.value().size() == 5);
    cursor = delta1.value().back().header.seq;
    CHECK(cursor == 8);
    // More appends, follower catches up again.
    for (int i = 8; i < 11; i++)
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
    auto delta2 = l.events_after_seq(cursor); REQUIRE(delta2);
    REQUIRE(delta2.value().size() == 3);
    CHECK(delta2.value()[0].header.seq == 9);
    CHECK(delta2.value()[2].header.seq == 11);
}

// ============== Ledger::content_address =================================

TEST_CASE("content_address returns the entry_hash for a given seq") {
    auto p = tmp_db("ca_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 4; i++)
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
    auto h = l.content_address(2); REQUIRE(h);
    auto e = l.at(2); REQUIRE(e);
    CHECK(h.value().hex() == e.value().entry_hash().hex());
}

TEST_CASE("content_address: out-of-range seq rejected") {
    auto p = tmp_db("ca_oor");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "x", nlohmann::json::object(), ""));
    auto r0 = l.content_address(0);
    CHECK(!r0);
    CHECK(r0.error().code() == ErrorCode::invalid_argument);
    auto r99 = l.content_address(99);
    CHECK(!r99);
    CHECK(r99.error().code() == ErrorCode::invalid_argument);
    // Empty chain: any seq is out of range.
    auto p2 = tmp_db("ca_empty");
    auto l2_ = Ledger::open(p2); REQUIRE(l2_);
    auto re = l2_.value().content_address(1);
    CHECK(!re);
    CHECK(re.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("content_address agrees with head_at_seq.head_hash and chains forward") {
    auto p = tmp_db("ca_chain");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 5; i++)
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
    for (std::uint64_t s = 1; s <= 5; s++) {
        auto h = l.content_address(s); REQUIRE(h);
        auto hs = l.head_at_seq(s); REQUIRE(hs);
        CHECK(h.value().hex() == hs.value().head_hash.hex());
    }
    // The next entry's prev_hash equals content_address(prev seq).
    for (std::uint64_t s = 2; s <= 5; s++) {
        auto e = l.at(s); REQUIRE(e);
        auto prev = l.content_address(s - 1); REQUIRE(prev);
        CHECK(e.value().header.prev_hash.hex() == prev.value().hex());
    }
}

// ---- oldest_entry / newest_entry ----------------------------------------

TEST_CASE("oldest_entry returns seq=1 on a populated chain") {
    auto p = tmp_db("oldest_entry_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 4; i++) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
    }
    auto e = l.oldest_entry();
    REQUIRE(e);
    CHECK(e.value().header.seq == 1);
    // body matches the first append (i==0).
    auto body = nlohmann::json::parse(e.value().body_json);
    CHECK(body.at("i").get<int>() == 0);
}

TEST_CASE("oldest_entry on an empty chain returns not_found") {
    auto p = tmp_db("oldest_entry_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto e = l_.value().oldest_entry();
    REQUIRE(!e);
    CHECK(e.error().code() == ErrorCode::not_found);
}

TEST_CASE("oldest_entry agrees with at(1) and survives reopen") {
    auto p = tmp_db("oldest_entry_persist");
    {
        auto l_ = Ledger::open(p); REQUIRE(l_);
        auto& l = l_.value();
        for (int i = 0; i < 6; i++) {
            REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
        }
    }
    auto l2 = Ledger::open(p); REQUIRE(l2);
    auto a = l2.value().at(1);             REQUIRE(a);
    auto o = l2.value().oldest_entry();    REQUIRE(o);
    CHECK(o.value().entry_hash().hex() == a.value().entry_hash().hex());
    CHECK(o.value().header.seq == 1);
}

TEST_CASE("newest_entry returns seq=length on a populated chain") {
    auto p = tmp_db("newest_entry_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 7; i++) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
    }
    auto e = l.newest_entry();
    REQUIRE(e);
    CHECK(e.value().header.seq == l.length());
    CHECK(e.value().entry_hash().hex() == l.head().hex());
}

TEST_CASE("newest_entry on an empty chain returns not_found") {
    auto p = tmp_db("newest_entry_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto e = l_.value().newest_entry();
    REQUIRE(!e);
    CHECK(e.error().code() == ErrorCode::not_found);
}

TEST_CASE("newest_entry tracks the head after each append") {
    auto p = tmp_db("newest_entry_tracks");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 5; i++) {
        auto a = l.append("e", "x", nlohmann::json{{"i", i}}, "");
        REQUIRE(a);
        auto n = l.newest_entry(); REQUIRE(n);
        CHECK(n.value().header.seq == static_cast<std::uint64_t>(i + 1));
        CHECK(n.value().entry_hash().hex() == a.value().entry_hash().hex());
        CHECK(n.value().entry_hash().hex() == l.head().hex());
    }
}

// ---- seq_at_time --------------------------------------------------------

TEST_CASE("seq_at_time returns largest seq with ts <= t") {
    auto p = tmp_db("sat_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // Insert 4 entries with a small sleep so timestamps are strictly
    // increasing on every reasonable clock.
    std::vector<Time> times;
    for (int i = 0; i < 4; i++) {
        auto a = l.append("e", "x", nlohmann::json{{"i", i}}, "");
        REQUIRE(a);
        times.push_back(a.value().header.ts);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    // t == ts of entry 3 → seq 3.
    auto r3 = l.seq_at_time(times[2]); REQUIRE(r3);
    CHECK(r3.value() == 3);
    // t far in the future → newest.
    auto rfut = l.seq_at_time(Time::now() + std::chrono::hours(1));
    REQUIRE(rfut);
    CHECK(rfut.value() == l.length());
    // t between entries 2 and 3 (just before ts[2]) → 2.
    auto rmid = l.seq_at_time(times[2] - std::chrono::nanoseconds(1));
    REQUIRE(rmid);
    CHECK(rmid.value() == 2);
}

TEST_CASE("seq_at_time returns 0 for empty chain or pre-genesis time") {
    auto p = tmp_db("sat_edge");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // Empty chain, any t.
    auto r0 = l.seq_at_time(Time::now()); REQUIRE(r0);
    CHECK(r0.value() == 0);
    // Append, then ask for a t earlier than the first entry.
    auto a = l.append("e", "x", nlohmann::json{{"i", 0}}, ""); REQUIRE(a);
    auto rprior = l.seq_at_time(a.value().header.ts - std::chrono::nanoseconds(1));
    REQUIRE(rprior);
    CHECK(rprior.value() == 0);
}

TEST_CASE("seq_at_time agrees with head_at_time for the same t") {
    auto p = tmp_db("sat_vs_head_at_time");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 6; i++) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    auto t = Time::now();
    auto sa = l.seq_at_time(t);   REQUIRE(sa);
    auto ha = l.head_at_time(t);  REQUIRE(ha);
    CHECK(sa.value() == ha.value().seq);
}

// ---- inclusion_proof ----------------------------------------------------

TEST_CASE("inclusion_proof basic: chain_to_head replays prev_hash linkage") {
    auto p = tmp_db("ip_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 6; i++) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
    }
    auto pr = l.inclusion_proof(2); REQUIRE(pr);
    const auto& proof = pr.value();
    CHECK(proof.seq == 2);
    CHECK(proof.head_seq == 6);
    CHECK(proof.head_hash.hex() == l.head().hex());
    // chain_to_head covers seq+1 .. head_seq.
    REQUIRE(proof.chain_to_head.size() == 4);  // seqs 3,4,5,6

    // Replay: each entry at seq+1+k has prev_hash equal to entry_hash
    // of the prior step. Step 0's prior is entry_hash for seq=2.
    Hash prior = proof.entry_hash;
    for (std::size_t k = 0; k < proof.chain_to_head.size(); k++) {
        auto e = l.at(proof.seq + 1 + k); REQUIRE(e);
        CHECK(e.value().header.prev_hash.hex() == prior.hex());
        // entry_hash in the proof equals the live recompute.
        CHECK(proof.chain_to_head[k].hex() == e.value().entry_hash().hex());
        prior = proof.chain_to_head[k];
    }
    // Final replayed hash equals the live head.
    CHECK(prior.hex() == proof.head_hash.hex());
}

TEST_CASE("inclusion_proof edges: invalid_argument on seq=0 / out of range / empty") {
    auto p = tmp_db("ip_edge");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // Empty chain: seq=1 out of range.
    auto e0 = l.inclusion_proof(1);
    REQUIRE(!e0);
    CHECK(e0.error().code() == ErrorCode::invalid_argument);
    for (int i = 0; i < 3; i++) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
    }
    auto ezero = l.inclusion_proof(0);
    REQUIRE(!ezero);
    CHECK(ezero.error().code() == ErrorCode::invalid_argument);
    auto eover = l.inclusion_proof(99);
    REQUIRE(!eover);
    CHECK(eover.error().code() == ErrorCode::invalid_argument);
    // seq == length() is valid: chain_to_head is empty.
    auto eend = l.inclusion_proof(l.length());
    REQUIRE(eend);
    CHECK(eend.value().chain_to_head.empty());
    CHECK(eend.value().entry_hash.hex() == eend.value().head_hash.hex());
}

TEST_CASE("inclusion_proof: to_json round-trips fields and hex digests") {
    auto p = tmp_db("ip_json");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 4; i++) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
    }
    auto pr = l.inclusion_proof(1); REQUIRE(pr);
    const auto& proof = pr.value();
    auto j = nlohmann::json::parse(proof.to_json());
    CHECK(j.at("seq").get<std::uint64_t>() == 1u);
    CHECK(j.at("head_seq").get<std::uint64_t>() == 4u);
    CHECK(j.at("entry_hash").get<std::string>() == proof.entry_hash.hex());
    CHECK(j.at("head_hash").get<std::string>() == proof.head_hash.hex());
    auto chain = j.at("chain_to_head");
    REQUIRE(chain.is_array());
    REQUIRE(chain.size() == proof.chain_to_head.size());
    for (std::size_t k = 0; k < chain.size(); k++) {
        CHECK(chain[k].get<std::string>() == proof.chain_to_head[k].hex());
    }
}

// ---- oldest_n_for_tenant ------------------------------------------------

TEST_CASE("oldest_n_for_tenant: basic seq-ascending tenant slice") {
    auto p = tmp_db("oldest_n_for_tenant_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // Interleave two tenants so ordering is non-trivial.
    REQUIRE(l.append("e", "x", nlohmann::json{{"i", 0}}, "alpha"));
    REQUIRE(l.append("e", "x", nlohmann::json{{"i", 1}}, "beta"));
    REQUIRE(l.append("e", "x", nlohmann::json{{"i", 2}}, "alpha"));
    REQUIRE(l.append("e", "x", nlohmann::json{{"i", 3}}, "beta"));
    REQUIRE(l.append("e", "x", nlohmann::json{{"i", 4}}, "alpha"));

    auto first2 = l.oldest_n_for_tenant("alpha", 2);
    REQUIRE(first2);
    REQUIRE(first2.value().size() == 2);
    CHECK(first2.value()[0].header.seq == 1u);
    CHECK(first2.value()[1].header.seq == 3u);
    // Oldest first.
    CHECK(first2.value()[0].header.seq < first2.value()[1].header.seq);
}

TEST_CASE("oldest_n_for_tenant edges: n=0, empty chain, larger n than matches, empty tenant scope") {
    auto p = tmp_db("oldest_n_for_tenant_edges");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // Empty chain — n=0 and n>0 both give empty.
    auto a = l.oldest_n_for_tenant("alpha", 0);
    REQUIRE(a); CHECK(a.value().empty());
    auto b = l.oldest_n_for_tenant("alpha", 5);
    REQUIRE(b); CHECK(b.value().empty());

    // One alpha, two empty-tenant.
    REQUIRE(l.append("e", "x", nlohmann::json{{"i", 0}}, "alpha"));
    REQUIRE(l.append("e", "x", nlohmann::json{{"i", 1}}, ""));
    REQUIRE(l.append("e", "x", nlohmann::json{{"i", 2}}, ""));

    // n=0 cheap no-op.
    auto z = l.oldest_n_for_tenant("alpha", 0);
    REQUIRE(z); CHECK(z.value().empty());

    // n much larger than matches: returns only the matches.
    auto big = l.oldest_n_for_tenant("alpha", 100);
    REQUIRE(big); REQUIRE(big.value().size() == 1u);
    CHECK(big.value()[0].header.seq == 1u);

    // Empty tenant ("") is its own scope.
    auto empt = l.oldest_n_for_tenant("", 10);
    REQUIRE(empt); REQUIRE(empt.value().size() == 2u);
    CHECK(empt.value()[0].header.seq == 2u);
    CHECK(empt.value()[1].header.seq == 3u);
}

TEST_CASE("oldest_n_for_tenant integration: paginates across the 1024 chunk size") {
    auto p = tmp_db("oldest_n_for_tenant_paginate");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // Append 2050 entries, all under tenant "t". This forces the chunked
    // loop (kChunk=1024) to run at least 2 full chunks before stopping.
    constexpr int N = 2050;
    for (int i = 0; i < N; i++) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, "t"));
    }
    // Ask for n=1500, which spans two chunk boundaries.
    auto r = l.oldest_n_for_tenant("t", 1500);
    REQUIRE(r);
    REQUIRE(r.value().size() == 1500u);
    CHECK(r.value().front().header.seq == 1u);
    CHECK(r.value().back().header.seq == 1500u);
    // Strictly seq-ascending.
    for (std::size_t k = 1; k < r.value().size(); k++) {
        CHECK(r.value()[k - 1].header.seq < r.value()[k].header.seq);
    }
    // Asking for more than exist returns just the available set.
    auto all = l.oldest_n_for_tenant("t", N + 999);
    REQUIRE(all);
    CHECK(all.value().size() == static_cast<std::size_t>(N));
}

// ---- events_between -----------------------------------------------------

TEST_CASE("events_between: basic time + event_type filter") {
    auto p = tmp_db("events_between_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    auto t0 = Time::now();
    std::this_thread::sleep_for(std::chrono::milliseconds{2});
    REQUIRE(l.append("inference.committed", "x", nlohmann::json{{"i", 0}}, ""));
    REQUIRE(l.append("drift.crossed",       "x", nlohmann::json{{"i", 1}}, ""));
    REQUIRE(l.append("inference.committed", "x", nlohmann::json{{"i", 2}}, ""));
    std::this_thread::sleep_for(std::chrono::milliseconds{2});
    auto t1 = Time::now();
    // Wide window — should pick up the two inference.committed entries.
    auto r = l.events_between(t0, t1, "inference.committed");
    REQUIRE(r);
    REQUIRE(r.value().size() == 2u);
    CHECK(r.value()[0].header.event_type == "inference.committed");
    CHECK(r.value()[1].header.event_type == "inference.committed");
    // Seq-ascending.
    CHECK(r.value()[0].header.seq < r.value()[1].header.seq);
}

TEST_CASE("events_between edges: empty event_type, from > to, empty window") {
    auto p = tmp_db("events_between_edges");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "x", nlohmann::json{{"i", 0}}, ""));

    // Empty event_type rejected.
    auto bad1 = l.events_between(Time{0}, Time::now(), "");
    REQUIRE(!bad1);
    CHECK(bad1.error().code() == ErrorCode::invalid_argument);

    // from > to rejected.
    auto now = Time::now();
    auto bad2 = l.events_between(now, Time{0}, "e");
    REQUIRE(!bad2);
    CHECK(bad2.error().code() == ErrorCode::invalid_argument);

    // Empty interval [t, t) returns nothing (half-open).
    auto t = Time::now();
    auto empty_range = l.events_between(t, t, "e");
    REQUIRE(empty_range); CHECK(empty_range.value().empty());

    // Unknown event type returns empty.
    auto none = l.events_between(Time{0}, Time::now(), "no.such.type");
    REQUIRE(none); CHECK(none.value().empty());
}

TEST_CASE("events_between integration: half-open interval excludes upper bound") {
    auto p = tmp_db("events_between_halfopen");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "x", nlohmann::json{{"i", 0}}, ""));
    std::this_thread::sleep_for(std::chrono::milliseconds{2});
    REQUIRE(l.append("e", "x", nlohmann::json{{"i", 1}}, ""));
    std::this_thread::sleep_for(std::chrono::milliseconds{2});
    REQUIRE(l.append("e", "x", nlohmann::json{{"i", 2}}, ""));
    std::this_thread::sleep_for(std::chrono::milliseconds{2});
    REQUIRE(l.append("other", "x", nlohmann::json{{"i", 3}}, ""));

    auto e2 = l.at(2); REQUIRE(e2);
    auto e3 = l.at(3); REQUIRE(e3);
    // [e2.ts, e3.ts) — e2 included, e3 excluded.
    auto r = l.events_between(e2.value().header.ts, e3.value().header.ts, "e");
    REQUIRE(r);
    REQUIRE(r.value().size() == 1u);
    CHECK(r.value()[0].header.seq == 2u);

    // event_type filter sieves out the "other" entry even when in window.
    auto r2 = l.events_between(Time{0}, Time::now(), "e");
    REQUIRE(r2);
    CHECK(r2.value().size() == 3u);
    for (const auto& e : r2.value()) CHECK(e.header.event_type == "e");
}

// ---- has_inference_id ---------------------------------------------------

TEST_CASE("has_inference_id: basic hit and miss") {
    auto p = tmp_db("has_iid_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("inference.committed", "x",
                     nlohmann::json{{"inference_id", "infer-abc"},
                                    {"patient", "P1"}}, ""));
    REQUIRE(l.append("inference.committed", "x",
                     nlohmann::json{{"inference_id", "infer-xyz"},
                                    {"patient", "P2"}}, ""));
    CHECK(l.has_inference_id("infer-abc") == true);
    CHECK(l.has_inference_id("infer-xyz") == true);
    CHECK(l.has_inference_id("infer-nope") == false);
}

TEST_CASE("has_inference_id edges: empty id, empty chain, non-JSON-string field") {
    auto p = tmp_db("has_iid_edges");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // Empty chain — must be false, no allocation.
    CHECK(l.has_inference_id("anything") == false);
    // Empty id — false (no scan).
    CHECK(l.has_inference_id("") == false);
    // Non-string inference_id field shouldn't false-positive.
    REQUIRE(l.append("inference.committed", "x",
                     nlohmann::json{{"inference_id", 12345}}, ""));
    CHECK(l.has_inference_id("12345") == false);
    // Body containing the id substring but not as the field's value.
    REQUIRE(l.append("inference.committed", "x",
                     nlohmann::json{{"inference_id", "real-id"},
                                    {"note", "decoy-id"}}, ""));
    CHECK(l.has_inference_id("decoy-id") == false);
    CHECK(l.has_inference_id("real-id") == true);
}

TEST_CASE("has_inference_id integration: parity with find_by_inference_id") {
    auto p = tmp_db("has_iid_parity");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 50; i++) {
        REQUIRE(l.append("inference.committed", "x",
                         nlohmann::json{{"inference_id",
                                         std::string{"id-"} + std::to_string(i)},
                                        {"patient", "P"}}, ""));
    }
    // Every id we appended must report true and resolve via find.
    for (int i = 0; i < 50; i++) {
        std::string id = "id-" + std::to_string(i);
        CHECK(l.has_inference_id(id) == true);
        auto found = l.find_by_inference_id(id);
        REQUIRE(found);
        CHECK(found.value().body_json.find(id) != std::string::npos);
    }
    // Absent ids: both report negative.
    CHECK(l.has_inference_id("id-9999") == false);
    auto miss = l.find_by_inference_id("id-9999");
    REQUIRE(!miss);
    CHECK(miss.error().code() == ErrorCode::not_found);
}

// ---- attestation_json ---------------------------------------------------

TEST_CASE("attestation_json: basic — keys present, head/key match attest()") {
    auto p = tmp_db("attestation_json_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "x", nlohmann::json{{"i", 0}}, ""));
    REQUIRE(l.append("e", "x", nlohmann::json{{"i", 1}}, ""));

    auto j = nlohmann::json::parse(l.attestation_json());
    auto a = l.attest();

    CHECK(j.at("length").get<std::uint64_t>() == 2u);
    CHECK(j.at("head_hash").get<std::string>() == a.head.hex());
    CHECK(j.at("key_id").get<std::string>() == a.key_id);
    CHECK(j.at("fingerprint").get<std::string>() == a.fingerprint);
    REQUIRE(j.contains("oldest_ts"));
    REQUIRE(j.contains("newest_ts"));
    CHECK(!j.at("oldest_ts").get<std::string>().empty());
    CHECK(!j.at("newest_ts").get<std::string>().empty());
}

TEST_CASE("attestation_json edges: empty chain emits empty oldest/newest ts") {
    auto p = tmp_db("attestation_json_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();

    auto j = nlohmann::json::parse(l.attestation_json());
    CHECK(j.at("length").get<std::uint64_t>() == 0u);
    CHECK(j.at("oldest_ts").get<std::string>() == "");
    CHECK(j.at("newest_ts").get<std::string>() == "");
    // head_hash is the all-zero head hex; still present and well-formed.
    CHECK(j.at("head_hash").get<std::string>().size() == Hash::size * 2);
    // key_id and fingerprint always present (open() generated/loaded a key).
    CHECK(!j.at("key_id").get<std::string>().empty());
    CHECK(!j.at("fingerprint").get<std::string>().empty());
}

TEST_CASE("attestation_json integration: oldest_ts/newest_ts match oldest/newest_entry") {
    auto p = tmp_db("attestation_json_ts");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 5; i++) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
    }
    auto oldest = l.oldest_entry(); REQUIRE(oldest);
    auto newest = l.newest_entry(); REQUIRE(newest);

    auto j = nlohmann::json::parse(l.attestation_json());
    CHECK(j.at("oldest_ts").get<std::string>() ==
          oldest.value().header.ts.iso8601());
    CHECK(j.at("newest_ts").get<std::string>() ==
          newest.value().header.ts.iso8601());
    CHECK(j.at("length").get<std::uint64_t>() == l.length());
    // Single-entry chain: oldest_ts == newest_ts.
    auto p2 = tmp_db("attestation_json_single");
    auto l2_ = Ledger::open(p2); REQUIRE(l2_);
    auto& l2 = l2_.value();
    REQUIRE(l2.append("e", "x", nlohmann::json{{"i", 0}}, ""));
    auto j2 = nlohmann::json::parse(l2.attestation_json());
    CHECK(j2.at("oldest_ts").get<std::string>() ==
          j2.at("newest_ts").get<std::string>());
}

// ---- head_attestation_json ----------------------------------------------

namespace {

// Test-local hex decoder: convert a hex string into a fixed-size byte
// array. Returns false on length or charset mismatch.
template <std::size_t N>
bool hex_to_bytes(std::string_view s, std::array<std::uint8_t, N>& out) {
    if (s.size() != N * 2) return false;
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < N; ++i) {
        int hi = val(s[2*i]);
        int lo = val(s[2*i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return true;
}

}  // namespace

TEST_CASE("head_attestation_json: keys present and consistent with public API") {
    auto p = tmp_db("head_attestation_json_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "x", nlohmann::json{{"i", 0}}, ""));
    REQUIRE(l.append("e", "x", nlohmann::json{{"i", 1}}, ""));

    auto s = l.head_attestation_json();
    auto j = nlohmann::json::parse(s);

    // Single-line JSON: no embedded newline.
    CHECK(s.find('\n') == std::string::npos);

    // All five required keys present.
    REQUIRE(j.contains("length"));
    REQUIRE(j.contains("head_hex"));
    REQUIRE(j.contains("key_id"));
    REQUIRE(j.contains("key_fingerprint"));
    REQUIRE(j.contains("head_signature"));

    CHECK(j.at("length").get<std::uint64_t>() == 2u);
    CHECK(j.at("head_hex").get<std::string>() == l.head().hex());
    CHECK(j.at("key_id").get<std::string>() == l.key_id());
    // Fingerprint matches what attest() reports (both delegate to the
    // same KeyStore::fingerprint).
    auto a = l.attest();
    CHECK(j.at("key_fingerprint").get<std::string>() == a.fingerprint);

    // head_signature is 64 bytes hex == 128 chars.
    CHECK(j.at("head_signature").get<std::string>().size() ==
          KeyStore::sig_bytes * 2);
}

TEST_CASE("head_attestation_json: signature verifies against the ledger's public key") {
    auto p = tmp_db("head_attestation_json_verify");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 3; ++i) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
    }

    auto j = nlohmann::json::parse(l.head_attestation_json());

    std::array<std::uint8_t, KeyStore::sig_bytes> sig{};
    REQUIRE(hex_to_bytes(j.at("head_signature").get<std::string>(), sig));

    auto head = l.head();
    auto pk   = l.public_key();

    CHECK(KeyStore::verify(
        Bytes{head.bytes.data(), head.bytes.size()},
        std::span<const std::uint8_t, KeyStore::sig_bytes>{sig.data(), sig.size()},
        std::span<const std::uint8_t, KeyStore::pk_bytes>{pk.data(),   pk.size()}));

    // Tampering with the head bytes invalidates the signature.
    auto bad_head = head.bytes;
    bad_head[0] = static_cast<std::uint8_t>(bad_head[0] ^ 0xFFu);
    CHECK(!KeyStore::verify(
        Bytes{bad_head.data(), bad_head.size()},
        std::span<const std::uint8_t, KeyStore::sig_bytes>{sig.data(), sig.size()},
        std::span<const std::uint8_t, KeyStore::pk_bytes>{pk.data(),   pk.size()}));
}

TEST_CASE("head_attestation_json edges: empty chain emits well-formed signature over zero head") {
    auto p = tmp_db("head_attestation_json_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();

    auto j = nlohmann::json::parse(l.head_attestation_json());
    CHECK(j.at("length").get<std::uint64_t>() == 0u);
    // head_hex is the all-zero head hex; still well-formed.
    CHECK(j.at("head_hex").get<std::string>().size() == Hash::size * 2);
    CHECK(j.at("head_hex").get<std::string>() == Hash::zero().hex());
    // key_id and key_fingerprint always present.
    CHECK(!j.at("key_id").get<std::string>().empty());
    CHECK(!j.at("key_fingerprint").get<std::string>().empty());

    // Signature is well-formed and verifies over the zero head bytes.
    std::array<std::uint8_t, KeyStore::sig_bytes> sig{};
    REQUIRE(hex_to_bytes(j.at("head_signature").get<std::string>(), sig));
    auto pk   = l.public_key();
    auto zero = Hash::zero();
    CHECK(KeyStore::verify(
        Bytes{zero.bytes.data(), zero.bytes.size()},
        std::span<const std::uint8_t, KeyStore::sig_bytes>{sig.data(), sig.size()},
        std::span<const std::uint8_t, KeyStore::pk_bytes>{pk.data(),   pk.size()}));
}

TEST_CASE("head_attestation_json: signature changes as the chain advances") {
    auto p = tmp_db("head_attestation_json_advance");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();

    REQUIRE(l.append("e", "x", nlohmann::json{{"i", 0}}, ""));
    auto j1 = nlohmann::json::parse(l.head_attestation_json());
    auto sig1 = j1.at("head_signature").get<std::string>();
    auto head1 = j1.at("head_hex").get<std::string>();

    REQUIRE(l.append("e", "x", nlohmann::json{{"i", 1}}, ""));
    auto j2 = nlohmann::json::parse(l.head_attestation_json());
    auto sig2 = j2.at("head_signature").get<std::string>();
    auto head2 = j2.at("head_hex").get<std::string>();

    // Advancing the chain advances both the head and (with overwhelming
    // probability) the signature over those head bytes.
    CHECK(head1 != head2);
    CHECK(sig1 != sig2);
    CHECK(j2.at("length").get<std::uint64_t>() == 2u);
    // key_id / fingerprint remain stable across appends.
    CHECK(j1.at("key_id").get<std::string>() ==
          j2.at("key_id").get<std::string>());
    CHECK(j1.at("key_fingerprint").get<std::string>() ==
          j2.at("key_fingerprint").get<std::string>());
}

// ---- seq_range_for_tenant ------------------------------------------------

TEST_CASE("seq_range_for_tenant: empty chain returns (0, 0)") {
    auto p = tmp_db("seq_range_for_tenant_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();

    auto r = l.seq_range_for_tenant("acme");
    REQUIRE(r);
    CHECK(r.value().first  == 0u);
    CHECK(r.value().second == 0u);
}

TEST_CASE("seq_range_for_tenant: bounds match interleaved tenants") {
    auto p = tmp_db("seq_range_for_tenant_interleaved");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();

    REQUIRE(l.append("e", "a", nlohmann::json{{"i", 1}}, "acme"));   // seq 1
    REQUIRE(l.append("e", "a", nlohmann::json{{"i", 2}}, "globex")); // seq 2
    REQUIRE(l.append("e", "a", nlohmann::json{{"i", 3}}, "acme"));   // seq 3
    REQUIRE(l.append("e", "a", nlohmann::json{{"i", 4}}, "acme"));   // seq 4
    REQUIRE(l.append("e", "a", nlohmann::json{{"i", 5}}, "globex")); // seq 5

    auto a = l.seq_range_for_tenant("acme");
    REQUIRE(a);
    CHECK(a.value().first  == 1u);
    CHECK(a.value().second == 4u);

    auto g = l.seq_range_for_tenant("globex");
    REQUIRE(g);
    CHECK(g.value().first  == 2u);
    CHECK(g.value().second == 5u);

    auto unknown = l.seq_range_for_tenant("nope");
    REQUIRE(unknown);
    CHECK(unknown.value().first  == 0u);
    CHECK(unknown.value().second == 0u);
}

TEST_CASE("seq_range_for_tenant: empty tenant is its own scope") {
    auto p = tmp_db("seq_range_for_tenant_empty_tenant");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();

    REQUIRE(l.append("e", "a", nlohmann::json{{"i", 1}}, ""));      // seq 1, ""
    REQUIRE(l.append("e", "a", nlohmann::json{{"i", 2}}, "acme"));  // seq 2
    REQUIRE(l.append("e", "a", nlohmann::json{{"i", 3}}, ""));      // seq 3, ""

    auto empty = l.seq_range_for_tenant("");
    REQUIRE(empty);
    CHECK(empty.value().first  == 1u);
    CHECK(empty.value().second == 3u);

    auto acme = l.seq_range_for_tenant("acme");
    REQUIRE(acme);
    CHECK(acme.value().first  == 2u);
    CHECK(acme.value().second == 2u);
}

// ---- distinct_event_types ------------------------------------------------

TEST_CASE("distinct_event_types: empty chain returns empty vector") {
    auto p = tmp_db("distinct_event_types_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();

    auto r = l.distinct_event_types();
    REQUIRE(r);
    CHECK(r.value().empty());
}

TEST_CASE("distinct_event_types: deduplicates and sorts alphabetically") {
    auto p = tmp_db("distinct_event_types_sorted");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();

    REQUIRE(l.append("zeta",   "a", nlohmann::json{{"i", 1}}));
    REQUIRE(l.append("alpha",  "a", nlohmann::json{{"i", 2}}));
    REQUIRE(l.append("zeta",   "a", nlohmann::json{{"i", 3}}));
    REQUIRE(l.append("middle", "a", nlohmann::json{{"i", 4}}));
    REQUIRE(l.append("alpha",  "a", nlohmann::json{{"i", 5}}));

    auto r = l.distinct_event_types();
    REQUIRE(r);
    REQUIRE(r.value().size() == 3u);
    CHECK(r.value()[0] == "alpha");
    CHECK(r.value()[1] == "middle");
    CHECK(r.value()[2] == "zeta");
}

TEST_CASE("distinct_event_types: single event_type returns vector of one") {
    auto p = tmp_db("distinct_event_types_one");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();

    REQUIRE(l.append("only", "a", nlohmann::json{{"i", 1}}));
    REQUIRE(l.append("only", "a", nlohmann::json{{"i", 2}}));
    REQUIRE(l.append("only", "a", nlohmann::json{{"i", 3}}));

    auto r = l.distinct_event_types();
    REQUIRE(r);
    REQUIRE(r.value().size() == 1u);
    CHECK(r.value()[0] == "only");
}

// ---- checksum_range ------------------------------------------------------

TEST_CASE("checksum_range: empty chain and empty range return Hash::zero()") {
    auto p = tmp_db("checksum_range_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();

    // empty chain
    auto r0 = l.checksum_range(1, 1);
    REQUIRE(r0);
    CHECK(r0.value() == Hash::zero());

    // populate
    REQUIRE(l.append("e", "a", nlohmann::json{{"i", 1}}));
    REQUIRE(l.append("e", "a", nlohmann::json{{"i", 2}}));

    // empty range over a populated chain
    auto r1 = l.checksum_range(2, 2);
    REQUIRE(r1);
    CHECK(r1.value() == Hash::zero());
}

TEST_CASE("checksum_range: deterministic over identical content; sensitive to range") {
    auto p = tmp_db("checksum_range_deterministic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();

    REQUIRE(l.append("e", "a", nlohmann::json{{"i", 1}}));
    REQUIRE(l.append("e", "a", nlohmann::json{{"i", 2}}));
    REQUIRE(l.append("e", "a", nlohmann::json{{"i", 3}}));

    auto a = l.checksum_range(1, 4);
    auto b = l.checksum_range(1, 4);
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a.value() == b.value());
    CHECK(a.value() != Hash::zero());

    // Different range -> different checksum (with overwhelming probability).
    auto c = l.checksum_range(1, 3);
    REQUIRE(c);
    CHECK(c.value() != a.value());

    // Single-entry range equals the entry's own entry_hash hashed once.
    auto d = l.checksum_range(2, 3);
    REQUIRE(d);
    auto e2 = l.at(2); REQUIRE(e2);
    Hasher h;
    auto eh = e2.value().entry_hash();
    h.update(Bytes{eh.bytes.data(), eh.bytes.size()});
    CHECK(d.value() == h.finalize());
}

TEST_CASE("checksum_range: rejects bad bounds") {
    auto p = tmp_db("checksum_range_bad");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();

    REQUIRE(l.append("e", "a", nlohmann::json{{"i", 1}}));
    REQUIRE(l.append("e", "a", nlohmann::json{{"i", 2}}));

    // start > end
    auto r1 = l.checksum_range(3, 1);
    REQUIRE(!r1);
    CHECK(r1.error().code() == ErrorCode::invalid_argument);

    // end past chain
    auto r2 = l.checksum_range(1, 99);
    REQUIRE(!r2);
    CHECK(r2.error().code() == ErrorCode::invalid_argument);

    // start == 0 with non-empty range
    auto r3 = l.checksum_range(0, 2);
    REQUIRE(!r3);
    CHECK(r3.error().code() == ErrorCode::invalid_argument);
}

// ---- has_event_after_seq -------------------------------------------------

TEST_CASE("has_event_after_seq: empty chain returns false for any seq") {
    auto p = tmp_db("has_event_after_seq_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();

    CHECK(l.has_event_after_seq(0)   == false);
    CHECK(l.has_event_after_seq(1)   == false);
    CHECK(l.has_event_after_seq(999) == false);
}

TEST_CASE("has_event_after_seq: true iff seq < length") {
    auto p = tmp_db("has_event_after_seq_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();

    REQUIRE(l.append("e", "a", nlohmann::json{{"i", 1}}));
    REQUIRE(l.append("e", "a", nlohmann::json{{"i", 2}}));
    REQUIRE(l.append("e", "a", nlohmann::json{{"i", 3}}));

    // length() == 3 — there's something past 0, 1, 2 but not past 3.
    CHECK(l.has_event_after_seq(0) == true);
    CHECK(l.has_event_after_seq(1) == true);
    CHECK(l.has_event_after_seq(2) == true);
    CHECK(l.has_event_after_seq(3) == false);
    CHECK(l.has_event_after_seq(4) == false);
}

TEST_CASE("has_event_after_seq: tracks newly appended entries") {
    auto p = tmp_db("has_event_after_seq_advance");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();

    REQUIRE(l.append("e", "a", nlohmann::json{{"i", 1}}));
    CHECK(l.has_event_after_seq(1) == false);  // caught up at seq=1

    REQUIRE(l.append("e", "a", nlohmann::json{{"i", 2}}));
    CHECK(l.has_event_after_seq(1) == true);   // a follower at 1 is now behind
    CHECK(l.has_event_after_seq(2) == false);  // a follower at 2 is caught up
}

// ---- cumulative_body_bytes ------------------------------------------------

TEST_CASE("cumulative_body_bytes: empty chain returns 0") {
    auto p  = tmp_db("cum_bytes_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();

    auto r = l.cumulative_body_bytes();
    REQUIRE(r);
    CHECK(r.value() == 0);
}

TEST_CASE("cumulative_body_bytes: matches sum of body_json sizes") {
    auto p  = tmp_db("cum_bytes_sum");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();

    std::uint64_t expected = 0;
    for (int i = 0; i < 7; ++i) {
        nlohmann::json b;
        b["i"]   = i;
        b["pad"] = std::string(static_cast<std::size_t>(i) * 4, 'x');
        auto e = l.append("e.t", "a", b);
        REQUIRE(e);
        expected += e.value().body_json.size();
    }
    auto r = l.cumulative_body_bytes();
    REQUIRE(r);
    CHECK(r.value() == expected);
}

TEST_CASE("cumulative_body_bytes: agrees with stats().total_body_bytes") {
    auto p  = tmp_db("cum_bytes_vs_stats");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();

    for (int i = 0; i < 25; ++i) {
        nlohmann::json b; b["i"] = i;
        REQUIRE(l.append("e.t", "a", b));
    }
    auto cum = l.cumulative_body_bytes(); REQUIRE(cum);
    auto st  = l.stats();                 REQUIRE(st);
    CHECK(cum.value() == st.value().total_body_bytes);
    CHECK(cum.value() > 0);
}

TEST_CASE("cumulative_body_bytes: grows monotonically with appends") {
    auto p  = tmp_db("cum_bytes_grow");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();

    auto r0 = l.cumulative_body_bytes(); REQUIRE(r0);
    CHECK(r0.value() == 0);

    nlohmann::json b1; b1["x"] = "first";
    REQUIRE(l.append("e", "a", b1));
    auto r1 = l.cumulative_body_bytes(); REQUIRE(r1);
    CHECK(r1.value() > r0.value());

    nlohmann::json b2; b2["x"] = "second-with-more-text";
    REQUIRE(l.append("e", "a", b2));
    auto r2 = l.cumulative_body_bytes(); REQUIRE(r2);
    CHECK(r2.value() > r1.value());
}

// ---- tail_in_seq_range ----------------------------------------------------

TEST_CASE("tail_in_seq_range: n=0 returns empty") {
    auto p  = tmp_db("tisr_n0");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 5; ++i) {
        nlohmann::json b; b["i"] = i;
        REQUIRE(l.append("e", "a", b));
    }
    auto r = l.tail_in_seq_range(1, 6, 0);
    REQUIRE(r);
    CHECK(r.value().empty());
}

TEST_CASE("tail_in_seq_range: start >= end returns empty") {
    auto p  = tmp_db("tisr_bounds");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 5; ++i) {
        nlohmann::json b; b["i"] = i;
        REQUIRE(l.append("e", "a", b));
    }
    auto r1 = l.tail_in_seq_range(3, 3, 10);
    REQUIRE(r1);
    CHECK(r1.value().empty());

    auto r2 = l.tail_in_seq_range(5, 1, 10);
    REQUIRE(r2);
    CHECK(r2.value().empty());
}

TEST_CASE("tail_in_seq_range: returns last n in most-recent-first order") {
    auto p  = tmp_db("tisr_order");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();

    for (int i = 1; i <= 10; ++i) {
        nlohmann::json b; b["i"] = i;
        REQUIRE(l.append("e", "a", b));
    }
    // window [3, 9) covers seq 3..8; last 3 are seq 6, 7, 8.
    auto r = l.tail_in_seq_range(3, 9, 3);
    REQUIRE(r);
    REQUIRE(r.value().size() == 3);
    CHECK(r.value()[0].header.seq == 8);
    CHECK(r.value()[1].header.seq == 7);
    CHECK(r.value()[2].header.seq == 6);
}

TEST_CASE("tail_in_seq_range: n larger than window yields entire window reversed") {
    auto p  = tmp_db("tisr_big_n");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();

    for (int i = 1; i <= 5; ++i) {
        nlohmann::json b; b["i"] = i;
        REQUIRE(l.append("e", "a", b));
    }
    auto r = l.tail_in_seq_range(2, 5, 100);
    REQUIRE(r);
    REQUIRE(r.value().size() == 3);
    CHECK(r.value()[0].header.seq == 4);
    CHECK(r.value()[1].header.seq == 3);
    CHECK(r.value()[2].header.seq == 2);
}

// ---- merkle_proof_path ----------------------------------------------------

TEST_CASE("merkle_proof_path: seq==0 is invalid_argument") {
    auto p  = tmp_db("mpp_zero");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "a", nlohmann::json{{"i", 1}}));
    auto r = l.merkle_proof_path(0);
    REQUIRE_FALSE(r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("merkle_proof_path: seq beyond length is invalid_argument") {
    auto p  = tmp_db("mpp_oob");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "a", nlohmann::json{{"i", 1}}));
    REQUIRE(l.append("e", "a", nlohmann::json{{"i", 2}}));
    auto r = l.merkle_proof_path(99);
    REQUIRE_FALSE(r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("merkle_proof_path: head entry path is just its own hash") {
    auto p  = tmp_db("mpp_head");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 4; ++i) {
        nlohmann::json b; b["i"] = i;
        REQUIRE(l.append("e", "a", b));
    }
    const auto len = l.length();
    auto path = l.merkle_proof_path(len);
    REQUIRE(path);
    REQUIRE(path.value().size() == 1);
    auto last = l.at(len); REQUIRE(last);
    CHECK(path.value()[0] == last.value().entry_hash());
    CHECK(path.value()[0] == l.head());
}

TEST_CASE("merkle_proof_path: path covers [seq, length] inclusive, in seq order") {
    auto p  = tmp_db("mpp_full");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 6; ++i) {
        nlohmann::json b; b["i"] = i;
        REQUIRE(l.append("e", "a", b));
    }
    const std::uint64_t target = 2;
    const auto len = l.length();

    auto path = l.merkle_proof_path(target);
    REQUIRE(path);
    REQUIRE(path.value().size() == len - target + 1);

    for (std::uint64_t i = 0; i < path.value().size(); ++i) {
        auto e = l.at(target + i);
        REQUIRE(e);
        CHECK(path.value()[i] == e.value().entry_hash());
    }
    // Last element matches the current chain head.
    CHECK(path.value().back() == l.head());
}

// ---- longest_run_of_event_type --------------------------------------------

TEST_CASE("longest_run_of_event_type: empty event_type is invalid_argument") {
    auto p  = tmp_db("lr_empty_arg");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "a", nlohmann::json{{"i", 1}}));
    auto r = l.longest_run_of_event_type("");
    REQUIRE_FALSE(r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("longest_run_of_event_type: no matches returns 0") {
    auto p  = tmp_db("lr_zero");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 5; ++i) {
        nlohmann::json b; b["i"] = i;
        REQUIRE(l.append("other.event", "a", b));
    }
    auto r = l.longest_run_of_event_type("drift.crossed");
    REQUIRE(r);
    CHECK(r.value() == 0);
}

TEST_CASE("longest_run_of_event_type: detects a single contiguous burst") {
    auto p  = tmp_db("lr_burst");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();

    // 2 misc, then 5 drift, then 1 misc.
    REQUIRE(l.append("misc", "a", nlohmann::json{{"i", 0}}));
    REQUIRE(l.append("misc", "a", nlohmann::json{{"i", 1}}));
    for (int i = 0; i < 5; ++i) {
        nlohmann::json b; b["i"] = i;
        REQUIRE(l.append("drift.crossed", "a", b));
    }
    REQUIRE(l.append("misc", "a", nlohmann::json{{"i", 99}}));

    auto r = l.longest_run_of_event_type("drift.crossed");
    REQUIRE(r);
    CHECK(r.value() == 5);
}

TEST_CASE("longest_run_of_event_type: picks longest among multiple runs") {
    auto p  = tmp_db("lr_multi");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();

    auto push_n = [&](const char* type, int n) {
        for (int i = 0; i < n; ++i) {
            nlohmann::json b; b["i"] = i;
            REQUIRE(l.append(type, "a", b));
        }
    };
    push_n("d", 3);
    push_n("o", 2);
    push_n("d", 7);   // longest run of "d"
    push_n("o", 1);
    push_n("d", 4);

    auto r = l.longest_run_of_event_type("d");
    REQUIRE(r);
    CHECK(r.value() == 7);

    auto r2 = l.longest_run_of_event_type("o");
    REQUIRE(r2);
    CHECK(r2.value() == 2);
}

// ============== Ledger::range_for_actor_in_window =======================

TEST_CASE("range_for_actor_in_window: basic actor + window filter, seq-asc") {
    auto p = tmp_db("rfaiw_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // First batch — these will fall *before* the window.
    for (int i = 0; i < 3; i++) {
        REQUIRE(l.append("e", "alice", nlohmann::json{{"i", i}}, ""));
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    auto from = Time::now();
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
    // Second batch — alice entries inside the window, plus a non-alice
    // sandwiched between, and a non-alice tail.
    REQUIRE(l.append("e", "alice", nlohmann::json{{"i", 3}}, ""));
    std::this_thread::sleep_for(std::chrono::milliseconds{2});
    REQUIRE(l.append("e", "bob",   nlohmann::json{{"i", 4}}, ""));
    std::this_thread::sleep_for(std::chrono::milliseconds{2});
    REQUIRE(l.append("e", "alice", nlohmann::json{{"i", 5}}, ""));
    std::this_thread::sleep_for(std::chrono::milliseconds{2});
    auto to = Time::now() + std::chrono::nanoseconds{std::chrono::seconds{60}};
    auto r = l.range_for_actor_in_window("alice", from, to); REQUIRE(r);
    REQUIRE(r.value().size() == 2);
    // seq-ascending invariant.
    CHECK(r.value()[0].header.seq < r.value()[1].header.seq);
    auto b0 = nlohmann::json::parse(r.value()[0].body_json);
    auto b1 = nlohmann::json::parse(r.value()[1].body_json);
    CHECK(b0["i"] == 3);
    CHECK(b1["i"] == 5);
}

TEST_CASE("range_for_actor_in_window: empty actor and from > to") {
    auto p = tmp_db("rfaiw_edge");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "alice", nlohmann::json::object(), ""));
    auto t1 = Time::now();
    auto t0 = t1 - std::chrono::nanoseconds{std::chrono::seconds{1}};
    auto r1 = l.range_for_actor_in_window("", t0, t1);
    CHECK(!r1);
    CHECK(r1.error().code() == ErrorCode::invalid_argument);
    auto r2 = l.range_for_actor_in_window("alice", t1, t0);
    CHECK(!r2);
    CHECK(r2.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("range_for_actor_in_window: filters out other actors and out-of-window entries") {
    auto p = tmp_db("rfaiw_filter");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    auto t0 = Time::now() - std::chrono::nanoseconds{std::chrono::seconds{1}};
    REQUIRE(l.append("e", "alice", nlohmann::json{{"i", 1}}, ""));
    REQUIRE(l.append("e", "bob",   nlohmann::json{{"i", 2}}, ""));
    REQUIRE(l.append("e", "alice", nlohmann::json{{"i", 3}}, ""));
    REQUIRE(l.append("e", "carol", nlohmann::json{{"i", 4}}, ""));
    auto t1 = Time::now() + std::chrono::nanoseconds{std::chrono::seconds{60}};
    auto r = l.range_for_actor_in_window("alice", t0, t1); REQUIRE(r);
    REQUIRE(r.value().size() == 2);
    auto b0 = nlohmann::json::parse(r.value()[0].body_json);
    auto b1 = nlohmann::json::parse(r.value()[1].body_json);
    CHECK(b0["i"] == 1);
    CHECK(b1["i"] == 3);
    // seq-ascending invariant.
    CHECK(r.value()[0].header.seq < r.value()[1].header.seq);
}

TEST_CASE("range_for_actor_in_window: half-open interval excludes upper bound") {
    auto p = tmp_db("rfaiw_halfopen");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    auto e1 = l.append("e", "alice", nlohmann::json{{"i", 1}}, ""); REQUIRE(e1);
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
    auto e2 = l.append("e", "alice", nlohmann::json{{"i", 2}}, ""); REQUIRE(e2);
    // Window [ts1, ts2) — should include e1, exclude e2.
    auto r = l.range_for_actor_in_window(
        "alice", e1.value().header.ts, e2.value().header.ts);
    REQUIRE(r);
    REQUIRE(r.value().size() == 1);
    auto b = nlohmann::json::parse(r.value()[0].body_json);
    CHECK(b["i"] == 1);
}

// ============== Ledger::ts_at_seq ========================================

TEST_CASE("ts_at_seq: returns the timestamp at a valid seq") {
    auto p = tmp_db("tas_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    auto e1 = l.append("e", "x", nlohmann::json{{"i", 1}}, ""); REQUIRE(e1);
    auto e2 = l.append("e", "x", nlohmann::json{{"i", 2}}, ""); REQUIRE(e2);
    auto r1 = l.ts_at_seq(1); REQUIRE(r1);
    auto r2 = l.ts_at_seq(2); REQUIRE(r2);
    CHECK(r1.value() == e1.value().header.ts);
    CHECK(r2.value() == e2.value().header.ts);
}

TEST_CASE("ts_at_seq: seq=0 and seq>length return invalid_argument") {
    auto p = tmp_db("tas_edge");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "x", nlohmann::json::object(), ""));
    auto r0 = l.ts_at_seq(0);
    CHECK(!r0);
    CHECK(r0.error().code() == ErrorCode::invalid_argument);
    auto r2 = l.ts_at_seq(2);
    CHECK(!r2);
    CHECK(r2.error().code() == ErrorCode::invalid_argument);
    // Empty chain: seq=1 also out of range.
    auto p2 = tmp_db("tas_edge_empty");
    auto l2_ = Ledger::open(p2); REQUIRE(l2_);
    auto re = l2_.value().ts_at_seq(1);
    CHECK(!re);
    CHECK(re.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("ts_at_seq: agrees with at(seq).header.ts across the chain") {
    auto p = tmp_db("tas_parity");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 5; i++) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    for (std::uint64_t s = 1; s <= 5; ++s) {
        auto r = l.ts_at_seq(s); REQUIRE(r);
        auto e = l.at(s); REQUIRE(e);
        CHECK(r.value() == e.value().header.ts);
    }
}

// ============== Ledger::age_of_oldest ====================================

TEST_CASE("age_of_oldest: empty chain returns 0ns") {
    auto p = tmp_db("aoo_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto r = l_.value().age_of_oldest(); REQUIRE(r);
    CHECK(r.value() == std::chrono::nanoseconds{0});
}

TEST_CASE("age_of_oldest: positive duration on a non-empty chain") {
    auto p = tmp_db("aoo_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "x", nlohmann::json::object(), ""));
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    auto r = l.age_of_oldest(); REQUIRE(r);
    CHECK(r.value() > std::chrono::nanoseconds{0});
    // Should be at least 20ms.
    CHECK(r.value() >= std::chrono::milliseconds{15});
}

TEST_CASE("age_of_oldest: anchors to seq==1 even after more appends") {
    auto p = tmp_db("aoo_anchor");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    auto e1 = l.append("e", "x", nlohmann::json{{"i", 1}}, ""); REQUIRE(e1);
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    auto e2 = l.append("e", "x", nlohmann::json{{"i", 2}}, ""); REQUIRE(e2);
    REQUIRE(l.append("e", "x", nlohmann::json{{"i", 3}}, ""));
    auto r = l.age_of_oldest(); REQUIRE(r);
    // age_of_oldest measures from seq==1, not the most recent entry.
    // The age must be at least the gap between the first append and a
    // subsequent append (15ms slack for clock granularity).
    auto gap = e2.value().header.ts - e1.value().header.ts;
    CHECK(r.value() >= gap);
}

// ============== Ledger::body_byte_size_at ================================

TEST_CASE("body_byte_size_at: matches body_json size at a valid seq") {
    auto p = tmp_db("bbsa_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    nlohmann::json b1; b1["msg"] = "hello";
    nlohmann::json b2; b2["msg"] = "a much longer payload to differ in size";
    auto e1 = l.append("e", "x", b1, ""); REQUIRE(e1);
    auto e2 = l.append("e", "x", b2, ""); REQUIRE(e2);
    auto r1 = l.body_byte_size_at(1); REQUIRE(r1);
    auto r2 = l.body_byte_size_at(2); REQUIRE(r2);
    CHECK(r1.value() == e1.value().body_json.size());
    CHECK(r2.value() == e2.value().body_json.size());
    CHECK(r2.value() > r1.value());
}

TEST_CASE("body_byte_size_at: invalid seq returns invalid_argument") {
    auto p = tmp_db("bbsa_edge");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "x", nlohmann::json::object(), ""));
    auto r0 = l.body_byte_size_at(0);
    CHECK(!r0);
    CHECK(r0.error().code() == ErrorCode::invalid_argument);
    auto rover = l.body_byte_size_at(99);
    CHECK(!rover);
    CHECK(rover.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("body_byte_size_at: sums to cumulative_body_bytes across the chain") {
    auto p = tmp_db("bbsa_sum");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 8; i++) {
        REQUIRE(l.append("e", "x",
                         nlohmann::json{{"i", i}, {"pad", "data" + std::to_string(i)}},
                         ""));
    }
    std::uint64_t sum = 0;
    for (std::uint64_t s = 1; s <= l.length(); ++s) {
        auto r = l.body_byte_size_at(s); REQUIRE(r);
        sum += r.value();
    }
    auto cb = l.cumulative_body_bytes(); REQUIRE(cb);
    CHECK(sum == cb.value());
}

// ============== Ledger::any_actor_matches ================================

TEST_CASE("any_actor_matches: true when actor present, false otherwise") {
    auto p = tmp_db("aam_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "alice", nlohmann::json::object(), ""));
    REQUIRE(l.append("e", "bob",   nlohmann::json::object(), ""));
    CHECK(l.any_actor_matches("alice"));
    CHECK(l.any_actor_matches("bob"));
    CHECK_FALSE(l.any_actor_matches("ghost"));
}

TEST_CASE("any_actor_matches: empty actor and empty chain return false") {
    auto p = tmp_db("aam_edge");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // Empty chain.
    CHECK_FALSE(l.any_actor_matches(""));
    CHECK_FALSE(l.any_actor_matches("alice"));
    // Now non-empty: empty actor still false.
    REQUIRE(l.append("e", "alice", nlohmann::json::object(), ""));
    CHECK_FALSE(l.any_actor_matches(""));
}

TEST_CASE("any_actor_matches: agrees with range_by_actor presence") {
    auto p = tmp_db("aam_parity");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "alice", nlohmann::json::object(), ""));
    REQUIRE(l.append("e", "bob",   nlohmann::json::object(), ""));
    REQUIRE(l.append("e", "alice", nlohmann::json::object(), ""));
    auto check = [&](std::string_view who) {
        auto r = l.range_by_actor(who);
        bool present = r && !r.value().empty();
        CHECK(l.any_actor_matches(who) == present);
    };
    check("alice");
    check("bob");
    check("ghost");
}

// ============== Ledger::most_recent_for_actor ============================

TEST_CASE("most_recent_for_actor returns the largest-seq matching entry") {
    auto p = tmp_db("mrfa_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "alice", nlohmann::json{{"i", 1}}, ""));
    REQUIRE(l.append("e", "bob",   nlohmann::json{{"i", 2}}, ""));
    REQUIRE(l.append("e", "alice", nlohmann::json{{"i", 3}}, ""));
    REQUIRE(l.append("e", "alice", nlohmann::json{{"i", 4}}, ""));
    REQUIRE(l.append("e", "bob",   nlohmann::json{{"i", 5}}, ""));
    auto r = l.most_recent_for_actor("alice"); REQUIRE(r);
    CHECK(r.value().header.seq == 4);
    auto body = nlohmann::json::parse(r.value().body_json);
    CHECK(body["i"] == 4);
}

TEST_CASE("most_recent_for_actor: empty actor rejected; missing returns not_found") {
    auto p = tmp_db("mrfa_edge");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "alice", nlohmann::json::object(), ""));
    auto r1 = l.most_recent_for_actor("");
    CHECK(!r1);
    CHECK(r1.error().code() == ErrorCode::invalid_argument);
    auto r2 = l.most_recent_for_actor("ghost");
    CHECK(!r2);
    CHECK(r2.error().code() == ErrorCode::not_found);
}

TEST_CASE("most_recent_for_actor on empty ledger returns not_found") {
    auto p = tmp_db("mrfa_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto r = l_.value().most_recent_for_actor("alice");
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::not_found);
}

TEST_CASE("most_recent_for_actor matches tail_by_actor[0]") {
    auto p = tmp_db("mrfa_consistent");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 12; i++) {
        REQUIRE(l.append("e", (i % 3 == 0) ? "alice" : "bob",
                         nlohmann::json{{"i", i}}, ""));
    }
    auto tail = l.tail_by_actor("alice", 1); REQUIRE(tail);
    REQUIRE(!tail.value().empty());
    auto top = l.most_recent_for_actor("alice"); REQUIRE(top);
    CHECK(top.value().header.seq == tail.value().front().header.seq);
}

// ============== Ledger::distinct_actors_count ============================

TEST_CASE("distinct_actors_count returns the cardinality of distinct actors") {
    auto p = tmp_db("dac_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "alice",                nlohmann::json::object(), ""));
    REQUIRE(l.append("e", "bob",                  nlohmann::json::object(), ""));
    REQUIRE(l.append("e", "alice",                nlohmann::json::object(), ""));
    REQUIRE(l.append("e", "system:drift_monitor", nlohmann::json::object(), ""));
    auto r = l.distinct_actors_count(); REQUIRE(r);
    CHECK(r.value() == 3u);
}

TEST_CASE("distinct_actors_count: empty chain returns 0") {
    auto p = tmp_db("dac_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto r = l_.value().distinct_actors_count(); REQUIRE(r);
    CHECK(r.value() == 0u);
}

TEST_CASE("distinct_actors_count agrees with actors().size()") {
    auto p = tmp_db("dac_parity");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 25; i++) {
        // Five distinct actors, repeated.
        std::string a = "actor_" + std::to_string(i % 5);
        REQUIRE(l.append("e", a, nlohmann::json{{"i", i}}, ""));
    }
    auto names = l.actors(); REQUIRE(names);
    auto count = l.distinct_actors_count(); REQUIRE(count);
    CHECK(count.value() == names.value().size());
    CHECK(count.value() == 5u);
}

// ============== Ledger::seq_density ======================================

TEST_CASE("seq_density: empty and single-entry chains return 0.0") {
    auto p = tmp_db("sd_short");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    auto r0 = l.seq_density(); REQUIRE(r0);
    CHECK(r0.value() == 0.0);
    REQUIRE(l.append("e", "x", nlohmann::json::object(), ""));
    auto r1 = l.seq_density(); REQUIRE(r1);
    CHECK(r1.value() == 0.0);
}

TEST_CASE("seq_density: roughly matches (length-1)/elapsed for a paced chain") {
    auto p = tmp_db("sd_paced");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    constexpr int N = 6;
    for (int i = 0; i < N; i++) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    auto r = l.seq_density(); REQUIRE(r);
    // Strict positive density and bounded by the manual span.
    CHECK(r.value() > 0.0);
    auto oldest = l.at(1);            REQUIRE(oldest);
    auto newest = l.at(l.length());   REQUIRE(newest);
    auto span_ns = newest.value().header.ts - oldest.value().header.ts;
    auto seconds = static_cast<double>(span_ns.count()) / 1e9;
    REQUIRE(seconds > 0.0);
    auto expected = static_cast<double>(N - 1) / seconds;
    CHECK(r.value() == doctest::Approx(expected));
}

TEST_CASE("seq_density: scales inversely with elapsed time between bursts") {
    auto p = tmp_db("sd_scaling");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // Tightly-packed: high density.
    for (int i = 0; i < 10; i++) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
    }
    auto fast = l.seq_density(); REQUIRE(fast);
    // After a deliberate pause and one more entry, density drops.
    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    REQUIRE(l.append("e", "x", nlohmann::json::object(), ""));
    auto slow = l.seq_density(); REQUIRE(slow);
    CHECK(fast.value() > 0.0);
    CHECK(slow.value() > 0.0);
    // Inserting an extra delay between the last two appends should not
    // raise density above the pre-pause snapshot.
    CHECK(slow.value() <= fast.value());
}

// ============== Ledger::find_first_for_actor =============================

TEST_CASE("find_first_for_actor returns the oldest matching entry") {
    auto p = tmp_db("ffa_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "bob",   nlohmann::json{{"i", 1}}, ""));
    REQUIRE(l.append("e", "alice", nlohmann::json{{"i", 2}}, ""));
    REQUIRE(l.append("e", "alice", nlohmann::json{{"i", 3}}, ""));
    REQUIRE(l.append("e", "bob",   nlohmann::json{{"i", 4}}, ""));
    auto r = l.find_first_for_actor("alice"); REQUIRE(r);
    CHECK(r.value().header.seq == 2);
    auto body = nlohmann::json::parse(r.value().body_json);
    CHECK(body["i"] == 2);
}

TEST_CASE("find_first_for_actor: empty actor rejected; missing returns not_found") {
    auto p = tmp_db("ffa_edge");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "alice", nlohmann::json::object(), ""));
    auto r1 = l.find_first_for_actor("");
    CHECK(!r1);
    CHECK(r1.error().code() == ErrorCode::invalid_argument);
    auto r2 = l.find_first_for_actor("ghost");
    CHECK(!r2);
    CHECK(r2.error().code() == ErrorCode::not_found);
}

TEST_CASE("find_first_for_actor on empty ledger returns not_found") {
    auto p = tmp_db("ffa_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto r = l_.value().find_first_for_actor("alice");
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::not_found);
}

TEST_CASE("find_first_for_actor matches range_by_actor[0]") {
    auto p = tmp_db("ffa_consistent");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 12; i++) {
        REQUIRE(l.append("e", (i % 4 == 0) ? "alice" : "bob",
                         nlohmann::json{{"i", i}}, ""));
    }
    auto rng = l.range_by_actor("alice"); REQUIRE(rng);
    REQUIRE(!rng.value().empty());
    auto first = l.find_first_for_actor("alice"); REQUIRE(first);
    CHECK(first.value().header.seq == rng.value().front().header.seq);
}

// ============== Ledger::is_chain_continuous ==============================

TEST_CASE("is_chain_continuous: clean chain reports true; empty chain reports true") {
    auto p = tmp_db("icc_clean");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // Empty chain — no gaps.
    CHECK(l.is_chain_continuous());
    for (int i = 0; i < 8; i++) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
    }
    CHECK(l.is_chain_continuous());
}

TEST_CASE("is_chain_continuous: detects tampered prev_hash as discontinuous") {
    auto p = tmp_db("icc_break");
    {
        auto led = Ledger::open(p); REQUIRE(led);
        auto& l  = led.value();
        REQUIRE(l.append("e", "actor", nlohmann::json{{"x", 1}}, ""));
        REQUIRE(l.append("e", "actor", nlohmann::json{{"x", 2}}, ""));
    }
    // Zero out prev_hash on entry 2 to fabricate a chain break.
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open(p.string().c_str(), &db) == SQLITE_OK);
    REQUIRE(sqlite3_exec(db,
        "UPDATE asclepius_ledger SET prev_hash = zeroblob(32) WHERE seq = 2;",
        nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);

    auto l_ = Ledger::open(p); REQUIRE(l_);
    CHECK_FALSE(l_.value().is_chain_continuous());
}

TEST_CASE("is_chain_continuous: agrees with verify().has_value()") {
    auto p = tmp_db("icc_parity");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 5; i++) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
    }
    auto v = l.verify();
    CHECK(l.is_chain_continuous() == v.has_value());
}

// ============== Ledger::first_seq_at_or_after_time ======================

TEST_CASE("first_seq_at_or_after_time returns smallest seq with ts >= t") {
    auto p = tmp_db("fsa_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    std::vector<Time> times;
    for (int i = 0; i < 5; i++) {
        auto a = l.append("e", "x", nlohmann::json{{"i", i}}, "");
        REQUIRE(a);
        times.push_back(a.value().header.ts);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    // t == ts of entry 3 → seq 3 (the first with ts >= t).
    auto r3 = l.first_seq_at_or_after_time(times[2]);
    REQUIRE(r3);
    CHECK(r3.value() == 3);
    // t one nanosecond after entry 3's ts → seq 4 (skips 3).
    auto r4 = l.first_seq_at_or_after_time(
        times[2] + std::chrono::nanoseconds(1));
    REQUIRE(r4);
    CHECK(r4.value() == 4);
    // t earlier than the first entry → seq 1.
    auto r1 = l.first_seq_at_or_after_time(
        times[0] - std::chrono::seconds(1));
    REQUIRE(r1);
    CHECK(r1.value() == 1);
}

TEST_CASE("first_seq_at_or_after_time: empty chain returns not_found") {
    auto p = tmp_db("fsa_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    auto r = l.first_seq_at_or_after_time(Time::now());
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::not_found);
}

TEST_CASE("first_seq_at_or_after_time: t beyond newest returns not_found") {
    auto p = tmp_db("fsa_after_newest");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 3; i++) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    auto far_future = Time::now() + std::chrono::hours(24);
    auto r = l.first_seq_at_or_after_time(far_future);
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::not_found);
    // Sanity: chain still verifies after the probe.
    REQUIRE(l.verify());
}

// ============== Ledger::head_age_ms =====================================

TEST_CASE("head_age_ms returns milliseconds since the newest entry") {
    auto p = tmp_db("ham_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "x", nlohmann::json::object(), ""));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto r = l.head_age_ms();
    REQUIRE(r);
    // Should be >= ~50ms but bounded above (allow for slow CI).
    CHECK(r.value() >= std::chrono::milliseconds(40));
    CHECK(r.value() <= std::chrono::milliseconds(5000));
}

TEST_CASE("head_age_ms on empty chain returns not_found") {
    auto p = tmp_db("ham_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    auto r = l.head_age_ms();
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::not_found);
}

TEST_CASE("head_age_ms tracks the newest entry, not the oldest") {
    auto p = tmp_db("ham_tracks_head");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "x", nlohmann::json{{"i", 0}}, ""));
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    REQUIRE(l.append("e", "x", nlohmann::json{{"i", 1}}, ""));
    auto r = l.head_age_ms();
    REQUIRE(r);
    // Anchored on the *second* (newest) append, so the elapsed age must
    // be far less than the gap between the two appends.
    CHECK(r.value() < std::chrono::milliseconds(70));
}

// ============== Ledger::events_in_window_count ==========================

TEST_CASE("events_in_window_count counts entries in [from, to)") {
    auto p = tmp_db("eiwc_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 4; i++) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto from = Time::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    for (int i = 4; i < 9; i++) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    auto to = Time::now() + std::chrono::seconds(60);
    auto r = l.events_in_window_count(from, to);
    REQUIRE(r);
    CHECK(r.value() == 5u);
}

TEST_CASE("events_in_window_count: from > to returns invalid_argument") {
    auto p = tmp_db("eiwc_inv");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "x", nlohmann::json::object(), ""));
    auto t1 = Time::now();
    auto t0 = t1 - std::chrono::seconds(1);
    auto r = l.events_in_window_count(t1, t0);
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
    // Equal bounds (from == to) yields zero, not an error.
    auto reqz = l.events_in_window_count(t1, t1);
    REQUIRE(reqz);
    CHECK(reqz.value() == 0u);
}

TEST_CASE("events_in_window_count: half-open and empty-chain semantics") {
    auto p = tmp_db("eiwc_halfopen");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // Empty chain returns 0.
    auto r0 = l.events_in_window_count(
        Time{0}, Time::now() + std::chrono::seconds(60));
    REQUIRE(r0);
    CHECK(r0.value() == 0u);
    // Append, then probe with the upper bound equal to entry 3's ts —
    // half-open must exclude that entry.
    for (int i = 0; i < 5; i++) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    auto e3 = l.at(3); REQUIRE(e3);
    auto r = l.events_in_window_count(Time{0}, e3.value().header.ts);
    REQUIRE(r);
    CHECK(r.value() == 2u);  // entries 1 and 2 only
    // Sanity: result agrees with count_in_window for any well-formed window.
    auto everything_b = l.events_in_window_count(
        Time{0}, Time::now() + std::chrono::seconds(60));
    auto everything_a = l.count_in_window(
        Time{0}, Time::now() + std::chrono::seconds(60));
    REQUIRE(everything_a);
    REQUIRE(everything_b);
    CHECK(everything_a.value() == everything_b.value());
}

// ============== Ledger::summary_string ==================================

TEST_CASE("summary_string: empty chain shape") {
    auto p = tmp_db("ss_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    auto s = l.summary_string();
    CHECK(s.find("length=0 (empty)") != std::string::npos);
    CHECK(s.find("key=" + l.key_id()) != std::string::npos);
    // No head=, oldest=, newest= fields on an empty chain.
    CHECK(s.find("head=") == std::string::npos);
    CHECK(s.find("oldest=") == std::string::npos);
    CHECK(s.find("newest=") == std::string::npos);
}

TEST_CASE("summary_string: non-empty chain layout") {
    auto p = tmp_db("ss_nonempty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 3; i++) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    auto s = l.summary_string();
    CHECK(s.find("length=3") != std::string::npos);
    CHECK(s.find("key=" + l.key_id()) != std::string::npos);
    CHECK(s.find("oldest=") != std::string::npos);
    CHECK(s.find("newest=") != std::string::npos);
    // Head field carries the leading 12 hex chars of the head hash.
    auto head_full = l.head().hex();
    REQUIRE(head_full.size() >= 12);
    auto head12 = head_full.substr(0, 12);
    CHECK(s.find("head=" + head12) != std::string::npos);
    // It's a single line — no embedded newlines.
    CHECK(s.find('\n') == std::string::npos);
}

TEST_CASE("summary_string: head field is exactly 12 hex chars long") {
    auto p = tmp_db("ss_head12");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "x", nlohmann::json::object(), ""));
    auto s = l.summary_string();
    auto pos = s.find("head=");
    REQUIRE(pos != std::string::npos);
    pos += 5;  // step past "head="
    auto end = s.find(' ', pos);
    REQUIRE(end != std::string::npos);
    auto head_field = s.substr(pos, end - pos);
    CHECK(head_field.size() == 12);
    // Must match the leading 12 chars of the live head hex.
    CHECK(head_field == l.head().hex().substr(0, 12));
}

// ============== Ledger::tail_after_time =================================

TEST_CASE("tail_after_time returns last n entries with ts >= t") {
    auto p = tmp_db("tat_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 4; i++) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto t = Time::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    for (int i = 4; i < 10; i++) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    auto r = l.tail_after_time(t, 3);
    REQUIRE(r);
    CHECK(r.value().size() == 3);
    auto b0 = nlohmann::json::parse(r.value()[0].body_json);
    auto b2 = nlohmann::json::parse(r.value()[2].body_json);
    CHECK(b0["i"] == 9);  // most-recent first
    CHECK(b2["i"] == 7);
}

TEST_CASE("tail_after_time: n=0 yields empty vector (mirrors tail_by_actor)") {
    auto p = tmp_db("tat_zero");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "x", nlohmann::json::object(), ""));
    auto r = l.tail_after_time(Time{0}, 0);
    REQUIRE(r);
    CHECK(r.value().empty());
}

TEST_CASE("tail_after_time: t past newest yields empty vector") {
    auto p = tmp_db("tat_future");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 4; i++) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    auto far_future = Time::now() + std::chrono::hours(24);
    auto r = l.tail_after_time(far_future, 5);
    REQUIRE(r);
    CHECK(r.value().empty());
    // And: requesting more than match-count returns just the matches.
    auto everything = l.tail_after_time(Time{0}, 100);
    REQUIRE(everything);
    CHECK(everything.value().size() == 4);
    // Most-recent first.
    CHECK(everything.value().front().header.seq == 4);
    CHECK(everything.value().back().header.seq == 1);
    // Chain still verifies after the probes.
    REQUIRE(l.verify());
}

// ============== Ledger::range_for_actor_and_event_type ==================

TEST_CASE("range_for_actor_and_event_type: filters on both actor and event_type, seq-asc") {
    auto p = tmp_db("rfaaet_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // alice/alpha (kept), alice/beta (out by event), bob/alpha (out by actor),
    // alice/alpha again (kept), bob/beta (out by both).
    REQUIRE(l.append("alpha", "alice", nlohmann::json{{"i", 1}}, ""));
    REQUIRE(l.append("beta",  "alice", nlohmann::json{{"i", 2}}, ""));
    REQUIRE(l.append("alpha", "bob",   nlohmann::json{{"i", 3}}, ""));
    REQUIRE(l.append("alpha", "alice", nlohmann::json{{"i", 4}}, ""));
    REQUIRE(l.append("beta",  "bob",   nlohmann::json{{"i", 5}}, ""));
    auto r = l.range_for_actor_and_event_type("alice", "alpha"); REQUIRE(r);
    REQUIRE(r.value().size() == 2);
    // seq-ascending invariant.
    CHECK(r.value()[0].header.seq < r.value()[1].header.seq);
    auto b0 = nlohmann::json::parse(r.value()[0].body_json);
    auto b1 = nlohmann::json::parse(r.value()[1].body_json);
    CHECK(b0["i"] == 1);
    CHECK(b1["i"] == 4);
}

TEST_CASE("range_for_actor_and_event_type: empty actor or empty event_type rejected") {
    auto p = tmp_db("rfaaet_edge");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("alpha", "alice", nlohmann::json::object(), ""));
    auto r1 = l.range_for_actor_and_event_type("", "alpha");
    CHECK(!r1);
    CHECK(r1.error().code() == ErrorCode::invalid_argument);
    auto r2 = l.range_for_actor_and_event_type("alice", "");
    CHECK(!r2);
    CHECK(r2.error().code() == ErrorCode::invalid_argument);
    auto r3 = l.range_for_actor_and_event_type("", "");
    CHECK(!r3);
    CHECK(r3.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("range_for_actor_and_event_type: no match and empty-chain return empty vector") {
    auto p = tmp_db("rfaaet_miss");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // Empty chain: well-formed query returns empty vector (not error).
    auto re = l.range_for_actor_and_event_type("alice", "alpha"); REQUIRE(re);
    CHECK(re.value().empty());
    // Populated chain with no matching combo: still empty.
    REQUIRE(l.append("alpha", "bob",   nlohmann::json::object(), ""));
    REQUIRE(l.append("beta",  "alice", nlohmann::json::object(), ""));
    auto r = l.range_for_actor_and_event_type("alice", "alpha"); REQUIRE(r);
    CHECK(r.value().empty());
}

TEST_CASE("range_for_actor_and_event_type: agrees with manual scan over the chain") {
    auto p = tmp_db("rfaaet_parity");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    const char* actors[] = {"alice", "bob", "carol"};
    const char* events[] = {"alpha", "beta", "gamma"};
    for (int i = 0; i < 30; i++) {
        REQUIRE(l.append(events[i % 3], actors[i % 3],
                         nlohmann::json{{"i", i}}, ""));
    }
    auto r = l.range_for_actor_and_event_type("alice", "alpha"); REQUIRE(r);
    // Manual reference: every (alice, alpha) entry, seq-ascending.
    auto rng = l.range_by_actor("alice"); REQUIRE(rng);
    std::vector<LedgerEntry> ref;
    for (const auto& e : rng.value()) {
        if (e.header.event_type == "alpha") ref.push_back(e);
    }
    REQUIRE(r.value().size() == ref.size());
    for (std::size_t i = 0; i < ref.size(); ++i) {
        CHECK(r.value()[i].header.seq == ref[i].header.seq);
    }
}

// ============== Ledger::has_tenant =======================================

TEST_CASE("has_tenant: true for present tenants, false otherwise") {
    auto p = tmp_db("ht_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "x", nlohmann::json::object(), "tenant-a"));
    REQUIRE(l.append("e", "x", nlohmann::json::object(), "tenant-b"));
    CHECK(l.has_tenant("tenant-a"));
    CHECK(l.has_tenant("tenant-b"));
    CHECK_FALSE(l.has_tenant("tenant-ghost"));
}

TEST_CASE("has_tenant: empty tenant is its own scope") {
    auto p = tmp_db("ht_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // Empty chain: nothing matches anything, including "".
    CHECK_FALSE(l.has_tenant(""));
    CHECK_FALSE(l.has_tenant("anything"));
    // Append with non-empty tenant only — empty-tenant scope still empty.
    REQUIRE(l.append("e", "x", nlohmann::json::object(), "real-tenant"));
    CHECK_FALSE(l.has_tenant(""));
    CHECK(l.has_tenant("real-tenant"));
    // Add an entry in the empty-tenant scope and recheck.
    REQUIRE(l.append("e", "x", nlohmann::json::object(), ""));
    CHECK(l.has_tenant(""));
}

TEST_CASE("has_tenant: agrees with tenants() membership") {
    auto p = tmp_db("ht_consistent");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    const char* tenants[] = {"alpha", "beta", "", "gamma"};
    for (int i = 0; i < 16; i++) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, tenants[i % 4]));
    }
    auto ts = l.tenants(); REQUIRE(ts);
    for (const auto& name : ts.value()) {
        CHECK(l.has_tenant(name));
    }
    CHECK_FALSE(l.has_tenant("does-not-exist"));
}

// ============== Ledger::most_active_tenant ===============================

TEST_CASE("most_active_tenant returns the tenant with the largest entry count") {
    auto p = tmp_db("mat_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // alpha: 5, beta: 3, gamma: 1 — alpha clearly wins.
    for (int i = 0; i < 5; i++) REQUIRE(l.append("e", "x", nlohmann::json::object(), "alpha"));
    for (int i = 0; i < 3; i++) REQUIRE(l.append("e", "x", nlohmann::json::object(), "beta"));
    REQUIRE(l.append("e", "x", nlohmann::json::object(), "gamma"));
    auto r = l.most_active_tenant(); REQUIRE(r);
    CHECK(r.value() == "alpha");
}

TEST_CASE("most_active_tenant: empty chain returns not_found") {
    auto p = tmp_db("mat_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto r = l_.value().most_active_tenant();
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::not_found);
}

TEST_CASE("most_active_tenant: empty tenant competes alongside named tenants") {
    auto p = tmp_db("mat_empty_scope");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // Empty tenant has the most entries.
    for (int i = 0; i < 4; i++) REQUIRE(l.append("e", "x", nlohmann::json::object(), ""));
    REQUIRE(l.append("e", "x", nlohmann::json::object(), "alpha"));
    auto r = l.most_active_tenant(); REQUIRE(r);
    CHECK(r.value() == "");
}

TEST_CASE("most_active_tenant: single-tenant chain returns that tenant") {
    auto p = tmp_db("mat_single");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 7; i++) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, "only-one"));
    }
    auto r = l.most_active_tenant(); REQUIRE(r);
    CHECK(r.value() == "only-one");
}

// ============== Ledger::body_size_histogram ==============================

TEST_CASE("body_size_histogram: empty chain returns an empty histogram with hi=1") {
    auto p = tmp_db("bsh_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto r = l_.value().body_size_histogram(20); REQUIRE(r);
    CHECK(r.value().total() == 0u);
    CHECK(r.value().bin_count() == 20u);
    CHECK(r.value().lo() == 0.0);
    CHECK(r.value().hi() == 1.0);
}

TEST_CASE("body_size_histogram: bins=0 returns invalid_argument") {
    auto p = tmp_db("bsh_zero_bins");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "x", nlohmann::json::object(), ""));
    auto r = l.body_size_histogram(0);
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("body_size_histogram: total observations equal chain length") {
    auto p = tmp_db("bsh_total");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 25; i++) {
        REQUIRE(l.append("e", "x",
                         nlohmann::json{{"i", i}, {"pad", std::string(i * 4, 'a')}},
                         ""));
    }
    auto r = l.body_size_histogram(10); REQUIRE(r);
    CHECK(r.value().total() == 25u);
    CHECK(r.value().bin_count() == 10u);
    CHECK(r.value().lo() == 0.0);
    // hi must be the maximum body size observed, which is positive here.
    CHECK(r.value().hi() > 0.0);
}

TEST_CASE("body_size_histogram: hi tracks the maximum observed body size") {
    auto p = tmp_db("bsh_hi_max");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // Append entries with increasing body sizes; the largest body
    // governs hi.
    REQUIRE(l.append("e", "x", nlohmann::json{{"k", "a"}}, ""));            // small
    REQUIRE(l.append("e", "x",
                     nlohmann::json{{"k", std::string(64, 'b')}}, ""));     // medium
    REQUIRE(l.append("e", "x",
                     nlohmann::json{{"k", std::string(256, 'c')}}, ""));    // largest
    auto r = l.body_size_histogram(12); REQUIRE(r);
    CHECK(r.value().total() == 3u);
    CHECK(r.value().lo() == 0.0);
    CHECK(r.value().hi() > 0.0);
    // The max body size must be at least the canonical-JSON length of
    // the largest body's payload (256 c's plus quoting and the rest of
    // the object), so hi must comfortably exceed that lower bound.
    CHECK(r.value().hi() >= 256.0);
    // Bin invariants: well-formed lo<hi and the requested bin count.
    CHECK(r.value().bin_count() == 12u);
    CHECK(r.value().hi() > r.value().lo());
}

// ============== Ledger::tail_summary_string ==============================

TEST_CASE("tail_summary_string: n=0 returns the empty string") {
    auto p = tmp_db("tss_zero");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "x", nlohmann::json::object(), ""));
    auto r = l.tail_summary_string(0); REQUIRE(r);
    CHECK(r.value() == "");
}

TEST_CASE("tail_summary_string: empty chain returns empty string") {
    auto p = tmp_db("tss_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto r = l_.value().tail_summary_string(5); REQUIRE(r);
    CHECK(r.value() == "");
}

TEST_CASE("tail_summary_string: line layout is seq=, ts=, actor=, event= oldest-first") {
    auto p = tmp_db("tss_layout");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    auto e1 = l.append("alpha", "alice", nlohmann::json::object(), ""); REQUIRE(e1);
    auto e2 = l.append("beta",  "bob",   nlohmann::json::object(), ""); REQUIRE(e2);
    auto e3 = l.append("gamma", "carol", nlohmann::json::object(), ""); REQUIRE(e3);
    auto r = l.tail_summary_string(2); REQUIRE(r);
    // Two lines for the last two entries (seq=2 and seq=3) in oldest-first.
    auto nl = r.value().find('\n');
    REQUIRE(nl != std::string::npos);
    auto line0 = r.value().substr(0, nl);
    auto line1 = r.value().substr(nl + 1);
    // Oldest-first: line0 is seq=2 (bob/beta), line1 is seq=3 (carol/gamma).
    CHECK(line0.find("seq=2") != std::string::npos);
    CHECK(line0.find("actor=bob") != std::string::npos);
    CHECK(line0.find("event=beta") != std::string::npos);
    CHECK(line0.find("ts=" + e2.value().header.ts.iso8601()) != std::string::npos);
    CHECK(line1.find("seq=3") != std::string::npos);
    CHECK(line1.find("actor=carol") != std::string::npos);
    CHECK(line1.find("event=gamma") != std::string::npos);
    // No trailing newline.
    CHECK(r.value().back() != '\n');
}

TEST_CASE("tail_summary_string: n larger than length emits one line per entry") {
    auto p = tmp_db("tss_overflow");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 4; i++) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
    }
    auto r = l.tail_summary_string(99); REQUIRE(r);
    // Four entries -> four lines -> three newlines (no trailing).
    std::size_t newlines = 0;
    for (char c : r.value()) {
        if (c == '\n') ++newlines;
    }
    CHECK(newlines == 3u);
    // Oldest-first ordering: seq=1 appears before seq=4 in the buffer.
    auto p1 = r.value().find("seq=1");
    auto p4 = r.value().find("seq=4");
    REQUIRE(p1 != std::string::npos);
    REQUIRE(p4 != std::string::npos);
    CHECK(p1 < p4);
}

// ============== Ledger::recent_failures =================================

TEST_CASE("recent_failures: empty chain returns empty vector") {
    auto p = tmp_db("rf_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    auto r = l.recent_failures(10);
    REQUIRE(r);
    CHECK(r.value().empty());
    // n=0 is a cheap no-op even on a populated chain.
    REQUIRE(l.append("inference.committed", "sys",
                     nlohmann::json{{"status", "blocked.input"}}, ""));
    auto r0 = l.recent_failures(0);
    REQUIRE(r0);
    CHECK(r0.value().empty());
}

TEST_CASE("recent_failures: returns non-ok inference.committed entries most-recent first") {
    auto p = tmp_db("rf_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // Mix of ok / non-ok inference.committed and unrelated event types.
    REQUIRE(l.append("inference.committed", "sys",
                     nlohmann::json{{"status", "ok"}, {"i", 1}}, ""));
    REQUIRE(l.append("inference.committed", "sys",
                     nlohmann::json{{"status", "blocked.input"}, {"i", 2}}, ""));
    REQUIRE(l.append("drift.crossed", "sys",
                     nlohmann::json{{"status", "blocked.output"}, {"i", 3}}, ""));
    REQUIRE(l.append("inference.committed", "sys",
                     nlohmann::json{{"status", "model_error"}, {"i", 4}}, ""));
    REQUIRE(l.append("inference.committed", "sys",
                     nlohmann::json{{"status", "ok"}, {"i", 5}}, ""));
    REQUIRE(l.append("inference.committed", "sys",
                     nlohmann::json{{"status", "timeout"}, {"i", 6}}, ""));

    auto r = l.recent_failures(10);
    REQUIRE(r);
    REQUIRE(r.value().size() == 3u);
    // Most-recent first: seq=6 (timeout), seq=4 (model_error), seq=2 (blocked.input).
    CHECK(r.value()[0].header.seq == 6u);
    CHECK(r.value()[1].header.seq == 4u);
    CHECK(r.value()[2].header.seq == 2u);
    // All are inference.committed; the drift.crossed entry was excluded
    // even though its body had a non-ok status.
    for (const auto& e : r.value()) {
        CHECK(e.header.event_type == "inference.committed");
    }
}

TEST_CASE("recent_failures: bounded ring buffer keeps the last n matches") {
    auto p = tmp_db("rf_ring");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // Twelve failures interleaved with ok entries.
    for (int i = 0; i < 12; ++i) {
        REQUIRE(l.append("inference.committed", "sys",
                         nlohmann::json{{"status", "blocked.input"}, {"i", i}},
                         ""));
        REQUIRE(l.append("inference.committed", "sys",
                         nlohmann::json{{"status", "ok"}, {"i", i + 100}}, ""));
    }
    auto r = l.recent_failures(3);
    REQUIRE(r);
    REQUIRE(r.value().size() == 3u);
    // The last three failures are the i=9, 10, 11 entries — most recent
    // first means the i=11 failure leads.
    auto b0 = nlohmann::json::parse(r.value()[0].body_json);
    auto b1 = nlohmann::json::parse(r.value()[1].body_json);
    auto b2 = nlohmann::json::parse(r.value()[2].body_json);
    CHECK(b0["i"] == 11);
    CHECK(b1["i"] == 10);
    CHECK(b2["i"] == 9);
}

TEST_CASE("recent_failures: covers every documented non-ok status") {
    auto p = tmp_db("rf_statuses");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // Every status from src/runtime/inference.cpp except "ok".
    const char* statuses[] = {
        "blocked.input", "blocked.output", "model_error",
        "timeout", "cancelled", "aborted",
    };
    for (const char* s : statuses) {
        REQUIRE(l.append("inference.committed", "sys",
                         nlohmann::json{{"status", s}}, ""));
    }
    // One ok entry that must be skipped.
    REQUIRE(l.append("inference.committed", "sys",
                     nlohmann::json{{"status", "ok"}}, ""));

    auto r = l.recent_failures(20);
    REQUIRE(r);
    CHECK(r.value().size() == 6u);
}

// ============== Ledger::events_in_window_by_type ========================

TEST_CASE("events_in_window_by_type: empty chain and empty window return empty map") {
    auto p = tmp_db("eiwbt_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    auto r0 = l.events_in_window_by_type(
        Time{0}, Time::now() + std::chrono::seconds(60));
    REQUIRE(r0);
    CHECK(r0.value().empty());

    REQUIRE(l.append("a", "x", nlohmann::json::object(), ""));
    auto t = Time::now() + std::chrono::hours(24);
    auto r1 = l.events_in_window_by_type(t, t + std::chrono::seconds(1));
    REQUIRE(r1);
    CHECK(r1.value().empty());
}

TEST_CASE("events_in_window_by_type: from > to returns invalid_argument") {
    auto p = tmp_db("eiwbt_inv");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("a", "x", nlohmann::json::object(), ""));
    auto t1 = Time::now();
    auto t0 = t1 - std::chrono::seconds(1);
    auto r = l.events_in_window_by_type(t1, t0);
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
    // from == to is a valid (empty) window, not an error.
    auto rz = l.events_in_window_by_type(t1, t1);
    REQUIRE(rz);
    CHECK(rz.value().empty());
}

TEST_CASE("events_in_window_by_type: groups by event_type within [from, to)") {
    auto p = tmp_db("eiwbt_groups");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // Pre-window noise (must not appear in the result).
    for (int i = 0; i < 3; ++i) {
        REQUIRE(l.append("alpha", "x", nlohmann::json{{"i", i}}, ""));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto from = Time::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    // In-window events: 4 alphas, 2 betas, 1 gamma.
    for (int i = 0; i < 4; ++i) {
        REQUIRE(l.append("alpha", "x", nlohmann::json{{"i", i}}, ""));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    for (int i = 0; i < 2; ++i) {
        REQUIRE(l.append("beta", "x", nlohmann::json{{"i", i}}, ""));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(l.append("gamma", "x", nlohmann::json::object(), ""));
    auto to = Time::now() + std::chrono::seconds(60);

    auto r = l.events_in_window_by_type(from, to);
    REQUIRE(r);
    CHECK(r.value().size() == 3u);
    CHECK(r.value().at("alpha") == 4u);
    CHECK(r.value().at("beta")  == 2u);
    CHECK(r.value().at("gamma") == 1u);
    // Pre-window alphas must not contribute.
    auto everything = l.count_by_event_type();
    REQUIRE(everything);
    CHECK(everything.value().at("alpha") == 7u);  // 3 pre + 4 in window
}

TEST_CASE("events_in_window_by_type: half-open upper bound excludes equal-ts entry") {
    auto p = tmp_db("eiwbt_halfopen");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 5; ++i) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    auto e3 = l.at(3); REQUIRE(e3);
    // Window is [epoch, e3.ts) — e3 itself is excluded.
    auto r = l.events_in_window_by_type(Time{0}, e3.value().header.ts);
    REQUIRE(r);
    CHECK(r.value().at("e") == 2u);  // entries 1 and 2 only
}

// ============== Ledger::active_tenants_count ============================

TEST_CASE("active_tenants_count: empty chain returns 0") {
    auto p = tmp_db("atc_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto r = l_.value().active_tenants_count();
    REQUIRE(r);
    CHECK(r.value() == 0u);
}

TEST_CASE("active_tenants_count: counts distinct tenants including the empty one") {
    auto p = tmp_db("atc_distinct");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // alpha, beta, gamma, plus the empty tenant — four distinct values.
    REQUIRE(l.append("e", "x", nlohmann::json{{"i", 1}}, "alpha"));
    REQUIRE(l.append("e", "x", nlohmann::json{{"i", 2}}, "beta"));
    REQUIRE(l.append("e", "x", nlohmann::json{{"i", 3}}, "gamma"));
    REQUIRE(l.append("e", "x", nlohmann::json{{"i", 4}}, ""));
    REQUIRE(l.append("e", "x", nlohmann::json{{"i", 5}}, "alpha"));  // dup
    auto r = l.active_tenants_count();
    REQUIRE(r);
    CHECK(r.value() == 4u);
    // Agrees with tenants().size().
    auto ts = l.tenants();
    REQUIRE(ts);
    CHECK(ts.value().size() == r.value());
}

TEST_CASE("active_tenants_count: single-tenant chain returns 1") {
    auto p = tmp_db("atc_single");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 8; ++i) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, "only"));
    }
    auto r = l.active_tenants_count();
    REQUIRE(r);
    CHECK(r.value() == 1u);
}

TEST_CASE("active_tenants_count: empty tenant alone counts as one") {
    auto p = tmp_db("atc_empty_only");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 4; ++i) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
    }
    auto r = l.active_tenants_count();
    REQUIRE(r);
    CHECK(r.value() == 1u);
}

// ============== Ledger::any_blocked_in_window ===========================

TEST_CASE("any_blocked_in_window: empty chain and empty window return false") {
    auto p = tmp_db("abiw_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    auto r0 = l.any_blocked_in_window(
        Time{0}, Time::now() + std::chrono::seconds(60));
    REQUIRE(r0);
    CHECK(r0.value() == false);

    REQUIRE(l.append("inference.committed", "sys",
                     nlohmann::json{{"status", "blocked.input"}}, ""));
    // Window in the future excludes the only entry.
    auto far = Time::now() + std::chrono::hours(24);
    auto r1 = l.any_blocked_in_window(far, far + std::chrono::seconds(1));
    REQUIRE(r1);
    CHECK(r1.value() == false);
}

TEST_CASE("any_blocked_in_window: from > to returns invalid_argument") {
    auto p = tmp_db("abiw_inv");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("inference.committed", "sys",
                     nlohmann::json{{"status", "ok"}}, ""));
    auto t1 = Time::now();
    auto t0 = t1 - std::chrono::seconds(1);
    auto r = l.any_blocked_in_window(t1, t0);
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("any_blocked_in_window: detects a blocked.* entry inside the window") {
    auto p = tmp_db("abiw_hit");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    auto from = Time::now();
    REQUIRE(l.append("inference.committed", "sys",
                     nlohmann::json{{"status", "ok"}}, ""));
    REQUIRE(l.append("inference.committed", "sys",
                     nlohmann::json{{"status", "blocked.output"}}, ""));
    REQUIRE(l.append("inference.committed", "sys",
                     nlohmann::json{{"status", "ok"}}, ""));
    auto to = Time::now() + std::chrono::seconds(60);
    auto r = l.any_blocked_in_window(from, to);
    REQUIRE(r);
    CHECK(r.value() == true);
}

TEST_CASE("any_blocked_in_window: ignores blocked.* entries outside the window") {
    auto p = tmp_db("abiw_outside");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // Pre-window blocked entry (must not trigger a hit).
    REQUIRE(l.append("inference.committed", "sys",
                     nlohmann::json{{"status", "blocked.input"}}, ""));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto from = Time::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    // In-window: only ok entries.
    for (int i = 0; i < 3; ++i) {
        REQUIRE(l.append("inference.committed", "sys",
                         nlohmann::json{{"status", "ok"}}, ""));
    }
    auto to = Time::now() + std::chrono::seconds(60);
    auto r = l.any_blocked_in_window(from, to);
    REQUIRE(r);
    CHECK(r.value() == false);
}

// ============== Ledger::head_attestation_hex ============================

TEST_CASE("head_attestation_hex: shape <hash>:<key16>:<sig16> on a non-empty chain") {
    auto p = tmp_db("hah_shape");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 3; ++i) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
    }
    auto r = l.head_attestation_hex(); REQUIRE(r);
    auto s = r.value();
    // Two ':' separators producing three fields.
    auto first  = s.find(':');
    REQUIRE(first != std::string::npos);
    auto second = s.find(':', first + 1);
    REQUIRE(second != std::string::npos);
    CHECK(s.find(':', second + 1) == std::string::npos);

    auto head_field = s.substr(0, first);
    auto key_field  = s.substr(first + 1, second - first - 1);
    auto sig_field  = s.substr(second + 1);

    // head: full 64-char hex, equal to live head.
    CHECK(head_field.size() == Hash::size * 2);
    CHECK(head_field == l.head().hex());
    // key_id is exactly 16 chars (KeyStore::key_id is 16; we clamp).
    CHECK(key_field.size() == 16u);
    CHECK(key_field == l.key_id().substr(0, 16));
    // sig is 16 hex chars (the leading bytes of the full ed25519 sig).
    CHECK(sig_field.size() == 16u);
    // Distinct from head_attestation_json output.
    CHECK(s != l.head_attestation_json());
    // No JSON syntax — pure colon-separated hex/text.
    CHECK(s.find('{') == std::string::npos);
    CHECK(s.find('}') == std::string::npos);
    CHECK(s.find('\n') == std::string::npos);
}

TEST_CASE("head_attestation_hex: empty chain emits all-zero head with a real signature prefix") {
    auto p = tmp_db("hah_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    auto r = l.head_attestation_hex(); REQUIRE(r);
    auto s = r.value();
    // First field is the all-zero head hex.
    auto colon = s.find(':');
    REQUIRE(colon != std::string::npos);
    CHECK(s.substr(0, colon) == Hash::zero().hex());
    // The full string is well-formed: <64>:<16>:<16> = 64 + 1 + 16 + 1 + 16 = 98.
    CHECK(s.size() == Hash::size * 2 + 1 + 16 + 1 + 16);
    // The sig prefix (last 16 chars) must match the leading 16 of the
    // full Ed25519 signature over the zero head bytes — i.e. the same
    // prefix that head_attestation_json reports.
    auto j = nlohmann::json::parse(l.head_attestation_json());
    auto full_sig_hex = j.at("head_signature").get<std::string>();
    CHECK(s.substr(s.size() - 16) == full_sig_hex.substr(0, 16));
}

TEST_CASE("head_attestation_hex: head field advances as the chain advances; key stays stable") {
    auto p = tmp_db("hah_advance");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();

    REQUIRE(l.append("e", "x", nlohmann::json{{"i", 0}}, ""));
    auto r1 = l.head_attestation_hex(); REQUIRE(r1);
    auto s1 = r1.value();

    REQUIRE(l.append("e", "x", nlohmann::json{{"i", 1}}, ""));
    auto r2 = l.head_attestation_hex(); REQUIRE(r2);
    auto s2 = r2.value();

    auto head1 = s1.substr(0, s1.find(':'));
    auto head2 = s2.substr(0, s2.find(':'));
    CHECK(head1 != head2);

    // key_id field — middle slice — is identical across appends.
    auto colon1_a = s1.find(':');
    auto colon1_b = s1.find(':', colon1_a + 1);
    auto colon2_a = s2.find(':');
    auto colon2_b = s2.find(':', colon2_a + 1);
    auto key1 = s1.substr(colon1_a + 1, colon1_b - colon1_a - 1);
    auto key2 = s2.substr(colon2_a + 1, colon2_b - colon2_a - 1);
    CHECK(key1 == key2);
    CHECK(key1 == l.key_id().substr(0, 16));
}

TEST_CASE("head_attestation_hex: signature prefix verifies as a prefix of the real Ed25519 sig") {
    auto p = tmp_db("hah_sig");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "x", nlohmann::json::object(), ""));

    auto compact = l.head_attestation_hex(); REQUIRE(compact);
    auto cstr = compact.value();
    auto colon_a = cstr.find(':');
    auto colon_b = cstr.find(':', colon_a + 1);
    REQUIRE(colon_a != std::string::npos);
    REQUIRE(colon_b != std::string::npos);
    auto sig_prefix = cstr.substr(colon_b + 1);

    // The full sig from head_attestation_json must verify, and our
    // compact form must be its first 16 hex chars (= 8 leading bytes).
    auto j = nlohmann::json::parse(l.head_attestation_json());
    auto full = j.at("head_signature").get<std::string>();
    CHECK(full.substr(0, 16) == sig_prefix);

    // Tampering any nibble of the leading bytes invalidates the
    // reconstructed signature — sanity check that the prefix really
    // came from the live signing key.
    std::array<std::uint8_t, KeyStore::sig_bytes> sig{};
    REQUIRE(hex_to_bytes(full, sig));
    auto head = l.head();
    auto pk   = l.public_key();
    CHECK(KeyStore::verify(
        Bytes{head.bytes.data(), head.bytes.size()},
        std::span<const std::uint8_t, KeyStore::sig_bytes>{sig.data(), sig.size()},
        std::span<const std::uint8_t, KeyStore::pk_bytes>{pk.data(),   pk.size()}));
    sig[0] ^= 0xFFu;
    CHECK(!KeyStore::verify(
        Bytes{head.bytes.data(), head.bytes.size()},
        std::span<const std::uint8_t, KeyStore::sig_bytes>{sig.data(), sig.size()},
        std::span<const std::uint8_t, KeyStore::pk_bytes>{pk.data(),   pk.size()}));
}

// ============== Ledger::find_inference_by_input_hash =====================

TEST_CASE("find_inference_by_input_hash locates the matching inference.committed entry") {
    auto p = tmp_db("fibih_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 10; i++) {
        REQUIRE(l.append("inference.committed", "system:rt",
            nlohmann::json{{"inference_id", "inf_" + std::to_string(i)},
                           {"input_hash",   "h_" + std::to_string(i)},
                           {"status",       "ok"}}, ""));
    }
    auto e = l.find_inference_by_input_hash("h_4"); REQUIRE(e);
    auto body = nlohmann::json::parse(e.value().body_json);
    CHECK(body["input_hash"]   == "h_4");
    CHECK(body["inference_id"] == "inf_4");
    CHECK(e.value().header.seq == 5u);  // 1-indexed
    CHECK(e.value().header.event_type == "inference.committed");
}

TEST_CASE("find_inference_by_input_hash returns not_found for unknown hash") {
    auto p = tmp_db("fibih_miss");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("inference.committed", "system:rt",
        nlohmann::json{{"input_hash", "deadbeef"}, {"status", "ok"}}, ""));
    auto r = l.find_inference_by_input_hash("never_recorded");
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::not_found);
}

TEST_CASE("find_inference_by_input_hash rejects empty hash_hex") {
    auto p = tmp_db("fibih_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto r = l_.value().find_inference_by_input_hash("");
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("find_inference_by_input_hash returns first/oldest match if duplicates exist") {
    auto p = tmp_db("fibih_dup");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("inference.committed", "system:rt",
        nlohmann::json{{"input_hash", "shared"}, {"v", 1}}, ""));
    REQUIRE(l.append("inference.committed", "system:rt",
        nlohmann::json{{"input_hash", "shared"}, {"v", 2}}, ""));
    auto e = l.find_inference_by_input_hash("shared"); REQUIRE(e);
    auto body = nlohmann::json::parse(e.value().body_json);
    CHECK(body["v"] == 1);  // first/oldest match
    CHECK(e.value().header.seq == 1u);
}

TEST_CASE("find_inference_by_input_hash ignores non-inference.committed event types") {
    auto p = tmp_db("fibih_noevt");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // Decoy: another event_type that incidentally carries an
    // input_hash field — should NOT match.
    REQUIRE(l.append("custom.evt", "system:rt",
        nlohmann::json{{"input_hash", "decoy"}}, ""));
    REQUIRE(l.append("inference.committed", "system:rt",
        nlohmann::json{{"input_hash", "real"}, {"i", 7}}, ""));
    auto miss = l.find_inference_by_input_hash("decoy");
    CHECK(!miss);
    CHECK(miss.error().code() == ErrorCode::not_found);
    auto hit = l.find_inference_by_input_hash("real"); REQUIRE(hit);
    auto body = nlohmann::json::parse(hit.value().body_json);
    CHECK(body["i"] == 7);
}

// ============== Ledger::tenant_event_counts =============================

TEST_CASE("tenant_event_counts: empty chain returns empty map") {
    auto p = tmp_db("tec_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto r = l_.value().tenant_event_counts();
    REQUIRE(r);
    CHECK(r.value().empty());
}

TEST_CASE("tenant_event_counts: groups counts by (tenant, event_type)") {
    auto p = tmp_db("tec_grp");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("alpha", "x", nlohmann::json::object(), "t1"));
    REQUIRE(l.append("alpha", "x", nlohmann::json::object(), "t1"));
    REQUIRE(l.append("beta",  "x", nlohmann::json::object(), "t1"));
    REQUIRE(l.append("alpha", "x", nlohmann::json::object(), "t2"));
    REQUIRE(l.append("beta",  "x", nlohmann::json::object(), "t2"));
    REQUIRE(l.append("beta",  "x", nlohmann::json::object(), "t2"));
    auto r = l.tenant_event_counts(); REQUIRE(r);
    auto& m = r.value();
    REQUIRE(m.size() == 2);
    CHECK(m.at("t1").at("alpha") == 2u);
    CHECK(m.at("t1").at("beta")  == 1u);
    CHECK(m.at("t2").at("alpha") == 1u);
    CHECK(m.at("t2").at("beta")  == 2u);
    // Counts in the inner map only contain matching event types — no
    // zero-buckets for event types absent from a tenant.
    CHECK(m.at("t1").size() == 2);
    CHECK(m.at("t2").size() == 2);
}

TEST_CASE("tenant_event_counts: empty tenant has its own bucket") {
    auto p = tmp_db("tec_emptytenant");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("evt", "x", nlohmann::json::object(), ""));
    REQUIRE(l.append("evt", "x", nlohmann::json::object(), ""));
    REQUIRE(l.append("evt", "x", nlohmann::json::object(), "named"));
    auto r = l.tenant_event_counts(); REQUIRE(r);
    auto& m = r.value();
    REQUIRE(m.size() == 2);
    CHECK(m.at("").at("evt")      == 2u);
    CHECK(m.at("named").at("evt") == 1u);
}

TEST_CASE("tenant_event_counts: row-sum agrees with count_by_event_type and length") {
    auto p = tmp_db("tec_sum");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 12; ++i) {
        REQUIRE(l.append(i % 2 ? "alpha" : "beta",
                         "x", nlohmann::json{{"i", i}},
                         (i % 3 == 0) ? "t1" : "t2"));
    }
    auto tec = l.tenant_event_counts();   REQUIRE(tec);
    auto cbe = l.count_by_event_type();   REQUIRE(cbe);

    // Total over the nested map must equal length().
    std::uint64_t total = 0;
    for (const auto& [_, inner] : tec.value())
        for (const auto& [__, n] : inner) total += n;
    CHECK(total == l.length());

    // Sum-by-event-type across tenants must agree with the global tally.
    std::unordered_map<std::string, std::uint64_t> by_event;
    for (const auto& [_, inner] : tec.value())
        for (const auto& [evt, n] : inner) by_event[evt] += n;
    CHECK(by_event == cbe.value());
}

// ============== Ledger::count_consent_events ============================

TEST_CASE("count_consent_events: empty chain returns 0") {
    auto p = tmp_db("cce_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto r = l_.value().count_consent_events();
    REQUIRE(r);
    CHECK(r.value() == 0u);
}

TEST_CASE("count_consent_events: tallies consent.granted + consent.revoked + consent.* extensions") {
    auto p = tmp_db("cce_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("consent.granted", "x", nlohmann::json::object(), ""));
    REQUIRE(l.append("consent.revoked", "x", nlohmann::json::object(), ""));
    REQUIRE(l.append("consent.granted", "x", nlohmann::json::object(), ""));
    REQUIRE(l.append("consent.future",  "x", nlohmann::json::object(), ""));
    // Non-consent events are ignored.
    REQUIRE(l.append("inference.committed", "x",
                     nlohmann::json{{"status", "ok"}}, ""));
    REQUIRE(l.append("drift.crossed", "x", nlohmann::json::object(), ""));
    auto r = l.count_consent_events();
    REQUIRE(r);
    CHECK(r.value() == 4u);
}

TEST_CASE("count_consent_events: ignores prefix-similar event types like 'consent' or 'consents.*'") {
    auto p = tmp_db("cce_strict");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("consent",           "x", nlohmann::json::object(), ""));
    REQUIRE(l.append("consents.granted",  "x", nlohmann::json::object(), ""));
    REQUIRE(l.append("preconsent.x",      "x", nlohmann::json::object(), ""));
    REQUIRE(l.append("consent.granted",   "x", nlohmann::json::object(), ""));
    auto r = l.count_consent_events();
    REQUIRE(r);
    CHECK(r.value() == 1u);  // only the dotted-prefix match
}

TEST_CASE("count_consent_events: agrees with manual count_by_event_type tally") {
    auto p = tmp_db("cce_agree");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 5; ++i)
        REQUIRE(l.append("consent.granted", "x", nlohmann::json::object(), ""));
    for (int i = 0; i < 3; ++i)
        REQUIRE(l.append("consent.revoked", "x", nlohmann::json::object(), ""));
    for (int i = 0; i < 7; ++i)
        REQUIRE(l.append("inference.committed", "x",
                         nlohmann::json{{"status", "ok"}}, ""));
    auto cce = l.count_consent_events();    REQUIRE(cce);
    auto cbe = l.count_by_event_type();     REQUIRE(cbe);
    auto manual = cbe.value().at("consent.granted")
                + cbe.value().at("consent.revoked");
    CHECK(cce.value() == manual);
    CHECK(cce.value() == 8u);
}

// ============== Ledger::distinct_inference_ids_count ====================

TEST_CASE("distinct_inference_ids_count: empty chain returns 0") {
    auto p = tmp_db("dic_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto r = l_.value().distinct_inference_ids_count();
    REQUIRE(r);
    CHECK(r.value() == 0u);
}

TEST_CASE("distinct_inference_ids_count: counts distinct ids, dedups duplicates") {
    auto p = tmp_db("dic_dedup");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // Two distinct ids with a dup; total entries = 5, distinct ids = 2.
    REQUIRE(l.append("inference.committed", "sys",
        nlohmann::json{{"inference_id", "inf_a"}, {"status", "ok"}}, ""));
    REQUIRE(l.append("inference.committed", "sys",
        nlohmann::json{{"inference_id", "inf_b"}, {"status", "ok"}}, ""));
    REQUIRE(l.append("inference.committed", "sys",
        nlohmann::json{{"inference_id", "inf_a"}, {"status", "ok"}}, ""));
    REQUIRE(l.append("inference.committed", "sys",
        nlohmann::json{{"inference_id", "inf_a"}, {"status", "ok"}}, ""));
    REQUIRE(l.append("inference.committed", "sys",
        nlohmann::json{{"inference_id", "inf_b"}, {"status", "ok"}}, ""));
    auto r = l.distinct_inference_ids_count();
    REQUIRE(r);
    CHECK(r.value() == 2u);
}

TEST_CASE("distinct_inference_ids_count: ignores non-inference.committed entries and missing fields") {
    auto p = tmp_db("dic_filter");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // Decoy carrying inference_id under a different event_type — ignored.
    REQUIRE(l.append("custom.evt", "sys",
        nlohmann::json{{"inference_id", "decoy"}}, ""));
    // Decoy missing the inference_id field.
    REQUIRE(l.append("inference.committed", "sys",
        nlohmann::json{{"status", "ok"}}, ""));
    // Decoy carrying a non-string inference_id — ignored.
    REQUIRE(l.append("inference.committed", "sys",
        nlohmann::json{{"inference_id", 42}, {"status", "ok"}}, ""));
    // Real distinct entries.
    REQUIRE(l.append("inference.committed", "sys",
        nlohmann::json{{"inference_id", "real_1"}, {"status", "ok"}}, ""));
    REQUIRE(l.append("inference.committed", "sys",
        nlohmann::json{{"inference_id", "real_2"}, {"status", "ok"}}, ""));
    auto r = l.distinct_inference_ids_count();
    REQUIRE(r);
    CHECK(r.value() == 2u);
}

TEST_CASE("distinct_inference_ids_count: survives reopen") {
    auto p = tmp_db("dic_reopen");
    {
        auto l_ = Ledger::open(p); REQUIRE(l_);
        for (int i = 0; i < 25; ++i) {
            REQUIRE(l_.value().append("inference.committed", "sys",
                nlohmann::json{{"inference_id", "inf_" + std::to_string(i % 5)},
                               {"status", "ok"}}, ""));
        }
    }
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto r = l_.value().distinct_inference_ids_count();
    REQUIRE(r);
    CHECK(r.value() == 5u);
}

// ============== Ledger::aborted_inference_count =========================

TEST_CASE("aborted_inference_count: empty chain returns 0") {
    auto p = tmp_db("aic_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto r = l_.value().aborted_inference_count();
    REQUIRE(r);
    CHECK(r.value() == 0u);
}

TEST_CASE("aborted_inference_count: tallies inference.aborted and only inference.aborted") {
    auto p = tmp_db("aic_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("inference.aborted",  "sys",
                     nlohmann::json{{"reason", "raii_drop"}}, ""));
    REQUIRE(l.append("inference.aborted",  "sys",
                     nlohmann::json{{"reason", "timeout"}}, ""));
    REQUIRE(l.append("inference.committed", "sys",
                     nlohmann::json{{"status", "ok"}}, ""));
    REQUIRE(l.append("inference.committed", "sys",
                     nlohmann::json{{"status", "aborted"}}, ""));  // body says "aborted" but event_type isn't
    REQUIRE(l.append("drift.crossed", "sys", nlohmann::json::object(), ""));
    auto r = l.aborted_inference_count();
    REQUIRE(r);
    CHECK(r.value() == 2u);
}

TEST_CASE("aborted_inference_count: agrees with count_by_event_type[\"inference.aborted\"]") {
    auto p = tmp_db("aic_agree");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 4; ++i)
        REQUIRE(l.append("inference.aborted", "sys",
                         nlohmann::json::object(), ""));
    for (int i = 0; i < 7; ++i)
        REQUIRE(l.append("inference.committed", "sys",
                         nlohmann::json{{"status", "ok"}}, ""));
    auto aic = l.aborted_inference_count(); REQUIRE(aic);
    auto cbe = l.count_by_event_type();     REQUIRE(cbe);
    CHECK(aic.value() == cbe.value().at("inference.aborted"));
    CHECK(aic.value() == 4u);
}

TEST_CASE("aborted_inference_count: substring 'aborted' in other event types does not match") {
    auto p = tmp_db("aic_strict");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("inference.aborted_run",  "sys",
                     nlohmann::json::object(), ""));
    REQUIRE(l.append("custom.aborted",          "sys",
                     nlohmann::json::object(), ""));
    REQUIRE(l.append("inference.aborted",       "sys",
                     nlohmann::json::object(), ""));
    auto r = l.aborted_inference_count();
    REQUIRE(r);
    CHECK(r.value() == 1u);
}

// ============== Ledger::range_for_actor_and_patient =====================

TEST_CASE("range_for_actor_and_patient: returns matching inferences in seq order") {
    auto p = tmp_db("rfap_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // Mix actors and patients; expect only (alice, p1) inferences.
    REQUIRE(l.append("inference.committed", "alice",
        nlohmann::json{{"patient", "p1"}, {"i", 1}}, ""));
    REQUIRE(l.append("inference.committed", "bob",
        nlohmann::json{{"patient", "p1"}, {"i", 2}}, ""));
    REQUIRE(l.append("inference.committed", "alice",
        nlohmann::json{{"patient", "p2"}, {"i", 3}}, ""));
    REQUIRE(l.append("inference.committed", "alice",
        nlohmann::json{{"patient", "p1"}, {"i", 4}}, ""));
    REQUIRE(l.append("inference.committed", "alice",
        nlohmann::json{{"patient", "p1"}, {"i", 5}}, ""));

    auto r = l.range_for_actor_and_patient("alice", "p1");
    REQUIRE(r);
    REQUIRE(r.value().size() == 3u);
    // Seq-ascending order.
    CHECK(r.value()[0].header.seq == 1u);
    CHECK(r.value()[1].header.seq == 4u);
    CHECK(r.value()[2].header.seq == 5u);
    for (const auto& e : r.value()) {
        CHECK(e.header.actor == "alice");
        CHECK(e.header.event_type == "inference.committed");
        auto body = nlohmann::json::parse(e.body_json);
        CHECK(body.at("patient").get<std::string>() == "p1");
    }
}

TEST_CASE("range_for_actor_and_patient: empty actor or empty patient -> invalid_argument") {
    auto p = tmp_db("rfap_invalid");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    auto r1 = l.range_for_actor_and_patient("", "p1");
    CHECK(!r1);
    CHECK(r1.error().code() == ErrorCode::invalid_argument);
    auto r2 = l.range_for_actor_and_patient("alice", "");
    CHECK(!r2);
    CHECK(r2.error().code() == ErrorCode::invalid_argument);
    auto r3 = l.range_for_actor_and_patient("", "");
    CHECK(!r3);
    CHECK(r3.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("range_for_actor_and_patient: skips non-inference.committed and unknown matches") {
    auto p = tmp_db("rfap_skips");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // Decoy: actor matches, patient body field present, but event_type
    // is not inference.committed — should be skipped.
    REQUIRE(l.append("custom.evt", "alice",
        nlohmann::json{{"patient", "p1"}}, ""));
    // Decoy: substring "p1" appears but as part of another field.
    REQUIRE(l.append("inference.committed", "alice",
        nlohmann::json{{"input_hash", "p1xx"}, {"patient", "p_other"}}, ""));
    // Decoy: drift event — not inference.committed.
    REQUIRE(l.append("drift.crossed", "alice",
        nlohmann::json{{"patient", "p1"}}, ""));
    // Real match.
    REQUIRE(l.append("inference.committed", "alice",
        nlohmann::json{{"patient", "p1"}, {"v", 99}}, ""));

    auto r = l.range_for_actor_and_patient("alice", "p1");
    REQUIRE(r);
    REQUIRE(r.value().size() == 1u);
    auto body = nlohmann::json::parse(r.value()[0].body_json);
    CHECK(body.at("v").get<int>() == 99);

    // Unknown actor or unknown patient returns the empty vector.
    auto rg = l.range_for_actor_and_patient("ghost", "p1");
    REQUIRE(rg); CHECK(rg.value().empty());
    auto rp = l.range_for_actor_and_patient("alice", "nobody");
    REQUIRE(rp); CHECK(rp.value().empty());
}

// ============== Ledger::tail_with_status ================================

TEST_CASE("tail_with_status: returns last n with matching status, most-recent first") {
    auto p = tmp_db("tws_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // Six entries: timeout, ok, timeout, blocked.input, ok, timeout.
    REQUIRE(l.append("inference.committed", "sys",
        nlohmann::json{{"status", "timeout"}, {"i", 1}}, ""));
    REQUIRE(l.append("inference.committed", "sys",
        nlohmann::json{{"status", "ok"}, {"i", 2}}, ""));
    REQUIRE(l.append("inference.committed", "sys",
        nlohmann::json{{"status", "timeout"}, {"i", 3}}, ""));
    REQUIRE(l.append("inference.committed", "sys",
        nlohmann::json{{"status", "blocked.input"}, {"i", 4}}, ""));
    REQUIRE(l.append("inference.committed", "sys",
        nlohmann::json{{"status", "ok"}, {"i", 5}}, ""));
    REQUIRE(l.append("inference.committed", "sys",
        nlohmann::json{{"status", "timeout"}, {"i", 6}}, ""));

    auto r = l.tail_with_status("timeout", 5);
    REQUIRE(r);
    REQUIRE(r.value().size() == 3u);
    // Most-recent first: seq=6, seq=3, seq=1.
    CHECK(r.value()[0].header.seq == 6u);
    CHECK(r.value()[1].header.seq == 3u);
    CHECK(r.value()[2].header.seq == 1u);
    for (const auto& e : r.value()) {
        CHECK(e.header.event_type == "inference.committed");
        auto body = nlohmann::json::parse(e.body_json);
        CHECK(body.at("status").get<std::string>() == "timeout");
    }
}

TEST_CASE("tail_with_status: empty status -> invalid_argument; n=0 -> empty vector") {
    auto p = tmp_db("tws_invalid");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("inference.committed", "sys",
        nlohmann::json{{"status", "timeout"}}, ""));

    auto r1 = l.tail_with_status("", 10);
    CHECK(!r1);
    CHECK(r1.error().code() == ErrorCode::invalid_argument);

    auto r2 = l.tail_with_status("timeout", 0);
    REQUIRE(r2); CHECK(r2.value().empty());

    // No matches returns the empty vector (not an error).
    auto r3 = l.tail_with_status("never_seen", 10);
    REQUIRE(r3); CHECK(r3.value().empty());
}

TEST_CASE("tail_with_status: ring buffer keeps last n; ignores non-inference.committed") {
    auto p = tmp_db("tws_ring");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // Decoy: event_type matches "blocked.input" status but not type.
    REQUIRE(l.append("custom.evt", "sys",
        nlohmann::json{{"status", "blocked.input"}, {"i", -1}}, ""));
    // Twelve real blocked.input failures; expect last three.
    for (int i = 0; i < 12; ++i) {
        REQUIRE(l.append("inference.committed", "sys",
            nlohmann::json{{"status", "blocked.input"}, {"i", i}}, ""));
    }
    auto r = l.tail_with_status("blocked.input", 3);
    REQUIRE(r);
    REQUIRE(r.value().size() == 3u);
    auto b0 = nlohmann::json::parse(r.value()[0].body_json);
    auto b1 = nlohmann::json::parse(r.value()[1].body_json);
    auto b2 = nlohmann::json::parse(r.value()[2].body_json);
    CHECK(b0.at("i").get<int>() == 11);
    CHECK(b1.at("i").get<int>() == 10);
    CHECK(b2.at("i").get<int>() ==  9);
    // Decoy event_type was excluded.
    for (const auto& e : r.value()) {
        CHECK(e.header.event_type == "inference.committed");
    }
}

TEST_CASE("tail_with_status: empty chain returns empty vector") {
    auto p = tmp_db("tws_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto r = l_.value().tail_with_status("timeout", 50);
    REQUIRE(r);
    CHECK(r.value().empty());
}

// ============== Ledger::peak_throughput_per_second ======================

TEST_CASE("peak_throughput_per_second: empty chain returns 0") {
    auto p = tmp_db("pkt_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto r = l_.value().peak_throughput_per_second();
    REQUIRE(r);
    CHECK(r.value() == 0u);
}

TEST_CASE("peak_throughput_per_second: single entry returns 0 (no measurable burst)") {
    auto p = tmp_db("pkt_single");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "x", nlohmann::json::object(), ""));
    auto r = l.peak_throughput_per_second();
    REQUIRE(r);
    CHECK(r.value() == 0u);
}

TEST_CASE("peak_throughput_per_second: dense burst lifts the peak above scattered baseline") {
    auto p = tmp_db("pkt_burst");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // A tight burst of 30 appends within ~30ms easily falls within a
    // single 1s window; the peak must therefore be at least 30.
    for (int i = 0; i < 30; ++i) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
    }
    auto r = l.peak_throughput_per_second();
    REQUIRE(r);
    CHECK(r.value() >= 30u);
}

TEST_CASE("peak_throughput_per_second: window cuts off entries older than 1s") {
    auto p = tmp_db("pkt_cutoff");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // Two entries, then sleep > 1s, then five entries — peak should be
    // 5 (the second cluster), not 7. The ts on each entry is wall-clock
    // from Time::now(), so the spaced cluster crosses the 1s boundary.
    for (int i = 0; i < 2; ++i) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    for (int i = 0; i < 5; ++i) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i + 100}}, ""));
    }
    auto r = l.peak_throughput_per_second();
    REQUIRE(r);
    // Tight cluster of 5 lands within the same 1s window; older 2-cluster
    // is fully outside the window when the 3rd-of-second-cluster entry
    // is processed.
    CHECK(r.value() == 5u);
}

// ============== Ledger::find_first_consent_grant_for ====================

TEST_CASE("find_first_consent_grant_for: finds the matching consent.granted entry") {
    auto p = tmp_db("ffcg_basic");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("consent.granted", "system:consent",
        nlohmann::json{{"patient", "p_alice"}, {"v", 1}}, ""));
    REQUIRE(l.append("consent.granted", "system:consent",
        nlohmann::json{{"patient", "p_bob"}, {"v", 2}}, ""));
    auto e = l.find_first_consent_grant_for("p_bob");
    REQUIRE(e);
    auto body = nlohmann::json::parse(e.value().body_json);
    CHECK(body.at("patient").get<std::string>() == "p_bob");
    CHECK(body.at("v").get<int>() == 2);
    CHECK(e.value().header.seq == 2u);
    CHECK(e.value().header.event_type == "consent.granted");
}

TEST_CASE("find_first_consent_grant_for: returns first/oldest match for duplicates") {
    auto p = tmp_db("ffcg_dup");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("consent.granted", "system:consent",
        nlohmann::json{{"patient", "p_alice"}, {"v", 1}}, ""));
    REQUIRE(l.append("consent.granted", "system:consent",
        nlohmann::json{{"patient", "p_alice"}, {"v", 2}}, ""));
    REQUIRE(l.append("consent.granted", "system:consent",
        nlohmann::json{{"patient", "p_alice"}, {"v", 3}}, ""));
    auto e = l.find_first_consent_grant_for("p_alice");
    REQUIRE(e);
    auto body = nlohmann::json::parse(e.value().body_json);
    CHECK(body.at("v").get<int>() == 1);
    CHECK(e.value().header.seq == 1u);
}

TEST_CASE("find_first_consent_grant_for: empty patient -> invalid_argument") {
    auto p = tmp_db("ffcg_inv");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto r = l_.value().find_first_consent_grant_for("");
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("find_first_consent_grant_for: not_found if no consent.granted matches") {
    auto p = tmp_db("ffcg_miss");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    // Decoy: consent.revoked carries the patient but isn't a grant.
    REQUIRE(l.append("consent.revoked", "system:consent",
        nlohmann::json{{"patient", "p_alice"}}, ""));
    // Decoy: inference.committed carries patient but isn't consent.
    REQUIRE(l.append("inference.committed", "system:rt",
        nlohmann::json{{"patient", "p_alice"}, {"status", "ok"}}, ""));
    auto r = l.find_first_consent_grant_for("p_alice");
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::not_found);

    // Empty-chain probe also returns not_found.
    auto p2 = tmp_db("ffcg_empty");
    auto l2_ = Ledger::open(p2); REQUIRE(l2_);
    auto r2 = l2_.value().find_first_consent_grant_for("p_alice");
    CHECK(!r2);
    CHECK(r2.error().code() == ErrorCode::not_found);
}

// ============== Ledger::head_attestation_hex_short ======================

TEST_CASE("head_attestation_hex_short: 24-char shape <8>:<8>:<6> on a non-empty chain") {
    auto p = tmp_db("hahs_shape");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    for (int i = 0; i < 3; ++i) {
        REQUIRE(l.append("e", "x", nlohmann::json{{"i", i}}, ""));
    }
    auto s = l.head_attestation_hex_short();
    CHECK(s.size() == 24u);  // 8 + 1 + 8 + 1 + 6
    auto first  = s.find(':');
    REQUIRE(first != std::string::npos);
    auto second = s.find(':', first + 1);
    REQUIRE(second != std::string::npos);
    CHECK(s.find(':', second + 1) == std::string::npos);

    auto head_field = s.substr(0, first);
    auto key_field  = s.substr(first + 1, second - first - 1);
    auto sig_field  = s.substr(second + 1);

    CHECK(head_field.size() == 8u);
    CHECK(head_field == l.head().hex().substr(0, 8));
    CHECK(key_field.size() == 8u);
    CHECK(key_field == l.key_id().substr(0, 8));
    CHECK(sig_field.size() == 6u);

    // Distinct from head_attestation_hex (which is 98 chars).
    auto full = l.head_attestation_hex();
    REQUIRE(full);
    CHECK(s != full.value());
    CHECK(s.size() < full.value().size());
}

TEST_CASE("head_attestation_hex_short: empty chain emits all-zero head prefix and stable shape") {
    auto p = tmp_db("hahs_empty");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    auto s = l.head_attestation_hex_short();
    CHECK(s.size() == 24u);
    auto colon = s.find(':');
    REQUIRE(colon != std::string::npos);
    // First 8 chars of the all-zero hash hex.
    CHECK(s.substr(0, colon) == Hash::zero().hex().substr(0, 8));
}

TEST_CASE("head_attestation_hex_short: head field advances; key field is stable across appends") {
    auto p = tmp_db("hahs_advance");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();

    REQUIRE(l.append("e", "x", nlohmann::json{{"i", 0}}, ""));
    auto s1 = l.head_attestation_hex_short();
    REQUIRE(l.append("e", "x", nlohmann::json{{"i", 1}}, ""));
    auto s2 = l.head_attestation_hex_short();

    auto head1 = s1.substr(0, s1.find(':'));
    auto head2 = s2.substr(0, s2.find(':'));
    CHECK(head1 != head2);

    // Middle slice (key_id) is identical.
    auto colon1_a = s1.find(':');
    auto colon1_b = s1.find(':', colon1_a + 1);
    auto colon2_a = s2.find(':');
    auto colon2_b = s2.find(':', colon2_a + 1);
    auto key1 = s1.substr(colon1_a + 1, colon1_b - colon1_a - 1);
    auto key2 = s2.substr(colon2_a + 1, colon2_b - colon2_a - 1);
    CHECK(key1 == key2);
    CHECK(key1 == l.key_id().substr(0, 8));
}

TEST_CASE("head_attestation_hex_short: distinct from head_attestation_hex on identical chain") {
    auto p = tmp_db("hahs_distinct");
    auto l_ = Ledger::open(p); REQUIRE(l_);
    auto& l = l_.value();
    REQUIRE(l.append("e", "x", nlohmann::json::object(), ""));
    auto compact_short = l.head_attestation_hex_short();
    auto compact_full  = l.head_attestation_hex();
    REQUIRE(compact_full);
    CHECK(compact_short != compact_full.value());
    // The short head prefix is the leading 8 chars of the full one.
    CHECK(compact_short.substr(0, 8) == compact_full.value().substr(0, 8));
}
