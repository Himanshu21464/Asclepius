// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
//
// End-to-end tests against a real PostgreSQL server. Skipped unless the
// environment variable ASCLEPIUS_PG_URI is set, e.g.
//
//   export ASCLEPIUS_PG_URI=postgres://asclepius:asclepius@127.0.0.1/asclepius
//   build/tests/asclepius_tests
//
// Each test case carves its own table-isolated namespace by truncating
// asclepius_ledger and asclepius_meta before running, so they're safe to
// run repeatedly against the same database.

#include <doctest/doctest.h>

#include "asclepius/asclepius.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using namespace asclepius;
using namespace std::chrono_literals;

namespace {

const char* pg_uri() { return std::getenv("ASCLEPIUS_PG_URI"); }

bool pg_available() { return pg_uri() != nullptr && std::string{pg_uri()}.size() > 0; }

// Wipe the ledger between tests using a one-shot Ledger to issue the truncate
// via libpq directly. (We don't expose truncate on Ledger — the chain is
// append-only by design — so we go around it for test setup.)
void truncate_pg_ledger() {
    // Re-open the storage purely to issue the truncate. Easiest way: use
    // psql via a system() call. But we want zero new test deps; instead we
    // just open Ledger, which init_schema()s, then issue raw SQL via libpq.
    // Since libpq isn't a public dependency, we shell out instead.
    std::string cmd = std::string("PGPASSWORD=asclepius psql -h 127.0.0.1 -U asclepius -d asclepius "
                                  "-c 'TRUNCATE TABLE asclepius_ledger; TRUNCATE TABLE asclepius_meta;' "
                                  ">/dev/null 2>&1");
    auto rc = std::system(cmd.c_str()); (void)rc;
}

}  // namespace

