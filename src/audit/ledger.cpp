// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include "asclepius/audit.hpp"

#include <sqlite3.h>

#include <fmt/core.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <fstream>
#include <mutex>

namespace asclepius {

using nlohmann::json;

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

// Canonical JSON serialization for hashing: sorted keys, no whitespace.
// This makes payload_hash deterministic across processes and platforms.
std::string canonical_json(const json& j) {
    return j.dump(/*indent=*/-1, /*indent_char=*/' ', /*ensure_ascii=*/false,
                  json::error_handler_t::strict);
}

// Compute a payload hash that is stable for a given (event_type, body)
// regardless of insertion order.
Hash compute_payload_hash(std::string_view event_type, const json& body) {
    Hasher h;
    h.update(event_type);
    h.update("\x1e");  // record separator
    h.update(canonical_json(body));
    return h.finalize();
}

// Compute the entry hash that participates in the chain. Includes header
// fields, the body, and the signature so any mutation invalidates the chain.
Hash compute_entry_hash(const LedgerEntryHeader& h,
                        std::string_view         body,
                        std::span<const std::uint8_t> sig,
                        std::string_view         key_id) {
    Hasher hh;
    hh.update(fmt::format("{}|{}|", h.seq, h.ts.nanos_since_epoch()));
    hh.update(Bytes{h.prev_hash.bytes.data(), h.prev_hash.bytes.size()});
    hh.update("|");
    hh.update(Bytes{h.payload_hash.bytes.data(), h.payload_hash.bytes.size()});
    hh.update("|");
    hh.update(h.actor);
    hh.update("|");
    hh.update(h.event_type);
    hh.update("|");
    hh.update(h.tenant);
    hh.update("|");
    hh.update(body);
    hh.update("|");
    hh.update(sig);
    hh.update("|");
    hh.update(key_id);
    return hh.finalize();
}

// Bytes-to-sign for the signature. Note: the signature does NOT cover itself
// (obviously), but covers everything else needed to authenticate the entry.
std::vector<std::uint8_t> bytes_to_sign(const LedgerEntryHeader& h,
                                       std::string_view         body) {
    std::vector<std::uint8_t> out;
    auto append = [&](const void* p, std::size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        out.insert(out.end(), b, b + n);
    };
    auto append_u64 = [&](std::uint64_t x) { append(&x, sizeof(x)); };

    append_u64(h.seq);
    append_u64(static_cast<std::uint64_t>(h.ts.nanos_since_epoch()));
    append(h.prev_hash.bytes.data(),    h.prev_hash.bytes.size());
    append(h.payload_hash.bytes.data(), h.payload_hash.bytes.size());
    append(h.actor.data(),              h.actor.size());           append(",", 1);
    append(h.event_type.data(),         h.event_type.size());      append(",", 1);
    append(h.tenant.data(),             h.tenant.size());          append(",", 1);
    append(body.data(),                 body.size());
    return out;
}

// Sidecar file path for the signing-key blob, with restrictive permissions.
std::filesystem::path key_path_for(const std::filesystem::path& db_path) {
    return std::filesystem::path{db_path}.replace_extension(".key");
}

// Best-effort 0600 permissions on POSIX systems.
void chmod_secret(const std::filesystem::path& p) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::permissions(p,
                    fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, ec);
    if (ec) {
        spdlog::warn("asclepius: could not chmod {}: {}", p.string(), ec.message());
    }
}

}  // namespace

Hash LedgerEntry::entry_hash() const {
    return compute_entry_hash(header,
                              body_json,
                              Bytes{signature.data(), signature.size()},
                              key_id);
}

// ---- Ledger::Impl --------------------------------------------------------

struct Ledger::Impl {
    std::filesystem::path           db_path;
    sqlite3*                        db = nullptr;
    KeyStore                        signer;
    std::array<std::uint8_t, KeyStore::pk_bytes> public_key{};
    std::string                     key_id;

    std::mutex                      mu;
    std::atomic<std::uint64_t>      length{0};
    Hash                            head{};

    Impl(std::filesystem::path p, KeyStore k) : db_path(std::move(p)), signer(std::move(k)) {}
    ~Impl() {
        if (db) sqlite3_close(db);
    }
};

