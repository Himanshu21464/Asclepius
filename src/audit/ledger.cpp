// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
//
// Ledger: append-only, Merkle-chained, Ed25519-signed event log. Storage
// is delegated to a LedgerStorage backend (SQLite or PostgreSQL); see
// src/audit/storage.hpp for the interface and src/audit/sqlite_backend.cpp
// + postgres_backend.cpp for the implementations.
//
// This file owns:
//   - canonical-JSON encoding for hash inputs
//   - payload_hash and entry_hash construction
//   - signing-input bytes layout
//   - chain-verify logic (re-hash + re-verify each entry against pubkey)
//   - keypair-on-disk discovery for the SQLite path mode

#include "asclepius/audit.hpp"

#include "storage.hpp"

#include <fmt/core.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>

#if defined(__unix__) || defined(__APPLE__)
#  include <sys/stat.h>
#endif

namespace asclepius {

using nlohmann::json;

namespace {

std::string canonical_json(const json& j) {
    return j.dump(/*indent=*/-1, /*indent_char=*/' ', /*ensure_ascii=*/false,
                  json::error_handler_t::strict);
}

Hash compute_payload_hash(std::string_view event_type, const json& body) {
    Hasher h;
    h.update(event_type);
    h.update("\x1e");  // record separator
    h.update(canonical_json(body));
    return h.finalize();
}

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
    hh.update(h.actor);       hh.update("|");
    hh.update(h.event_type);  hh.update("|");
    hh.update(h.tenant);      hh.update("|");
    hh.update(body);          hh.update("|");
    hh.update(sig);           hh.update("|");
    hh.update(key_id);
    return hh.finalize();
}

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
    append(h.actor.data(),      h.actor.size());
    append(h.event_type.data(), h.event_type.size());
    append(h.tenant.data(),     h.tenant.size());
    append(body.data(),         body.size());
    return out;
}

std::filesystem::path key_path_for(const std::filesystem::path& db_path) {
    auto p = db_path;
    p.replace_extension(".key");
    return p;
}

void chmod_secret([[maybe_unused]] const std::filesystem::path& p) {
#if defined(__unix__) || defined(__APPLE__)
    ::chmod(p.string().c_str(), 0600);
#endif
}

}  // namespace

Hash LedgerEntry::entry_hash() const {
    return compute_entry_hash(header, body_json,
                              Bytes{signature.data(), signature.size()},
                              key_id);
}

// ---- Ledger::Impl --------------------------------------------------------

struct Ledger::Impl {
    std::unique_ptr<detail::LedgerStorage>          storage;
    KeyStore                                        signer;
    std::array<std::uint8_t, KeyStore::pk_bytes>    public_key{};
    std::string                                     key_id;

    std::mutex                                      mu;
    std::atomic<std::uint64_t>                      length{0};
    Hash                                            head{};

