// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
//
// PostgreSQL implementation of LedgerStorage. Selected when Ledger::open()
// is given a URI starting with "postgres://" or "postgresql://".
//
// Schema mirrors the SQLite version with type adjustments:
//   seq          BIGINT PRIMARY KEY      (matches SQLite INTEGER PK)
//   ts_ns        BIGINT                  (matches sqlite3_int64)
//   *_hash       BYTEA                   (matches BLOB)
//   actor / event_type / tenant TEXT     (same)
//   body         TEXT                    (canonical-JSON, not JSONB —
//                                         we hash bytes, JSONB would
//                                         re-encode)
//   signature    BYTEA                   (64 bytes Ed25519)
//   key_id       TEXT
//   entry_hash   BYTEA
//
// Concurrency: matches SQLite's single-writer model. Multi-writer
// per-ledger needs a separate ADR (see migrations.html#m-002).

#include "storage.hpp"

#include <libpq-fe.h>

#include <cstring>
#include <fmt/core.h>
#include <utility>

// Portable host-to-big-endian / be-to-host for 64-bit integers. libpq's
// binary parameter format requires network-byte-order (big-endian); we
// can't depend on <endian.h> (Linux glibc only) or <libkern/OSByteOrder.h>
// (macOS only). C++20 doesn't have std::byteswap until C++23, so we
// provide our own. Renamed away from htobe64/be64toh to avoid clashing
// with the glibc macros of those names.
namespace {
inline std::uint64_t bswap64(std::uint64_t v) noexcept {
    return ((v & 0x00000000000000FFULL) << 56) |
           ((v & 0x000000000000FF00ULL) << 40) |
           ((v & 0x0000000000FF0000ULL) << 24) |
           ((v & 0x00000000FF000000ULL) << 8)  |
           ((v & 0x000000FF00000000ULL) >> 8)  |
           ((v & 0x0000FF0000000000ULL) >> 24) |
           ((v & 0x00FF000000000000ULL) >> 40) |
           ((v & 0xFF00000000000000ULL) >> 56);
}
inline bool is_little_endian() noexcept {
    constexpr std::uint16_t probe = 0x1;
    return *reinterpret_cast<const std::uint8_t*>(&probe) == 0x1;
}
inline std::uint64_t pg_htobe64(std::uint64_t v) noexcept {
    return is_little_endian() ? bswap64(v) : v;
}
inline std::uint64_t pg_be64toh(std::uint64_t v) noexcept {
    return is_little_endian() ? bswap64(v) : v;
}
}  // namespace

namespace asclepius::detail {

namespace {

constexpr const char* kSchemaSql =
    "CREATE TABLE IF NOT EXISTS asclepius_meta ("
    "  key TEXT PRIMARY KEY,"
    "  val BYTEA"
    ");"
    "CREATE TABLE IF NOT EXISTS asclepius_ledger ("
    "  seq          BIGINT  PRIMARY KEY,"
    "  ts_ns        BIGINT  NOT NULL,"
    "  prev_hash    BYTEA   NOT NULL,"
    "  payload_hash BYTEA   NOT NULL,"
    "  actor        TEXT    NOT NULL,"
    "  event_type   TEXT    NOT NULL,"
    "  tenant       TEXT    NOT NULL DEFAULT '',"
    "  body         TEXT    NOT NULL,"
    "  signature    BYTEA   NOT NULL,"
    "  key_id       TEXT    NOT NULL,"
    "  entry_hash   BYTEA   NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_asc_ledger_ts ON asclepius_ledger(ts_ns);"
    "CREATE INDEX IF NOT EXISTS idx_asc_ledger_tenant_seq ON asclepius_ledger(tenant, seq);";

constexpr const char* kSelectCols =
    "SELECT seq, ts_ns, prev_hash, payload_hash, actor, event_type, tenant,"
    "       body, signature, key_id FROM asclepius_ledger ";

// libpq returns BYTEA in binary format when paramFormats[i] = 1 and
// resultFormat = 1. For text-format BYTEA we'd need PQunescapeBytea. We
// stick to binary throughout.

// Parse a PGresult row at index r into a LedgerEntry. Assumes binary format
// and the column layout in kSelectCols. Big-endian integers per pg wire.
LedgerEntry row_to_entry(PGresult* res, int r) {
    LedgerEntry e;

    auto read_u64 = [&](int c) -> std::uint64_t {
        std::uint64_t be;
        std::memcpy(&be, PQgetvalue(res, r, c), sizeof(be));
        return pg_be64toh(be);
    };
    auto read_bytea = [&](int c, void* dst, std::size_t n) {
        if (PQgetlength(res, r, c) >= static_cast<int>(n)) {
            std::memcpy(dst, PQgetvalue(res, r, c), n);
        }
    };

    e.header.seq = read_u64(0);
    e.header.ts  = Time{static_cast<std::int64_t>(read_u64(1))};
    read_bytea(2, e.header.prev_hash.bytes.data(),    Hash::size);
    read_bytea(3, e.header.payload_hash.bytes.data(), Hash::size);
    e.header.actor      = std::string{PQgetvalue(res, r, 4),
                                      static_cast<std::size_t>(PQgetlength(res, r, 4))};
    e.header.event_type = std::string{PQgetvalue(res, r, 5),
                                      static_cast<std::size_t>(PQgetlength(res, r, 5))};
    e.header.tenant     = std::string{PQgetvalue(res, r, 6),
                                      static_cast<std::size_t>(PQgetlength(res, r, 6))};
    e.body_json         = std::string{PQgetvalue(res, r, 7),
                                      static_cast<std::size_t>(PQgetlength(res, r, 7))};
    read_bytea(8, e.signature.data(), KeyStore::sig_bytes);
    e.key_id            = std::string{PQgetvalue(res, r, 9),
                                      static_cast<std::size_t>(PQgetlength(res, r, 9))};
    return e;
}

class PostgresStorage final : public LedgerStorage {
public:
    explicit PostgresStorage(std::string conninfo) : conninfo_(std::move(conninfo)) {}