Ledger::Ledger(Ledger&& other) noexcept : impl_(other.impl_) { other.impl_ = nullptr; }
Ledger& Ledger::operator=(Ledger&& other) noexcept {
    if (this != &other) {
        delete impl_;
        impl_       = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}
Ledger::~Ledger() { delete impl_; }

Result<Ledger> Ledger::open(std::filesystem::path path) {
    auto kp = key_path_for(path);
    KeyStore key = [&]() -> KeyStore {
        if (std::filesystem::exists(kp)) {
            std::ifstream in(kp);
            std::string blob((std::istreambuf_iterator<char>(in)), {});
            auto loaded = KeyStore::deserialize(blob);
            if (loaded) return std::move(loaded.value());
            // fall through to fresh keypair on parse failure
        }
        auto fresh = KeyStore::generate();
        std::ofstream out(kp, std::ios::trunc);
        out << fresh.serialize();
        out.close();
        chmod_secret(kp);
        return fresh;
    }();
    return open(std::move(path), std::move(key));
}

Result<Ledger> Ledger::open(std::filesystem::path path, KeyStore key) {
    auto* impl = new Impl{std::move(path), std::move(key)};

    if (sqlite3_open(impl->db_path.string().c_str(), &impl->db) != SQLITE_OK) {
        std::string msg = sqlite3_errmsg(impl->db);
        delete impl;
        return Error::backend("sqlite open: " + msg);
    }

    if (sqlite3_exec(impl->db, kSchema, nullptr, nullptr, nullptr) != SQLITE_OK) {
        std::string msg = sqlite3_errmsg(impl->db);
        delete impl;
        return Error::backend("sqlite schema: " + msg);
    }

    // Cache public key + key id.
    impl->public_key = impl->signer.public_key();
    impl->key_id     = impl->signer.key_id();

    // Bootstrap chain head.
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(impl->db,
                           "SELECT seq, entry_hash FROM asclepius_ledger "
                           "ORDER BY seq DESC LIMIT 1;",
                           -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            impl->length.store(static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0)));
            const void* eh = sqlite3_column_blob(stmt, 1);
            int   eh_n     = sqlite3_column_bytes(stmt, 1);
            if (eh && eh_n == static_cast<int>(Hash::size)) {
                std::memcpy(impl->head.bytes.data(), eh, Hash::size);
            }
        }
        sqlite3_finalize(stmt);
    }

    Ledger out;
    out.impl_ = impl;
    return out;
}