    Impl(std::unique_ptr<detail::LedgerStorage> s, KeyStore k)
        : storage(std::move(s)), signer(std::move(k)) {}
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

namespace {

// Look up or generate the keypair for a SQLite path-based ledger. For
// Postgres-backed ledgers callers must pass an explicit KeyStore, since
// there's no obvious filesystem location for the key.
KeyStore key_for_path(const std::filesystem::path& path) {
    auto kp = key_path_for(path);
    if (std::filesystem::exists(kp)) {
        std::ifstream in(kp);
        std::string blob((std::istreambuf_iterator<char>(in)), {});
        auto loaded = KeyStore::deserialize(blob);
        if (loaded) return std::move(loaded.value());
    }
    auto fresh = KeyStore::generate();
    std::ofstream out(kp, std::ios::trunc);
    out << fresh.serialize();
    out.close();
    chmod_secret(kp);
    return fresh;
}

}  // namespace

Result<Ledger> Ledger::open(std::filesystem::path path) {
    return open(path, key_for_path(path));
}

Result<Ledger> Ledger::open(std::filesystem::path path, KeyStore key) {
    return open_uri(path.string(), std::move(key));
}

Result<Ledger> Ledger::open_uri(const std::string& uri) {
    // SQLite paths reuse the SQLite key-on-disk discovery (sibling .key file).
    // Postgres URIs use a default key location: ./<dbname>.key in the current
    // working directory. Production deployments should pass an explicit
    // KeyStore via open_uri(uri, key) — the default is for development only.
    if (uri.compare(0, 11, "postgres://") == 0
     || uri.compare(0, 13, "postgresql://") == 0) {
        // Pull the dbname out of the URI for a stable per-DB keystore path.
        // libpq URIs end with /<dbname>?<params> ; we take the segment between
        // the last '/' and the first '?' (or end of string).
        std::string db = "asclepius";
        auto last_slash = uri.rfind('/');
        if (last_slash != std::string::npos && last_slash + 1 < uri.size()) {
            auto q = uri.find('?', last_slash + 1);
            db = uri.substr(last_slash + 1,
                            (q == std::string::npos ? uri.size() : q) - last_slash - 1);
            if (db.empty()) db = "asclepius";
        }
        std::filesystem::path key_path{db + ".key"};
        return open_uri(uri, key_for_path(key_path));
    }
    return open_uri(uri, key_for_path(std::filesystem::path{uri}));
}

Result<Ledger> Ledger::open_uri(const std::string& uri, KeyStore key) {
    auto s = detail::make_storage(uri);
    if (!s) return s.error();

    auto* impl = new Impl{std::move(s.value()), std::move(key)};
    impl->public_key = impl->signer.public_key();
    impl->key_id     = impl->signer.key_id();

    auto tail = impl->storage->read_tail();
    if (!tail) {
        delete impl;
        return tail.error();
    }
    impl->length.store(tail.value().first);
    impl->head = tail.value().second;

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

    Hash entry_h = compute_entry_hash(e.header, e.body_json,
                                      Bytes{e.signature.data(), e.signature.size()},
                                      e.key_id);

    auto rr = impl_->storage->insert_entry(e, entry_h);
    if (!rr) return rr.error();

    impl_->head = entry_h;
    impl_->length.fetch_add(1);
    return e;
}

Result<LedgerEntry> Ledger::at(std::uint64_t seq) const {
    return impl_->storage->select_at(seq);
}

Result<std::vector<LedgerEntry>> Ledger::tail(std::size_t n) const {
    return impl_->storage->select_tail(n);
}

Result<std::vector<LedgerEntry>> Ledger::range(std::uint64_t start, std::uint64_t end) const {
    return impl_->storage->select_range(start, end);
}

Result<std::vector<LedgerEntry>> Ledger::range_by_time(Time from, Time to) const {
    return impl_->storage->select_time_range(from.nanos_since_epoch(),
                                             to.nanos_since_epoch());
}

Result<void> Ledger::verify() const {
    Hash          expected_prev = Hash::zero();
    std::uint64_t expected_seq  = 1;
    std::optional<Error> visit_err;

    auto r = impl_->storage->for_each([&](const LedgerEntry& e) -> bool {
        if (e.header.seq != expected_seq) {
            visit_err = Error::integrity(fmt::format("ledger gap at seq {} (expected {})",
                                                     e.header.seq, expected_seq));
            return false;
        }
        if (!(e.header.prev_hash == expected_prev)) {
            visit_err = Error::integrity(fmt::format("chain break at seq {}", e.header.seq));
            return false;
        }
        json body;
        try { body = json::parse(e.body_json); }
        catch (...) {
            visit_err = Error::integrity(fmt::format("body parse failure at seq {}", e.header.seq));
            return false;
        }
        auto recomputed = compute_payload_hash(e.header.event_type, body);
        if (!(recomputed == e.header.payload_hash)) {
            visit_err = Error::integrity(fmt::format("payload hash mismatch at seq {}", e.header.seq));
            return false;
        }
        auto sb = bytes_to_sign(e.header, e.body_json);
        if (!KeyStore::verify(Bytes{sb.data(), sb.size()},
                              std::span<const std::uint8_t, KeyStore::sig_bytes>{
                                  e.signature.data(), KeyStore::sig_bytes},
                              std::span<const std::uint8_t, KeyStore::pk_bytes>{
                                  impl_->public_key.data(), KeyStore::pk_bytes})) {
            visit_err = Error::integrity(fmt::format("bad signature at seq {}", e.header.seq));
            return false;
        }
        expected_prev = compute_entry_hash(e.header, e.body_json,
                                           Bytes{e.signature.data(), e.signature.size()},
                                           e.key_id);
        ++expected_seq;
        return true;
    });
    if (!r) return r.error();
    if (visit_err) return visit_err.value();
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