    ~PostgresStorage() override {
        if (conn_) PQfinish(conn_);
    }

    Result<void> open() {
        conn_ = PQconnectdb(conninfo_.c_str());
        if (PQstatus(conn_) != CONNECTION_OK) {
            std::string msg = PQerrorMessage(conn_);
            return Error::backend("postgres connect: " + msg);
        }
        return Result<void>::ok();
    }

    Result<void> init_schema() override {
        PGresult* r = PQexec(conn_, kSchemaSql);
        auto status = PQresultStatus(r);
        std::string err = PQresultErrorMessage(r);
        PQclear(r);
        if (status != PGRES_COMMAND_OK) {
            return Error::backend("postgres schema: " + err);
        }
        return Result<void>::ok();
    }

    Result<void> insert_entry(const LedgerEntry& e, const Hash& entry_h) override {
        // Param order matches the INSERT below.
        std::uint64_t seq_be = pg_htobe64(e.header.seq);
        std::int64_t  ts_be_signed;
        {
            auto u = pg_htobe64(static_cast<std::uint64_t>(e.header.ts.nanos_since_epoch()));
            std::memcpy(&ts_be_signed, &u, sizeof(ts_be_signed));
        }

        const char* values[11];
        int         lengths[11];
        int         formats[11];

        values [0] = reinterpret_cast<const char*>(&seq_be);
        lengths[0] = sizeof(seq_be);  formats[0] = 1;

        values [1] = reinterpret_cast<const char*>(&ts_be_signed);
        lengths[1] = sizeof(ts_be_signed); formats[1] = 1;

        values [2] = reinterpret_cast<const char*>(e.header.prev_hash.bytes.data());
        lengths[2] = Hash::size; formats[2] = 1;

        values [3] = reinterpret_cast<const char*>(e.header.payload_hash.bytes.data());
        lengths[3] = Hash::size; formats[3] = 1;

        values [4] = e.header.actor.c_str();      lengths[4] = static_cast<int>(e.header.actor.size());      formats[4] = 0;
        values [5] = e.header.event_type.c_str(); lengths[5] = static_cast<int>(e.header.event_type.size()); formats[5] = 0;
        values [6] = e.header.tenant.c_str();     lengths[6] = static_cast<int>(e.header.tenant.size());     formats[6] = 0;
        values [7] = e.body_json.c_str();         lengths[7] = static_cast<int>(e.body_json.size());         formats[7] = 0;

        values [8] = reinterpret_cast<const char*>(e.signature.data());
        lengths[8] = static_cast<int>(e.signature.size()); formats[8] = 1;

        values [9] = e.key_id.c_str(); lengths[9] = static_cast<int>(e.key_id.size()); formats[9] = 0;

        values [10] = reinterpret_cast<const char*>(entry_h.bytes.data());
        lengths[10] = Hash::size; formats[10] = 1;

        const char* sql =
            "INSERT INTO asclepius_ledger("
            "  seq, ts_ns, prev_hash, payload_hash, actor, event_type, tenant,"
            "  body, signature, key_id, entry_hash"
            ") VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11);";

        PGresult* r = PQexecParams(conn_, sql, 11, /*paramTypes=*/nullptr,
                                   values, lengths, formats, /*resultFormat=*/0);
        auto status = PQresultStatus(r);
        std::string err = PQresultErrorMessage(r);
        PQclear(r);
        if (status != PGRES_COMMAND_OK) {
            return Error::backend("postgres insert: " + err);
        }
        return Result<void>::ok();
    }

