// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
//
// SQLite implementation of LedgerStorage. The default backend; selected
// when Ledger::open() is given a filesystem path or a URI that doesn't
// match a Postgres scheme.

#include "storage.hpp"

#include <sqlite3.h>

#include <cstring>
#include <fmt/core.h>
#include <utility>

namespace asclepius::detail {

namespace {

constexpr const char* kSchema =
    "PRAGMA journal_mode = WAL;"
    "PRAGMA synchronous  = NORMAL;"
    "CREATE TABLE IF NOT EXISTS asclepius_meta ("
    "  key TEXT PRIMARY KEY,"
    "  val BLOB"
    ");"
    "CREATE TABLE IF NOT EXISTS asclepius_ledger ("
    "  seq          INTEGER PRIMARY KEY,"
    "  ts_ns        INTEGER NOT NULL,"
    "  prev_hash    BLOB    NOT NULL,"
    "  payload_hash BLOB    NOT NULL,"
    "  actor        TEXT    NOT NULL,"
    "  event_type   TEXT    NOT NULL,"
    "  tenant       TEXT    NOT NULL DEFAULT '',"
    "  body         TEXT    NOT NULL,"
    "  signature    BLOB    NOT NULL,"
    "  key_id       TEXT    NOT NULL,"
    "  entry_hash   BLOB    NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_ledger_ts ON asclepius_ledger(ts_ns);";

constexpr const char* kSelectCols =
    "SELECT seq, ts_ns, prev_hash, payload_hash, actor, event_type, tenant,"
    "       body, signature, key_id FROM asclepius_ledger ";

LedgerEntry row_to_entry(sqlite3_stmt* st) {
    LedgerEntry e;
    e.header.seq = static_cast<std::uint64_t>(sqlite3_column_int64(st, 0));
    e.header.ts  = Time{sqlite3_column_int64(st, 1)};
    if (const void* p = sqlite3_column_blob(st, 2)) {
        std::memcpy(e.header.prev_hash.bytes.data(), p, Hash::size);
    }
    if (const void* p = sqlite3_column_blob(st, 3)) {
        std::memcpy(e.header.payload_hash.bytes.data(), p, Hash::size);
    }
    e.header.actor      = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    e.header.event_type = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
    e.header.tenant     = reinterpret_cast<const char*>(sqlite3_column_text(st, 6));
    if (const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(st, 7))) {
        e.body_json = p;
    }
    {
        const void* p = sqlite3_column_blob(st, 8);
        int n = sqlite3_column_bytes(st, 8);
        if (p && n == static_cast<int>(KeyStore::sig_bytes)) {
            std::memcpy(e.signature.data(), p, KeyStore::sig_bytes);
        }
    }
    e.key_id = reinterpret_cast<const char*>(sqlite3_column_text(st, 9));
    return e;
}

class SqliteStorage final : public LedgerStorage {
public:
    explicit SqliteStorage(std::string path) : path_(std::move(path)) {}

    ~SqliteStorage() override {
        if (db_) sqlite3_close(db_);
    }

    Result<void> open() {
        if (sqlite3_open(path_.c_str(), &db_) != SQLITE_OK) {
            std::string msg = sqlite3_errmsg(db_);
            return Error::backend("sqlite open: " + msg);
        }
        return Result<void>::ok();
    }

    Result<void> init_schema() override {
        if (sqlite3_exec(db_, kSchema, nullptr, nullptr, nullptr) != SQLITE_OK) {
            return Error::backend(std::string{"sqlite schema: "} + sqlite3_errmsg(db_));
        }
        return Result<void>::ok();
    }

    Result<void> insert_entry(const LedgerEntry& e, const Hash& entry_h) override {
        sqlite3_stmt* stmt = nullptr;
        const char* sql =
            "INSERT INTO asclepius_ledger("
            "  seq, ts_ns, prev_hash, payload_hash, actor, event_type, tenant,"
            "  body, signature, key_id, entry_hash"
            ") VALUES (?,?,?,?,?,?,?,?,?,?,?);";

        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return Error::backend(std::string{"sqlite prepare: "} + sqlite3_errmsg(db_));
        }