TEST_CASE("[postgres] open + append + verify against live PG") {
    if (!pg_available()) {
        MESSAGE("ASCLEPIUS_PG_URI unset; skipping postgres tests");
        return;
    }
    truncate_pg_ledger();

    auto rt = Runtime::open_uri(pg_uri());
    REQUIRE(rt);

    auto pid = PatientId::pseudonymous("p_pg_smoke");
    auto tok = rt.value().consent().grant(pid, {Purpose::triage}, 1h);
    REQUIRE(tok);

    auto inf = rt.value().begin_inference({
        .model            = ModelId{"scribe", "v3"},
        .actor            = ActorId::clinician("smith"),
        .patient          = pid,
        .encounter        = EncounterId::make(),
        .purpose          = Purpose::triage,
        .tenant           = TenantId{},
        .consent_token_id = tok.value().token_id,
    });
    REQUIRE(inf);
    REQUIRE(inf.value().run("hello postgres",
                            [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(inf.value().commit());

    // The ledger contains: at least the inference event. (The runtime also
    // appends consent.granted, model.attested, etc.) Verify the full chain
    // is internally consistent.
    auto v = rt.value().ledger().verify();
    REQUIRE(v);
    CHECK(rt.value().ledger().length() >= 1);
}

TEST_CASE("[postgres] tail and at round-trip the same entries") {
    if (!pg_available()) return;
    truncate_pg_ledger();

    auto rt = Runtime::open_uri(pg_uri());
    REQUIRE(rt);
    auto pid = PatientId::pseudonymous("p_pg_tail");
    auto tok = rt.value().consent().grant(pid, {Purpose::triage}, 1h);
    REQUIRE(tok);

    for (int i = 0; i < 5; ++i) {
        auto inf = rt.value().begin_inference({
            .model            = ModelId{"scribe", "v3"},
            .actor            = ActorId::clinician("smith"),
            .patient          = pid,
            .encounter        = EncounterId::make(),
            .purpose          = Purpose::triage,
            .tenant           = TenantId{},
            .consent_token_id = tok.value().token_id,
        });
        REQUIRE(inf);
        REQUIRE(inf.value().run("entry " + std::to_string(i),
                                [](std::string s) -> Result<std::string> { return s; }));
        REQUIRE(inf.value().commit());
    }

    auto len = rt.value().ledger().length();
    CHECK(len >= 5);

    auto t = rt.value().ledger().tail(3);
    REQUIRE(t);
    CHECK(t.value().size() == 3);

    // tail returns most-recent first; their seqs must be strictly decreasing
    for (std::size_t i = 1; i < t.value().size(); ++i) {
        CHECK(t.value()[i - 1].header.seq > t.value()[i].header.seq);
    }

    // at(seq) for the most-recent entry matches tail()[0].
    auto e = rt.value().ledger().at(t.value()[0].header.seq);
    REQUIRE(e);
    CHECK(e.value().entry_hash().bytes
          == t.value()[0].entry_hash().bytes);
}

TEST_CASE("[postgres] verify catches retroactive entry mutation") {
    if (!pg_available()) return;
    truncate_pg_ledger();

    auto rt = Runtime::open_uri(pg_uri());
    REQUIRE(rt);
    auto pid = PatientId::pseudonymous("p_pg_tamper");
    auto tok = rt.value().consent().grant(pid, {Purpose::triage}, 1h);
    REQUIRE(tok);

    auto inf = rt.value().begin_inference({
        .model            = ModelId{"scribe", "v3"},
        .actor            = ActorId::clinician("smith"),
        .patient          = pid,
        .encounter        = EncounterId::make(),
        .purpose          = Purpose::triage,
        .tenant           = TenantId{},
        .consent_token_id = tok.value().token_id,
    });
    REQUIRE(inf);
    REQUIRE(inf.value().run("real input",
                            [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(inf.value().commit());

    // First verify clean.
    REQUIRE(rt.value().ledger().verify());

    // Now sneak in via raw SQL — flip a byte of one body. Our verify must
    // catch this on the next run.
    std::string cmd = "PGPASSWORD=asclepius psql -h 127.0.0.1 -U asclepius -d asclepius "
                      "-c \"UPDATE asclepius_ledger SET body = body || ' ' WHERE seq = 1;\" "
                      ">/dev/null 2>&1";
    auto rc = std::system(cmd.c_str()); (void)rc;

    // Reopen the ledger so the in-memory cached head is recomputed from disk.
    auto rt2 = Runtime::open_uri(pg_uri());
    REQUIRE(rt2);
    auto v = rt2.value().ledger().verify();
    CHECK(!v);
}

TEST_CASE("[postgres] range_by_time covers entries within a window") {
    if (!pg_available()) return;
    truncate_pg_ledger();

    auto rt = Runtime::open_uri(pg_uri());
    REQUIRE(rt);
    auto pid = PatientId::pseudonymous("p_pg_time");
    auto tok = rt.value().consent().grant(pid, {Purpose::triage}, 1h);
    REQUIRE(tok);

    Time t0 = Time::now() - 1s;
    for (int i = 0; i < 4; ++i) {
        auto inf = rt.value().begin_inference({
            .model            = ModelId{"scribe", "v3"},
            .actor            = ActorId::clinician("smith"),
            .patient          = pid,
            .encounter        = EncounterId::make(),
            .purpose          = Purpose::triage,
            .tenant           = TenantId{},
            .consent_token_id = tok.value().token_id,
        });
        REQUIRE(inf);
        REQUIRE(inf.value().run("e", [](std::string s) -> Result<std::string> { return s; }));
        REQUIRE(inf.value().commit());
    }
    Time t1 = Time::now() + 1s;

    auto in_window = rt.value().ledger().range_by_time(t0, t1);
    REQUIRE(in_window);
    CHECK(in_window.value().size() >= 4);

    // Empty future window returns 0 entries.
    auto future = rt.value().ledger().range_by_time(t1 + 1h, t1 + 2h);
    REQUIRE(future);
    CHECK(future.value().empty());
}

TEST_CASE("[postgres] LedgerMigrator: SQLite → Postgres preserves the chain") {
    if (!pg_available()) return;
    truncate_pg_ledger();

    // Build a fresh SQLite ledger with a few entries via Runtime,
    // capturing the keystore so we can pass the same key when verifying
    // the destination.
    auto src_path = std::filesystem::temp_directory_path()
                  / ("asc_mig_" + std::to_string(std::random_device{}()) + ".db");
    {
        auto rt = Runtime::open(src_path);
        REQUIRE(rt);
        auto pid = PatientId::pseudonymous("p_mig");
        auto tok = rt.value().consent().grant(pid, {Purpose::triage}, 1h);
        REQUIRE(tok);
        for (int i = 0; i < 3; ++i) {
            auto inf = rt.value().begin_inference({
                .model            = ModelId{"scribe", "v3"},
                .actor            = ActorId::clinician("smith"),
                .patient          = pid,
                .encounter        = EncounterId::make(),
                .purpose          = Purpose::triage,
                .tenant           = TenantId{},
                .consent_token_id = tok.value().token_id,
            });
            REQUIRE(inf);
            REQUIRE(inf.value().run("hello",
                                    [](std::string s) -> Result<std::string> { return s; }));
            REQUIRE(inf.value().commit());
        }
        REQUIRE(rt.value().ledger().verify());
    }

    // The signing key is at <src_path>.key — this is what the migrator
    // doesn't need to read (entries are already signed) but the destination
    // needs to verify against the same public key.
    auto key_path = src_path;
    key_path.replace_extension(".key");
    std::ifstream kf(key_path);
    std::string blob((std::istreambuf_iterator<char>(kf)), {});
    auto key_for_verify = KeyStore::deserialize(blob);
    REQUIRE(key_for_verify);

    auto stats = LedgerMigrator::copy(src_path.string(), pg_uri(),
                                      std::move(key_for_verify.value()));
    REQUIRE(stats);
    CHECK(stats.value().entries_copied >= 3);

    // Verify the destination chain with the SAME signing key (heads must match).
    auto k2 = KeyStore::deserialize(blob);
    REQUIRE(k2);
    auto dst = Runtime::open_uri(pg_uri(), std::move(k2.value()));
    REQUIRE(dst);
    auto v = dst.value().ledger().verify();
    REQUIRE(v);
    CHECK(dst.value().ledger().length() == stats.value().entries_copied);
    CHECK(dst.value().ledger().head().bytes == stats.value().dest_head.bytes);

    std::filesystem::remove(src_path);
    std::filesystem::remove(key_path);
}