    Result<std::pair<std::uint64_t, Hash>> read_tail() override {
        const char* sql =
            "SELECT seq, entry_hash FROM asclepius_ledger ORDER BY seq DESC LIMIT 1;";
        PGresult* r = PQexecParams(conn_, sql, 0, nullptr, nullptr, nullptr, nullptr, /*binary=*/1);
        if (PQresultStatus(r) != PGRES_TUPLES_OK) {
            std::string err = PQresultErrorMessage(r);
            PQclear(r);
            return Error::backend("postgres read_tail: " + err);
        }
        std::pair<std::uint64_t, Hash> result{0, Hash::zero()};
        if (PQntuples(r) > 0) {
            std::uint64_t be;
            std::memcpy(&be, PQgetvalue(r, 0, 0), sizeof(be));
            result.first = pg_be64toh(be);
            int n = PQgetlength(r, 0, 1);
            if (n == static_cast<int>(Hash::size)) {
                std::memcpy(result.second.bytes.data(), PQgetvalue(r, 0, 1), Hash::size);
            }
        }
        PQclear(r);
        return result;
    }

    Result<LedgerEntry> select_at(std::uint64_t seq) override {
        std::uint64_t seq_be = pg_htobe64(seq);
        const char* values[1] = { reinterpret_cast<const char*>(&seq_be) };
        int lengths[1] = { sizeof(seq_be) };
        int formats[1] = { 1 };
        std::string sql = std::string{kSelectCols} + "WHERE seq = $1 LIMIT 1;";
        PGresult* r = PQexecParams(conn_, sql.c_str(), 1, nullptr,
                                   values, lengths, formats, /*binary=*/1);
        if (PQresultStatus(r) != PGRES_TUPLES_OK) {
            std::string err = PQresultErrorMessage(r);
            PQclear(r);
            return Error::backend("postgres select_at: " + err);
        }
        if (PQntuples(r) == 0) {
            PQclear(r);
            return Error::not_found(fmt::format("ledger seq {} not found", seq));
        }
        auto e = row_to_entry(r, 0);
        PQclear(r);
        return e;
    }

    Result<std::vector<LedgerEntry>> select_tail(std::size_t n) override {
        std::int64_t lim_be;
        {
            auto u = pg_htobe64(static_cast<std::uint64_t>(n));
            std::memcpy(&lim_be, &u, sizeof(lim_be));
        }
        const char* values[1] = { reinterpret_cast<const char*>(&lim_be) };
        int lengths[1] = { sizeof(lim_be) };
        int formats[1] = { 1 };
        std::string sql = std::string{kSelectCols} + "ORDER BY seq DESC LIMIT $1;";
        return run_select(sql, 1, values, lengths, formats);
    }

    Result<std::vector<LedgerEntry>> select_range(std::uint64_t start, std::uint64_t end) override {
        std::uint64_t s_be = pg_htobe64(start);
        std::uint64_t e_be = pg_htobe64(end);
        const char* values[2] = {
            reinterpret_cast<const char*>(&s_be),
            reinterpret_cast<const char*>(&e_be),
        };
        int lengths[2] = { sizeof(s_be), sizeof(e_be) };
        int formats[2] = { 1, 1 };
        std::string sql = std::string{kSelectCols} +
            "WHERE seq >= $1 AND seq < $2 ORDER BY seq ASC;";
        return run_select(sql, 2, values, lengths, formats);
    }

    Result<std::vector<LedgerEntry>> select_all() override {
        std::string sql = std::string{kSelectCols} + "ORDER BY seq ASC;";
        return run_select(sql, 0, nullptr, nullptr, nullptr);
    }

    Result<std::vector<LedgerEntry>> select_time_range(std::int64_t from_ns,
                                                       std::int64_t to_ns) override {
        std::int64_t f_be, t_be;
        {
            auto u = pg_htobe64(static_cast<std::uint64_t>(from_ns));
            std::memcpy(&f_be, &u, sizeof(f_be));
            u = pg_htobe64(static_cast<std::uint64_t>(to_ns));
            std::memcpy(&t_be, &u, sizeof(t_be));
        }
        const char* values[2] = {
            reinterpret_cast<const char*>(&f_be),
            reinterpret_cast<const char*>(&t_be),
        };
        int lengths[2] = { sizeof(f_be), sizeof(t_be) };
        int formats[2] = { 1, 1 };
        std::string sql = std::string{kSelectCols}
                        + "WHERE ts_ns >= $1 AND ts_ns < $2 ORDER BY seq ASC;";
        return run_select(sql, 2, values, lengths, formats);
    }