        int i = 1;
        sqlite3_bind_int64(stmt, i++, static_cast<sqlite3_int64>(e.header.seq));
        sqlite3_bind_int64(stmt, i++, static_cast<sqlite3_int64>(e.header.ts.nanos_since_epoch()));
        sqlite3_bind_blob (stmt, i++, e.header.prev_hash.bytes.data(),    static_cast<int>(Hash::size), SQLITE_TRANSIENT);
        sqlite3_bind_blob (stmt, i++, e.header.payload_hash.bytes.data(), static_cast<int>(Hash::size), SQLITE_TRANSIENT);
        sqlite3_bind_text (stmt, i++, e.header.actor.c_str(),      -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (stmt, i++, e.header.event_type.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (stmt, i++, e.header.tenant.c_str(),     -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (stmt, i++, e.body_json.c_str(),
                                       static_cast<int>(e.body_json.size()), SQLITE_TRANSIENT);
        sqlite3_bind_blob (stmt, i++, e.signature.data(),
                                       static_cast<int>(e.signature.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text (stmt, i++, e.key_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_blob (stmt, i++, entry_h.bytes.data(),
                                       static_cast<int>(Hash::size), SQLITE_TRANSIENT);

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            std::string msg = sqlite3_errmsg(db_);
            sqlite3_finalize(stmt);
            return Error::backend("sqlite insert: " + msg);
        }
        sqlite3_finalize(stmt);
        return Result<void>::ok();
    }

    Result<std::pair<std::uint64_t, Hash>> read_tail() override {
        sqlite3_stmt* st = nullptr;
        const char* sql =
            "SELECT seq, entry_hash FROM asclepius_ledger ORDER BY seq DESC LIMIT 1;";
        std::pair<std::uint64_t, Hash> result{0, Hash::zero()};
        if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
            return Error::backend(std::string{"sqlite prepare: "} + sqlite3_errmsg(db_));
        }
        if (sqlite3_step(st) == SQLITE_ROW) {
            result.first = static_cast<std::uint64_t>(sqlite3_column_int64(st, 0));
            const void* eh = sqlite3_column_blob(st, 1);
            int   eh_n     = sqlite3_column_bytes(st, 1);
            if (eh && eh_n == static_cast<int>(Hash::size)) {
                std::memcpy(result.second.bytes.data(), eh, Hash::size);
            }
        }
        sqlite3_finalize(st);
        return result;
    }

    Result<LedgerEntry> select_at(std::uint64_t seq) override {
        sqlite3_stmt* st = nullptr;
        std::string sql = std::string{kSelectCols} + "WHERE seq = ? LIMIT 1;";
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
            return Error::backend(std::string{"sqlite prepare: "} + sqlite3_errmsg(db_));
        }
        sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(seq));
        if (sqlite3_step(st) != SQLITE_ROW) {
            sqlite3_finalize(st);
            return Error::not_found(fmt::format("ledger seq {} not found", seq));
        }
        auto e = row_to_entry(st);
        sqlite3_finalize(st);
        return e;
    }

    Result<std::vector<LedgerEntry>> select_tail(std::size_t n) override {
        sqlite3_stmt* st = nullptr;
        std::string sql = std::string{kSelectCols} + "ORDER BY seq DESC LIMIT ?;";
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
            return Error::backend(std::string{"sqlite prepare: "} + sqlite3_errmsg(db_));
        }
        sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(n));
        std::vector<LedgerEntry> out;
        while (sqlite3_step(st) == SQLITE_ROW) out.push_back(row_to_entry(st));
        sqlite3_finalize(st);
        return out;
    }

    Result<std::vector<LedgerEntry>> select_range(std::uint64_t start, std::uint64_t end) override {
        sqlite3_stmt* st = nullptr;
        std::string sql = std::string{kSelectCols} + "WHERE seq >= ? AND seq < ? ORDER BY seq ASC;";
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
            return Error::backend(std::string{"sqlite prepare: "} + sqlite3_errmsg(db_));
        }
        sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(start));
        sqlite3_bind_int64(st, 2, static_cast<sqlite3_int64>(end));
        std::vector<LedgerEntry> out;
        while (sqlite3_step(st) == SQLITE_ROW) out.push_back(row_to_entry(st));
        sqlite3_finalize(st);
        return out;
    }

    Result<std::vector<LedgerEntry>> select_all() override {
        sqlite3_stmt* st = nullptr;
        std::string sql = std::string{kSelectCols} + "ORDER BY seq ASC;";
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
            return Error::backend(std::string{"sqlite prepare: "} + sqlite3_errmsg(db_));
        }
        std::vector<LedgerEntry> out;
        while (sqlite3_step(st) == SQLITE_ROW) out.push_back(row_to_entry(st));
        sqlite3_finalize(st);
        return out;
    }

private:
    std::string  path_;
    sqlite3*     db_ = nullptr;
};

}  // namespace

Result<std::unique_ptr<LedgerStorage>> make_sqlite_storage(const std::string& path) {
    auto s = std::make_unique<SqliteStorage>(path);
    auto r = s->open();
    if (!r) return r.error();
    auto i = s->init_schema();
    if (!i) return i.error();
    return std::unique_ptr<LedgerStorage>{std::move(s)};
}

}  // namespace asclepius::detail