Result<LedgerEntry> Ledger::append(std::string event_type,
                                   std::string actor,
                                   nlohmann::json body,
                                   std::string tenant) {
    std::lock_guard<std::mutex> lk(impl_->mu);

    LedgerEntry e;
    e.header.seq          = impl_->length.load() + 1;
    e.header.ts           = Time::now();
    e.header.prev_hash    = impl_->head;
    e.header.actor        = std::move(actor);
    e.header.event_type   = std::move(event_type);
    e.header.tenant       = std::move(tenant);

    e.body_json           = canonical_json(body);
    e.header.payload_hash = compute_payload_hash(e.header.event_type, body);

    auto sign_buf = bytes_to_sign(e.header, e.body_json);
    e.signature   = impl_->signer.sign(Bytes{sign_buf.data(), sign_buf.size()});
    e.key_id      = impl_->key_id;

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO asclepius_ledger("
        "  seq, ts_ns, prev_hash, payload_hash, actor, event_type, tenant,"
        "  body, signature, key_id, entry_hash"
        ") VALUES (?,?,?,?,?,?,?,?,?,?,?);";

    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return Error::backend(std::string{"sqlite prepare: "} + sqlite3_errmsg(impl_->db));
    }

    Hash entry_h = compute_entry_hash(e.header, e.body_json,
                                      Bytes{e.signature.data(), e.signature.size()},
                                      e.key_id);

    int  i = 1;
    sqlite3_bind_int64(stmt, i++, static_cast<sqlite3_int64>(e.header.seq));
    sqlite3_bind_int64(stmt, i++, static_cast<sqlite3_int64>(e.header.ts.nanos_since_epoch()));
    sqlite3_bind_blob (stmt, i++, e.header.prev_hash.bytes.data(),
                                  static_cast<int>(Hash::size), SQLITE_TRANSIENT);
    sqlite3_bind_blob (stmt, i++, e.header.payload_hash.bytes.data(),
                                  static_cast<int>(Hash::size), SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, i++, e.header.actor.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, i++, e.header.event_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, i++, e.header.tenant.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, i++, e.body_json.c_str(),
                                  static_cast<int>(e.body_json.size()), SQLITE_TRANSIENT);
    sqlite3_bind_blob (stmt, i++, e.signature.data(),
                                  static_cast<int>(e.signature.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, i++, e.key_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob (stmt, i++, entry_h.bytes.data(),
                                  static_cast<int>(Hash::size), SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::string msg = sqlite3_errmsg(impl_->db);
        sqlite3_finalize(stmt);
        return Error::backend("sqlite insert: " + msg);
    }
    sqlite3_finalize(stmt);

    impl_->head = entry_h;
    impl_->length.fetch_add(1);
    return e;
}

namespace {

LedgerEntry row_to_entry(sqlite3_stmt* st) {
    LedgerEntry e;
    e.header.seq        = static_cast<std::uint64_t>(sqlite3_column_int64(st, 0));
    e.header.ts         = Time{sqlite3_column_int64(st, 1)};
    {
        const void* p = sqlite3_column_blob(st, 2);
        if (p) std::memcpy(e.header.prev_hash.bytes.data(), p, Hash::size);
    }
    {
        const void* p = sqlite3_column_blob(st, 3);
        if (p) std::memcpy(e.header.payload_hash.bytes.data(), p, Hash::size);
    }
    e.header.actor      = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    e.header.event_type = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
    e.header.tenant     = reinterpret_cast<const char*>(sqlite3_column_text(st, 6));
    {
        const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(st, 7));
        if (p) e.body_json = p;
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

const char* kSelectCols =
    "SELECT seq, ts_ns, prev_hash, payload_hash, actor, event_type, tenant,"
    "       body, signature, key_id FROM asclepius_ledger ";

}  // namespace

Result<LedgerEntry> Ledger::at(std::uint64_t seq) const {
    sqlite3_stmt* st = nullptr;
    std::string sql  = std::string{kSelectCols} + "WHERE seq = ? LIMIT 1;";
    if (sqlite3_prepare_v2(impl_->db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        return Error::backend(std::string{"sqlite prepare: "} + sqlite3_errmsg(impl_->db));
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

Result<std::vector<LedgerEntry>> Ledger::tail(std::size_t n) const {
    sqlite3_stmt* st = nullptr;
    std::string sql  = std::string{kSelectCols} + "ORDER BY seq DESC LIMIT ?;";
    if (sqlite3_prepare_v2(impl_->db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        return Error::backend(std::string{"sqlite prepare: "} + sqlite3_errmsg(impl_->db));
    }
    sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(n));
    std::vector<LedgerEntry> out;
    while (sqlite3_step(st) == SQLITE_ROW) {
        out.push_back(row_to_entry(st));
    }
    sqlite3_finalize(st);
    return out;
}

Result<std::vector<LedgerEntry>> Ledger::range(std::uint64_t start, std::uint64_t end) const {
    sqlite3_stmt* st = nullptr;
    std::string sql  = std::string{kSelectCols} + "WHERE seq >= ? AND seq < ? ORDER BY seq ASC;";
    if (sqlite3_prepare_v2(impl_->db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        return Error::backend(std::string{"sqlite prepare: "} + sqlite3_errmsg(impl_->db));
    }
    sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(start));
    sqlite3_bind_int64(st, 2, static_cast<sqlite3_int64>(end));
    std::vector<LedgerEntry> out;
    while (sqlite3_step(st) == SQLITE_ROW) {
        out.push_back(row_to_entry(st));
    }
    sqlite3_finalize(st);
    return out;
}

Result<void> Ledger::verify() const {
    sqlite3_stmt* st = nullptr;
    std::string sql  = std::string{kSelectCols} + "ORDER BY seq ASC;";
    if (sqlite3_prepare_v2(impl_->db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        return Error::backend(std::string{"sqlite prepare: "} + sqlite3_errmsg(impl_->db));
    }

    Hash          expected_prev = Hash::zero();
    std::uint64_t expected_seq  = 1;

    while (sqlite3_step(st) == SQLITE_ROW) {
        auto e = row_to_entry(st);

        if (e.header.seq != expected_seq) {
            sqlite3_finalize(st);
            return Error::integrity(fmt::format("ledger gap at seq {} (expected {})",
                                                e.header.seq, expected_seq));
        }
        if (!(e.header.prev_hash == expected_prev)) {
            sqlite3_finalize(st);
            return Error::integrity(fmt::format("chain break at seq {}", e.header.seq));
        }

        // Validate payload_hash.
        json body;
        try { body = json::parse(e.body_json); }
        catch (...) {
            sqlite3_finalize(st);
            return Error::integrity(fmt::format("body parse failure at seq {}", e.header.seq));
        }
        auto recomputed = compute_payload_hash(e.header.event_type, body);
        if (!(recomputed == e.header.payload_hash)) {
            sqlite3_finalize(st);
            return Error::integrity(fmt::format("payload hash mismatch at seq {}", e.header.seq));
        }

        // Validate signature.
        auto sb = bytes_to_sign(e.header, e.body_json);
        if (!KeyStore::verify(Bytes{sb.data(), sb.size()},
                              std::span<const std::uint8_t, KeyStore::sig_bytes>{
                                  e.signature.data(), KeyStore::sig_bytes},
                              std::span<const std::uint8_t, KeyStore::pk_bytes>{
                                  impl_->public_key.data(), KeyStore::pk_bytes})) {
            sqlite3_finalize(st);
            return Error::integrity(fmt::format("bad signature at seq {}", e.header.seq));
        }

        expected_prev = compute_entry_hash(e.header, e.body_json,
                                           Bytes{e.signature.data(), e.signature.size()},
                                           e.key_id);
        ++expected_seq;
    }
    sqlite3_finalize(st);
    return Result<void>::ok();
}

Hash          Ledger::head()       const { return impl_->head; }
std::uint64_t Ledger::length()     const { return impl_->length.load(); }
std::array<std::uint8_t, KeyStore::pk_bytes> Ledger::public_key() const { return impl_->public_key; }
std::string   Ledger::key_id()     const { return impl_->key_id; }

std::array<std::uint8_t, KeyStore::sig_bytes> Ledger::sign_attestation(Bytes message) const {
    return impl_->signer.sign(message);
}

}  // namespace asclepius