    Result<std::vector<LedgerEntry>> select_tail_for_tenant(const std::string& tenant,
                                                            std::size_t n) override {
        std::int64_t lim_be;
        {
            auto u = pg_htobe64(static_cast<std::uint64_t>(n));
            std::memcpy(&lim_be, &u, sizeof(lim_be));
        }
        const char* values[2] = {
            tenant.c_str(),
            reinterpret_cast<const char*>(&lim_be),
        };
        int lengths[2] = { static_cast<int>(tenant.size()), sizeof(lim_be) };
        int formats[2] = { 0, 1 };
        std::string sql = std::string{kSelectCols}
                        + "WHERE tenant = $1 ORDER BY seq DESC LIMIT $2;";
        return run_select(sql, 2, values, lengths, formats);
    }

    Result<std::vector<LedgerEntry>> select_range_for_tenant(const std::string& tenant,
                                                             std::uint64_t start,
                                                             std::uint64_t end) override {
        std::uint64_t s_be = pg_htobe64(start);
        std::uint64_t e_be = pg_htobe64(end);
        const char* values[3] = {
            tenant.c_str(),
            reinterpret_cast<const char*>(&s_be),
            reinterpret_cast<const char*>(&e_be),
        };
        int lengths[3] = { static_cast<int>(tenant.size()), sizeof(s_be), sizeof(e_be) };
        int formats[3] = { 0, 1, 1 };
        std::string sql = std::string{kSelectCols}
                        + "WHERE tenant = $1 AND seq >= $2 AND seq < $3 ORDER BY seq ASC;";
        return run_select(sql, 3, values, lengths, formats);
    }

    Result<void> for_each(std::function<bool(const LedgerEntry&)> visitor) override {
        // libpq single-row mode: PQsendQueryParams + PQsetSingleRowMode lets
        // the server stream rows without materializing the whole result set
        // in libpq's memory. Each PQgetResult call returns ONE row at a
        // time; the caller iterates until PQgetResult returns nullptr.
        std::string sql = std::string{kSelectCols} + "ORDER BY seq ASC;";
        if (!PQsendQueryParams(conn_, sql.c_str(), 0, nullptr,
                               nullptr, nullptr, nullptr, /*binary=*/1)) {
            return Error::backend(std::string{"postgres send: "} + PQerrorMessage(conn_));
        }
        if (!PQsetSingleRowMode(conn_)) {
            // Drain whatever's pending so the connection stays usable.
            while (PGresult* drain = PQgetResult(conn_)) PQclear(drain);
            return Error::backend("postgres single-row mode rejected");
        }

        bool stopped = false;
        while (PGresult* r = PQgetResult(conn_)) {
            auto status = PQresultStatus(r);
            if (status == PGRES_SINGLE_TUPLE) {
                if (!stopped) {
                    auto e = row_to_entry(r, 0);
                    if (!visitor(e)) stopped = true;
                }
                PQclear(r);
            } else if (status == PGRES_TUPLES_OK) {
                // End-of-stream marker. No actual rows in this result.
                PQclear(r);
            } else {
                std::string err = PQresultErrorMessage(r);
                PQclear(r);
                // Drain to keep connection clean.
                while (PGresult* drain = PQgetResult(conn_)) PQclear(drain);
                return Error::backend("postgres for_each: " + err);
            }
        }
        return Result<void>::ok();
    }

private:
    Result<std::vector<LedgerEntry>> run_select(const std::string& sql,
                                                int nparams,
                                                const char* const* values,
                                                const int* lengths,
                                                const int* formats) {
        PGresult* r = PQexecParams(conn_, sql.c_str(), nparams, nullptr,
                                   values, lengths, formats, /*binary=*/1);
        if (PQresultStatus(r) != PGRES_TUPLES_OK) {
            std::string err = PQresultErrorMessage(r);
            PQclear(r);
            return Error::backend("postgres select: " + err);
        }
        std::vector<LedgerEntry> out;
        out.reserve(PQntuples(r));
        for (int i = 0; i < PQntuples(r); ++i) out.push_back(row_to_entry(r, i));
        PQclear(r);
        return out;
    }

    std::string conninfo_;
    PGconn*     conn_ = nullptr;
};

}  // namespace

Result<std::unique_ptr<LedgerStorage>> make_postgres_storage(const std::string& uri) {
    // libpq accepts both "postgres://" and "postgresql://" URIs directly.
    auto s = std::make_unique<PostgresStorage>(uri);
    auto r = s->open();
    if (!r) return r.error();
    auto i = s->init_schema();
    if (!i) return i.error();
    return std::unique_ptr<LedgerStorage>{std::move(s)};
}

}  // namespace asclepius::detail
