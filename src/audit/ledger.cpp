// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
//
// Ledger: append-only, Merkle-chained, Ed25519-signed event log. Storage
// is delegated to a SQLite-backed LedgerStorage; see src/audit/storage.hpp
// for the interface and src/audit/sqlite_backend.cpp for the implementation.
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

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>

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

    // Subscriptions. Held under sub_mu (separate from append mu so a
    // misbehaving subscriber can't deadlock the append path).
    std::mutex                                      sub_mu;
    std::uint64_t                                   next_sub_id = 1;
    std::vector<std::pair<std::uint64_t, Ledger::Subscriber>> subs;

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

// Look up or generate the keypair beside the SQLite ledger file. The key
// is stored at <db>.key with 0600 permissions.
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
    auto s = detail::make_sqlite_storage(path.string());
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

    // Snapshot the subscriber list under sub_mu, then call without
    // holding append mu so subscribers can do anything except call
    // back into append on this thread.
    std::vector<Subscriber> snap;
    {
        std::lock_guard<std::mutex> lk2(impl_->sub_mu);
        snap.reserve(impl_->subs.size());
        for (const auto& [_, cb] : impl_->subs) snap.push_back(cb);
    }
    for (const auto& cb : snap) {
        try { cb(e); } catch (...) { /* subscriber failures must not break the chain */ }
    }
    return e;
}

Result<std::vector<LedgerEntry>> Ledger::append_batch(std::vector<AppendSpec> specs) {
    // Empty input is a valid no-op.
    if (specs.empty()) return std::vector<LedgerEntry>{};

    std::lock_guard<std::mutex> lk(impl_->mu);

    auto txn = impl_->storage->begin_transaction();
    if (!txn) return txn.error();

    // Build & insert each entry; track the running prev/seq locally so
    // intermediate rollbacks don't corrupt the in-memory state.
    std::vector<LedgerEntry> emitted;
    emitted.reserve(specs.size());
    Hash          working_prev = impl_->head;
    std::uint64_t working_seq  = impl_->length.load();

    for (auto& spec : specs) {
        LedgerEntry e;
        e.header.seq          = ++working_seq;
        e.header.ts           = Time::now();
        e.header.prev_hash    = working_prev;
        e.header.actor        = std::move(spec.actor);
        e.header.event_type   = std::move(spec.event_type);
        e.header.tenant       = std::move(spec.tenant);
        e.body_json           = canonical_json(spec.body);
        e.header.payload_hash = compute_payload_hash(e.header.event_type, spec.body);

        auto sign_buf = bytes_to_sign(e.header, e.body_json);
        e.signature   = impl_->signer.sign(Bytes{sign_buf.data(), sign_buf.size()});
        e.key_id      = impl_->key_id;

        Hash entry_h = compute_entry_hash(e.header, e.body_json,
                                          Bytes{e.signature.data(), e.signature.size()},
                                          e.key_id);

        auto rr = impl_->storage->insert_entry(e, entry_h);
        if (!rr) {
            // Roll back; nothing in this batch is observable.
            (void)impl_->storage->rollback_transaction();
            return rr.error();
        }
        working_prev = entry_h;
        emitted.push_back(std::move(e));
    }

    auto cm = impl_->storage->commit_transaction();
    if (!cm) {
        (void)impl_->storage->rollback_transaction();
        return cm.error();
    }

    // Promote the working state on success.
    impl_->head = working_prev;
    impl_->length.store(working_seq);

    // Snapshot subscribers under sub_mu, fire without it. Order: same
    // as registration; events: in seq-ascending order across the batch.
    std::vector<Subscriber> snap;
    {
        std::lock_guard<std::mutex> lk2(impl_->sub_mu);
        snap.reserve(impl_->subs.size());
        for (const auto& [_, cb] : impl_->subs) snap.push_back(cb);
    }
    for (const auto& cb : snap) {
        for (const auto& entry : emitted) {
            try { cb(entry); } catch (...) { /* swallow */ }
        }
    }

    return emitted;
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

Result<std::vector<LedgerEntry>> Ledger::tail_for_tenant(const std::string& tenant,
                                                         std::size_t n) const {
    return impl_->storage->select_tail_for_tenant(tenant, n);
}

Result<std::vector<LedgerEntry>> Ledger::range_for_tenant(const std::string& tenant,
                                                          std::uint64_t start,
                                                          std::uint64_t end) const {
    return impl_->storage->select_range_for_tenant(tenant, start, end);
}

Result<void> Ledger::verify_parallel(unsigned threads) const {
    // Strategy: collect entries into a vector via for_each (so the
    // streaming interface still drives the read), parallelise the per-
    // entry self-verification (signature + payload_hash + body parse —
    // the expensive ~140µs per entry), then walk the chain sequentially
    // in this thread to check prev_hash continuity and seq gap-freeness.
    //
    // For small chains the parallel split is pure overhead; fall through
    // to plain verify().
    constexpr std::size_t kMinForParallel = 512;

    if (threads == 0) {
        threads = std::thread::hardware_concurrency();
        if (threads == 0) threads = 2;
    }
    if (threads == 1) return verify();

    // Drain the chain into memory.
    std::vector<LedgerEntry> all;
    {
        std::optional<Error> err;
        auto r = impl_->storage->for_each([&](const LedgerEntry& e) -> bool {
            all.push_back(e);
            return true;
        });
        if (!r) return r.error();
        if (err) return err.value();
    }
    if (all.size() < kMinForParallel) return verify();

    // Per-entry self-verification (signature + payload_hash). Parallelised
    // with N workers each owning a contiguous slice.
    std::atomic<bool> any_failed{false};
    std::mutex        err_mu;
    std::optional<Error> first_error;

    auto worker = [&](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
            if (any_failed.load(std::memory_order_relaxed)) return;
            const auto& e = all[i];
            json body;
            try { body = json::parse(e.body_json); }
            catch (...) {
                std::lock_guard<std::mutex> lk(err_mu);
                if (!first_error) {
                    first_error = Error::integrity(fmt::format(
                        "body parse failure at seq {}", e.header.seq));
                }
                any_failed.store(true);
                return;
            }
            auto recomputed = compute_payload_hash(e.header.event_type, body);
            if (!(recomputed == e.header.payload_hash)) {
                std::lock_guard<std::mutex> lk(err_mu);
                if (!first_error) {
                    first_error = Error::integrity(fmt::format(
                        "payload hash mismatch at seq {}", e.header.seq));
                }
                any_failed.store(true);
                return;
            }
            auto sb = bytes_to_sign(e.header, e.body_json);
            if (!KeyStore::verify(
                    Bytes{sb.data(), sb.size()},
                    std::span<const std::uint8_t, KeyStore::sig_bytes>{
                        e.signature.data(), KeyStore::sig_bytes},
                    std::span<const std::uint8_t, KeyStore::pk_bytes>{
                        impl_->public_key.data(), KeyStore::pk_bytes})) {
                std::lock_guard<std::mutex> lk(err_mu);
                if (!first_error) {
                    first_error = Error::integrity(fmt::format(
                        "bad signature at seq {}", e.header.seq));
                }
                any_failed.store(true);
                return;
            }
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(threads);
    std::size_t per = (all.size() + threads - 1) / threads;
    for (unsigned t = 0; t < threads; ++t) {
        std::size_t lo = t * per;
        if (lo >= all.size()) break;
        std::size_t hi = std::min(lo + per, all.size());
        pool.emplace_back(worker, lo, hi);
    }
    for (auto& th : pool) th.join();
    if (first_error) return first_error.value();

    // Chain-consistency walk (sequential, fast).
    Hash          expected_prev = Hash::zero();
    std::uint64_t expected_seq  = 1;
    for (const auto& e : all) {
        if (e.header.seq != expected_seq) {
            return Error::integrity(fmt::format(
                "ledger gap at seq {} (expected {})",
                e.header.seq, expected_seq));
        }
        if (!(e.header.prev_hash == expected_prev)) {
            return Error::integrity(fmt::format(
                "chain break at seq {}", e.header.seq));
        }
        expected_prev = compute_entry_hash(
            e.header, e.body_json,
            Bytes{e.signature.data(), e.signature.size()},
            e.key_id);
        ++expected_seq;
    }
    return Result<void>::ok();
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

// ---- Checkpoint helpers -------------------------------------------------

namespace {

// Canonical signing input: (seq, head_hash_hex, ts_ns, key_id) packed as
// JSON with sorted keys, no whitespace. Bit-equal across hosts.
std::string checkpoint_sign_input(std::uint64_t seq,
                                  const Hash&   head,
                                  Time          ts,
                                  std::string_view key_id) {
    json j;
    j["head_hash"] = head.hex();
    j["key_id"]    = std::string{key_id};
    j["seq"]       = seq;
    j["ts_ns"]     = ts.nanos_since_epoch();
    return j.dump(/*indent=*/-1, /*indent_char=*/' ', /*ensure_ascii=*/false,
                  json::error_handler_t::strict);
}

std::string b64_encode(std::span<const std::uint8_t> in) {
    static const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < in.size(); i += 3) {
        std::uint32_t v = std::uint32_t(in[i]) << 16;
        if (i + 1 < in.size()) v |= std::uint32_t(in[i + 1]) << 8;
        if (i + 2 < in.size()) v |= std::uint32_t(in[i + 2]);
        out.push_back(a[(v >> 18) & 0x3F]);
        out.push_back(a[(v >> 12) & 0x3F]);
        out.push_back(i + 1 < in.size() ? a[(v >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < in.size() ? a[(v >> 0) & 0x3F] : '=');
    }
    return out;
}

bool b64_decode(std::string_view in, std::vector<std::uint8_t>& out) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    out.clear();
    if (in.size() % 4 != 0) return false;
    out.reserve(in.size() / 4 * 3);
    for (std::size_t i = 0; i < in.size(); i += 4) {
        int a0 = val(in[i]),     a1 = val(in[i + 1]);
        int a2 = in[i + 2] == '=' ? 0 : val(in[i + 2]);
        int a3 = in[i + 3] == '=' ? 0 : val(in[i + 3]);
        if (a0 < 0 || a1 < 0 || a2 < 0 || a3 < 0) return false;
        std::uint32_t v = std::uint32_t(a0) << 18
                        | std::uint32_t(a1) << 12
                        | std::uint32_t(a2) << 6
                        | std::uint32_t(a3);
        out.push_back(static_cast<std::uint8_t>(v >> 16));
        if (in[i + 2] != '=') out.push_back(static_cast<std::uint8_t>(v >> 8));
        if (in[i + 3] != '=') out.push_back(static_cast<std::uint8_t>(v));
    }
    return true;
}

}  // namespace

std::string LedgerCheckpoint::to_json() const {
    json j;
    j["seq"]        = seq;
    j["head_hash"]  = head_hash.hex();
    j["ts_ns"]      = ts.nanos_since_epoch();
    j["key_id"]     = key_id;
    j["signature"]  = b64_encode({signature.data(), signature.size()});
    j["public_key"] = b64_encode({public_key.data(), public_key.size()});
    return j.dump();
}

Result<LedgerCheckpoint> LedgerCheckpoint::from_json(std::string_view s) {
    LedgerCheckpoint cp;
    try {
        auto j = json::parse(s);
        cp.seq    = j.at("seq").get<std::uint64_t>();
        std::string head_hex = j.at("head_hash").get<std::string>();
        if (head_hex.size() != Hash::size * 2) return Error::invalid("head_hash hex size");
        // hex -> bytes
        auto valc = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        for (std::size_t i = 0; i < Hash::size; ++i) {
            int hi = valc(head_hex[2 * i]);
            int lo = valc(head_hex[2 * i + 1]);
            if (hi < 0 || lo < 0) return Error::invalid("head_hash hex");
            cp.head_hash.bytes[i] = static_cast<std::uint8_t>((hi << 4) | lo);
        }
        cp.ts     = Time{j.at("ts_ns").get<std::int64_t>()};
        cp.key_id = j.at("key_id").get<std::string>();
        std::vector<std::uint8_t> sig, pk;
        if (!b64_decode(j.at("signature").get<std::string>(), sig)
         || sig.size() != KeyStore::sig_bytes)
            return Error::invalid("signature b64 size");
        if (!b64_decode(j.at("public_key").get<std::string>(), pk)
         || pk.size() != KeyStore::pk_bytes)
            return Error::invalid("public_key b64 size");
        std::memcpy(cp.signature.data(), sig.data(), KeyStore::sig_bytes);
        std::memcpy(cp.public_key.data(), pk.data(), KeyStore::pk_bytes);
    } catch (const std::exception& ex) {
        return Error::invalid(std::string{"checkpoint json: "} + ex.what());
    }
    return cp;
}

Result<Ledger::Stats> Ledger::stats() const {
    Stats s{};
    s.entry_count = impl_->length.load();
    s.head_hash   = impl_->head;
    s.key_id      = impl_->key_id;

    if (s.entry_count == 0) {
        return s;
    }

    bool first = true;
    auto r = impl_->storage->for_each([&](const LedgerEntry& e) -> bool {
        if (first) {
            s.oldest_seq = e.header.seq;
            s.oldest_ts  = e.header.ts;
            first = false;
        }
        s.newest_seq         = e.header.seq;
        s.newest_ts          = e.header.ts;
        s.total_body_bytes  += e.body_json.size();
        return true;
    });
    if (!r) return r.error();

    s.avg_body_bytes = s.entry_count == 0
        ? 0
        : s.total_body_bytes / s.entry_count;
    return s;
}

std::string Ledger::Stats::to_json() const {
    nlohmann::json j;
    j["entry_count"]      = entry_count;
    j["head_hash"]        = head_hash.hex();
    j["oldest_seq"]       = oldest_seq;
    j["newest_seq"]       = newest_seq;
    j["oldest_ts"]        = oldest_ts.iso8601();
    j["newest_ts"]        = newest_ts.iso8601();
    j["total_body_bytes"] = total_body_bytes;
    j["avg_body_bytes"]   = avg_body_bytes;
    j["key_id"]           = key_id;
    return j.dump();
}

Result<Ledger::Stats> Ledger::stats_for_tenant(const std::string& tenant) const {
    Stats s{};
    s.head_hash = impl_->head;
    s.key_id    = impl_->key_id;

    const auto chain_len = impl_->length.load();
    if (chain_len == 0) return s;

    constexpr std::uint64_t kChunk = 1024;
    bool first = true;
    for (std::uint64_t start = 1; start <= chain_len; start += kChunk) {
        std::uint64_t end = std::min(chain_len + 1, start + kChunk);
        auto rng = impl_->storage->select_range_for_tenant(tenant, start, end);
        if (!rng) return rng.error();
        for (const auto& e : rng.value()) {
            if (first) {
                s.oldest_seq = e.header.seq;
                s.oldest_ts  = e.header.ts;
                first = false;
            }
            s.newest_seq        = e.header.seq;
            s.newest_ts         = e.header.ts;
            s.total_body_bytes += e.body_json.size();
            s.entry_count      += 1;
        }
    }
    s.avg_body_bytes = s.entry_count == 0
        ? 0
        : s.total_body_bytes / s.entry_count;
    return s;
}

Result<std::unordered_map<std::string, std::uint64_t>>
Ledger::count_by_event_type() const {
    std::unordered_map<std::string, std::uint64_t> out;
    auto r = impl_->storage->for_each([&](const LedgerEntry& e) -> bool {
        out[e.header.event_type] += 1;
        return true;
    });
    if (!r) return r.error();
    return out;
}

Result<std::vector<std::string>> Ledger::distinct_event_types() const {
    std::unordered_map<std::string, std::uint8_t> seen;
    auto r = impl_->storage->for_each([&](const LedgerEntry& e) -> bool {
        seen.emplace(e.header.event_type, 1);
        return true;
    });
    if (!r) return r.error();
    std::vector<std::string> out;
    out.reserve(seen.size());
    for (const auto& [t, _] : seen) out.push_back(t);
    std::sort(out.begin(), out.end());
    return out;
}

Result<std::vector<LedgerEntry>>
Ledger::tail_by_event_type(std::string_view event_type, std::size_t n) const {
    if (event_type.empty()) {
        return Error::invalid("tail_by_event_type requires non-empty event_type");
    }
    if (n == 0) return std::vector<LedgerEntry>{};
    std::vector<LedgerEntry> ring;
    ring.reserve(n);
    auto r = impl_->storage->for_each([&](const LedgerEntry& e) -> bool {
        if (e.header.event_type != event_type) return true;
        if (ring.size() < n) {
            ring.push_back(e);
        } else {
            std::move(ring.begin() + 1, ring.end(), ring.begin());
            ring.back() = e;
        }
        return true;
    });
    if (!r) return r.error();
    std::reverse(ring.begin(), ring.end());
    return ring;
}

Result<Hash> Ledger::checksum_range(std::uint64_t start, std::uint64_t end) const {
    if (start > end) {
        return Error::invalid("checksum_range: start > end");
    }
    const auto len = impl_->length.load();
    if (end > len + 1) {
        return Error::invalid("checksum_range: end exceeds chain length");
    }
    if (start == end || len == 0) return Hash::zero();
    if (start == 0) {
        return Error::invalid("checksum_range: start must be >= 1");
    }
    auto rng = impl_->storage->select_range(start, end);
    if (!rng) return rng.error();
    Hasher h;
    for (const auto& e : rng.value()) {
        auto eh = e.entry_hash();
        h.update(Bytes{eh.bytes.data(), eh.bytes.size()});
    }
    return h.finalize();
}

Result<void> Ledger::verify_range(std::uint64_t start, std::uint64_t end) const {
    if (start == 0 || start >= end) {
        return Error::invalid("verify_range: requires 0 < start < end");
    }
    const auto len = impl_->length.load();
    if (end > len + 1) {
        return Error::invalid("verify_range: end exceeds chain length");
    }
    auto rng = impl_->storage->select_range(start, end);
    if (!rng) return rng.error();

    Hash expected_prev{};  // zero for start==1
    if (start > 1) {
        auto anchor = impl_->storage->select_at(start - 1);
        if (!anchor) return anchor.error();
        expected_prev = anchor.value().entry_hash();
    }

    std::uint64_t expected_seq = start;
    for (const auto& e : rng.value()) {
        if (e.header.seq != expected_seq) {
            return Error::integrity("ledger gap at seq " +
                                    std::to_string(e.header.seq));
        }
        if (!(e.header.prev_hash == expected_prev)) {
            return Error::integrity("chain break at seq " +
                                    std::to_string(e.header.seq));
        }
        nlohmann::json body;
        try { body = nlohmann::json::parse(e.body_json); }
        catch (...) {
            return Error::integrity("body parse failure at seq " +
                                    std::to_string(e.header.seq));
        }
        auto recomputed = compute_payload_hash(e.header.event_type, body);
        if (!(recomputed == e.header.payload_hash)) {
            return Error::integrity("payload hash mismatch at seq " +
                                    std::to_string(e.header.seq));
        }
        auto sb = bytes_to_sign(e.header, e.body_json);
        if (!KeyStore::verify(Bytes{sb.data(), sb.size()},
                              std::span<const std::uint8_t, KeyStore::sig_bytes>{
                                  e.signature.data(), KeyStore::sig_bytes},
                              std::span<const std::uint8_t, KeyStore::pk_bytes>{
                                  impl_->public_key.data(), KeyStore::pk_bytes})) {
            return Error::integrity("bad signature at seq " +
                                    std::to_string(e.header.seq));
        }
        expected_prev = e.entry_hash();
        ++expected_seq;
    }
    return Result<void>::ok();
}

Result<std::vector<std::string>> Ledger::tenants() const {
    // Collect into a small set (most ledgers have a handful of tenants);
    // the unordered_set keeps the scan O(n) and the final sort O(k log k)
    // where k = distinct tenants << n.
    std::unordered_map<std::string, std::uint8_t> seen;
    auto r = impl_->storage->for_each([&](const LedgerEntry& e) -> bool {
        seen.emplace(e.header.tenant, 1);
        return true;
    });
    if (!r) return r.error();
    std::vector<std::string> out;
    out.reserve(seen.size());
    for (const auto& [t, _] : seen) out.push_back(t);
    std::sort(out.begin(), out.end());
    return out;
}

Result<std::vector<std::string>> Ledger::actors() const {
    std::unordered_map<std::string, std::uint8_t> seen;
    auto r = impl_->storage->for_each([&](const LedgerEntry& e) -> bool {
        seen.emplace(e.header.actor, 1);
        return true;
    });
    if (!r) return r.error();
    std::vector<std::string> out;
    out.reserve(seen.size());
    for (const auto& [a, _] : seen) out.push_back(a);
    std::sort(out.begin(), out.end());
    return out;
}

Result<std::vector<LedgerEntry>>
Ledger::range_by_model(std::string_view model_id) const {
    if (model_id.empty()) {
        return Error::invalid("range_by_model requires non-empty model_id");
    }
    std::vector<LedgerEntry> out;
    auto r = impl_->storage->for_each([&](const LedgerEntry& e) -> bool {
        // Only inference-committed events carry the "model" body field.
        // Cheap prefilter on the raw body string before parsing.
        if (e.header.event_type != "inference.committed") return true;
        if (e.body_json.find(model_id) == std::string::npos) return true;
        try {
            auto j = nlohmann::json::parse(e.body_json);
            auto it = j.find("model");
            if (it != j.end() && it->is_string() &&
                it->get<std::string>() == model_id) {
                out.push_back(e);
            }
        } catch (...) {}
        return true;
    });
    if (!r) return r.error();
    return out;
}

Result<std::vector<LedgerEntry>>
Ledger::oldest_n_for_tenant(const std::string& tenant, std::size_t n) const {
    if (n == 0) return std::vector<LedgerEntry>{};

    const auto chain_len = impl_->length.load();
    if (chain_len == 0) return std::vector<LedgerEntry>{};

    constexpr std::uint64_t kChunk = 1024;
    std::vector<LedgerEntry> out;
    out.reserve(std::min<std::size_t>(n, 1024));
    for (std::uint64_t start = 1; start <= chain_len && out.size() < n;
         start += kChunk) {
        std::uint64_t end = std::min(chain_len + 1, start + kChunk);
        auto rng = impl_->storage->select_range_for_tenant(tenant, start, end);
        if (!rng) return rng.error();
        for (const auto& e : rng.value()) {
            out.push_back(e);
            if (out.size() >= n) break;
        }
    }
    return out;
}

Result<std::vector<LedgerEntry>>
Ledger::events_between(Time from, Time to, std::string_view event_type) const {
    if (event_type.empty()) {
        return Error::invalid("events_between requires non-empty event_type");
    }
    if (from > to) {
        return Error::invalid("events_between: from > to");
    }
    std::vector<LedgerEntry> out;
    auto r = impl_->storage->for_each([&](const LedgerEntry& e) -> bool {
        // Half-open [from, to): include from, exclude to.
        if (e.header.ts < from) return true;
        if (!(e.header.ts < to)) return true;
        if (e.header.event_type == event_type) out.push_back(e);
        return true;
    });
    if (!r) return r.error();
    return out;
}

bool Ledger::has_inference_id(std::string_view id) const {
    if (id.empty()) return false;
    bool found = false;
    auto r = impl_->storage->for_each([&](const LedgerEntry& e) -> bool {
        // Cheap prefilter: skip entries whose body can't contain the id.
        if (e.body_json.find(id) == std::string::npos) return true;
        try {
            auto j = nlohmann::json::parse(e.body_json);
            auto it = j.find("inference_id");
            if (it != j.end() && it->is_string() &&
                it->get<std::string>() == id) {
                found = true;
                return false;  // stop scanning on first match
            }
        } catch (...) {
            // body is not JSON — ignore; defensive against future event types.
        }
        return true;
    });
    (void)r;  // best-effort probe; backend errors are observable via verify()
    return found;
}

std::string Ledger::attestation_json() const {
    json j;
    j["length"]      = impl_->length.load();
    j["head_hash"]   = impl_->head.hex();
    j["key_id"]      = impl_->key_id;
    j["fingerprint"] = impl_->signer.fingerprint();

    std::string oldest_ts;
    std::string newest_ts;
    if (impl_->length.load() > 0) {
        auto oldest = impl_->storage->select_at(1);
        if (oldest) oldest_ts = oldest.value().header.ts.iso8601();
        auto newest = impl_->storage->select_at(impl_->length.load());
        if (newest) newest_ts = newest.value().header.ts.iso8601();
    }
    j["oldest_ts"] = oldest_ts;
    j["newest_ts"] = newest_ts;
    return j.dump();
}

std::string Ledger::head_attestation_json() const {
    // Sign the 32 bytes of the current head with the same key that signs
    // ledger entries. Empty chain -> head.bytes is all-zero; we still
    // return a well-formed signature (over the zero bytes), so the JSON
    // shape is constant regardless of length.
    const auto& head = impl_->head;
    auto sig = impl_->signer.sign(
        Bytes{head.bytes.data(), head.bytes.size()});

    static const char* d = "0123456789abcdef";
    std::string sig_hex;
    sig_hex.resize(sig.size() * 2);
    for (std::size_t i = 0; i < sig.size(); ++i) {
        sig_hex[2 * i + 0] = d[(sig[i] >> 4) & 0xF];
        sig_hex[2 * i + 1] = d[(sig[i] >> 0) & 0xF];
    }

    json j;
    j["length"]          = impl_->length.load();
    j["head_hex"]        = head.hex();
    j["key_id"]          = impl_->key_id;
    j["key_fingerprint"] = impl_->signer.fingerprint();
    j["head_signature"]  = sig_hex;
    return j.dump();
}

Result<std::uint64_t> Ledger::count_in_window(Time from, Time to) const {
    std::uint64_t n = 0;
    auto r = impl_->storage->for_each([&](const LedgerEntry& e) -> bool {
        // Half-open [from, to): include from, exclude to.
        if (e.header.ts < from) return true;
        if (!(e.header.ts < to)) return true;
        ++n;
        return true;
    });
    if (!r) return r.error();
    return n;
}

Result<std::vector<LedgerEntry>>
Ledger::range_by_patient(const std::string& patient) const {
    if (patient.empty()) {
        return Error::invalid("range_by_patient requires non-empty patient");
    }
    std::vector<LedgerEntry> out;
    auto r = impl_->storage->for_each([&](const LedgerEntry& e) -> bool {
        // Only inference-committed events carry the "patient" body field
        // produced by Inference::commit. Cheap prefilter on the raw body
        // string before parsing.
        if (e.header.event_type != "inference.committed") return true;
        if (e.body_json.find(patient) == std::string::npos) return true;
        try {
            auto j = nlohmann::json::parse(e.body_json);
            auto it = j.find("patient");
            if (it != j.end() && it->is_string() &&
                it->get<std::string>() == patient) {
                out.push_back(e);
            }
        } catch (...) {}
        return true;
    });
    if (!r) return r.error();
    return out;
}

Result<std::vector<LedgerEntry>>
Ledger::tail_for_patient(const std::string& patient, std::size_t n) const {
    if (patient.empty()) {
        return Error::invalid("tail_for_patient requires non-empty patient");
    }
    if (n == 0) return std::vector<LedgerEntry>{};

    // Ring buffer of the last `n` matches, oldest-first; reversed on
    // return so callers see most-recent-first. Mirrors tail_by_actor.
    std::vector<LedgerEntry> ring;
    ring.reserve(n);
    auto r = impl_->storage->for_each([&](const LedgerEntry& e) -> bool {
        // Only inference-committed events carry the "patient" body field.
        // Cheap substring prefilter before JSON parse.
        if (e.header.event_type != "inference.committed") return true;
        if (e.body_json.find(patient) == std::string::npos) return true;
        try {
            auto j = nlohmann::json::parse(e.body_json);
            auto it = j.find("patient");
            if (it == j.end() || !it->is_string() ||
                it->get<std::string>() != patient) {
                return true;
            }
        } catch (...) {
            return true;
        }
        if (ring.size() < n) {
            ring.push_back(e);
        } else {
            std::move(ring.begin() + 1, ring.end(), ring.begin());
            ring.back() = e;
        }
        return true;
    });
    if (!r) return r.error();
    std::reverse(ring.begin(), ring.end());
    return ring;
}

Result<std::vector<LedgerEntry>>
Ledger::range_for_patient_in_window(const std::string& patient,
                                    Time from, Time to) const {
    if (patient.empty()) {
        return Error::invalid("range_for_patient_in_window requires non-empty patient");
    }
    if (from > to) {
        return Error::invalid("range_for_patient_in_window: from > to");
    }
    std::vector<LedgerEntry> out;
    auto r = impl_->storage->for_each([&](const LedgerEntry& e) -> bool {
        // Half-open [from, to): include from, exclude to.
        if (e.header.ts < from) return true;
        if (!(e.header.ts < to)) return true;
        if (e.header.event_type != "inference.committed") return true;
        if (e.body_json.find(patient) == std::string::npos) return true;
        try {
            auto j = nlohmann::json::parse(e.body_json);
            auto it = j.find("patient");
            if (it != j.end() && it->is_string() &&
                it->get<std::string>() == patient) {
                out.push_back(e);
            }
        } catch (...) {}
        return true;
    });
    if (!r) return r.error();
    return out;
}

Result<std::vector<LedgerEntry>>
Ledger::events_after_seq(std::uint64_t after_seq) const {
    const auto len = impl_->length.load();
    if (after_seq >= len) return std::vector<LedgerEntry>{};
    return impl_->storage->select_range(after_seq + 1, len + 1);
}

Result<Hash> Ledger::content_address(std::uint64_t seq) const {
    if (seq == 0 || seq > impl_->length.load()) {
        return Error::invalid("content_address: seq out of range");
    }
    auto e = impl_->storage->select_at(seq);
    if (!e) return e.error();
    return e.value().entry_hash();
}

Result<Ledger::HistoricalHead> Ledger::head_at_seq(std::uint64_t seq) const {
    if (seq == 0 || seq > impl_->length.load()) {
        return Error::invalid("head_at_seq: seq out of range");
    }
    auto e = impl_->storage->select_at(seq);
    if (!e) return e.error();
    HistoricalHead h;
    h.seq       = e.value().header.seq;
    h.head_hash = e.value().entry_hash();
    h.ts        = e.value().header.ts;
    return h;
}

Result<LedgerEntry> Ledger::oldest_entry() const {
    if (impl_->length.load() == 0) {
        return Error::not_found("oldest_entry: chain is empty");
    }
    return impl_->storage->select_at(1);
}

Result<LedgerEntry> Ledger::newest_entry() const {
    const auto len = impl_->length.load();
    if (len == 0) {
        return Error::not_found("newest_entry: chain is empty");
    }
    return impl_->storage->select_at(len);
}

Result<std::uint64_t> Ledger::seq_at_time(Time t) const {
    if (impl_->length.load() == 0) return std::uint64_t{0};
    std::uint64_t out = 0;
    auto r = impl_->storage->for_each([&](const LedgerEntry& e) -> bool {
        // Entries are emitted in seq-ascending order. Stop scanning at
        // the first entry past t — everything beyond is also > t since
        // ts is monotonic across appends in normal flow. Note: if a
        // future backend permits non-monotonic ts we'd need to drop the
        // early-exit; for now this matches head_at_time's strategy.
        if (e.header.ts > t) return false;
        out = e.header.seq;
        return true;
    });
    if (!r) return r.error();
    return out;
}

std::string Ledger::InclusionProof::to_json() const {
    json j;
    j["seq"]        = seq;
    j["entry_hash"] = entry_hash.hex();
    j["head_seq"]   = head_seq;
    j["head_hash"]  = head_hash.hex();
    json chain = json::array();
    for (const auto& h : chain_to_head) chain.push_back(h.hex());
    j["chain_to_head"] = std::move(chain);
    return j.dump();
}

Result<Ledger::InclusionProof> Ledger::inclusion_proof(std::uint64_t seq) const {
    const auto len = impl_->length.load();
    if (seq == 0 || seq > len) {
        return Error::invalid("inclusion_proof: seq out of range");
    }
    auto target = impl_->storage->select_at(seq);
    if (!target) return target.error();

    InclusionProof proof;
    proof.seq        = seq;
    proof.entry_hash = target.value().entry_hash();
    proof.head_seq   = len;
    proof.head_hash  = impl_->head;

    if (seq < len) {
        auto rng = impl_->storage->select_range(seq + 1, len + 1);
        if (!rng) return rng.error();
        proof.chain_to_head.reserve(rng.value().size());
        for (const auto& e : rng.value()) {
            proof.chain_to_head.push_back(e.entry_hash());
        }
    }
    return proof;
}

Result<std::vector<LedgerEntry>>
Ledger::tail_in_window(Time from, Time to, std::size_t n) const {
    if (from > to) {
        return Error::invalid("tail_in_window: from > to");
    }
    if (n == 0) return std::vector<LedgerEntry>{};

    // Ring buffer of the last `n` matches, oldest-first; reversed on
    // return so callers see most-recent-first. Mirrors tail_by_actor.
    std::vector<LedgerEntry> ring;
    ring.reserve(n);
    auto r = impl_->storage->for_each([&](const LedgerEntry& e) -> bool {
        // Half-open [from, to): include from, exclude to.
        if (e.header.ts < from) return true;
        if (!(e.header.ts < to)) return true;
        if (ring.size() < n) {
            ring.push_back(e);
        } else {
            std::move(ring.begin() + 1, ring.end(), ring.begin());
            ring.back() = e;
        }
        return true;
    });
    if (!r) return r.error();
    std::reverse(ring.begin(), ring.end());
    return ring;
}

bool Ledger::has_entry(std::uint64_t seq) const noexcept {
    return seq > 0 && seq <= impl_->length.load();
}

bool Ledger::has_event_after_seq(std::uint64_t seq) const {
    return impl_->length.load() > seq;
}

std::string Ledger::Attestation::to_json() const {
    json j;
    j["length"]      = length;
    j["head"]        = head.hex();
    j["key_id"]      = key_id;
    j["fingerprint"] = fingerprint;
    return j.dump();
}

Ledger::Attestation Ledger::attest() const {
    Attestation a;
    a.length      = impl_->length.load();
    a.head        = impl_->head;
    a.key_id      = impl_->key_id;
    a.fingerprint = impl_->signer.fingerprint();
    return a;
}

Result<std::vector<LedgerEntry>>
Ledger::range_by_event_type(std::string_view event_type) const {
    if (event_type.empty()) {
        return Error::invalid("range_by_event_type requires non-empty event_type");
    }
    std::vector<LedgerEntry> out;
    auto r = impl_->storage->for_each([&](const LedgerEntry& e) -> bool {
        if (e.header.event_type == event_type) out.push_back(e);
        return true;
    });
    if (!r) return r.error();
    return out;
}

Result<LedgerEntry>
Ledger::find_first_by_event_type(std::string_view event_type) const {
    if (event_type.empty()) {
        return Error::invalid("find_first_by_event_type requires non-empty event_type");
    }
    std::optional<LedgerEntry> hit;
    auto r = impl_->storage->for_each([&](const LedgerEntry& e) -> bool {
        if (e.header.event_type == event_type) {
            hit = e;
            return false;  // stop scanning on first match
        }
        return true;
    });
    if (!r) return r.error();
    if (!hit) {
        return Error::not_found(fmt::format(
            "no ledger entry matches event_type '{}'", event_type));
    }
    return std::move(*hit);
}

bool Ledger::has_event_type(std::string_view event_type) const {
    if (event_type.empty()) return false;
    bool found = false;
    auto r = impl_->storage->for_each([&](const LedgerEntry& e) -> bool {
        if (e.header.event_type == event_type) {
            found = true;
            return false;  // stop scanning on first match
        }
        return true;
    });
    (void)r;  // backend errors are observable via verify(); this is a best-effort probe
    return found;
}

Result<std::unordered_map<std::string, std::uint64_t>>
Ledger::byte_size_per_tenant() const {
    std::unordered_map<std::string, std::uint64_t> out;
    auto r = impl_->storage->for_each([&](const LedgerEntry& e) -> bool {
        out[e.header.tenant] += e.body_json.size();
        return true;
    });
    if (!r) return r.error();
    return out;
}

Result<std::vector<std::pair<std::string, std::uint64_t>>>
Ledger::most_active_actors(std::size_t top_n) const {
    if (top_n == 0) return std::vector<std::pair<std::string, std::uint64_t>>{};
    std::unordered_map<std::string, std::uint64_t> counts;
    auto r = impl_->storage->for_each([&](const LedgerEntry& e) -> bool {
        counts[e.header.actor] += 1;
        return true;
    });
    if (!r) return r.error();
    std::vector<std::pair<std::string, std::uint64_t>> all;
    all.reserve(counts.size());
    for (auto& [a, c] : counts) all.emplace_back(a, c);
    // Sort by count desc, ties alphabetic asc for deterministic output.
    std::sort(all.begin(), all.end(),
              [](const auto& l, const auto& r) {
                  if (l.second != r.second) return l.second > r.second;
                  return l.first < r.first;
              });
    if (all.size() > top_n) all.resize(top_n);
    return all;
}

Result<std::vector<LedgerEntry>>
Ledger::range_by_actor(std::string_view actor) const {
    if (actor.empty()) {
        return Error::invalid("range_by_actor requires non-empty actor");
    }
    std::vector<LedgerEntry> out;
    auto r = impl_->storage->for_each([&](const LedgerEntry& e) -> bool {
        if (e.header.actor == actor) out.push_back(e);
        return true;
    });
    if (!r) return r.error();
    return out;
}

Result<std::vector<LedgerEntry>> Ledger::oldest_n(std::size_t n) const {
    if (n == 0) return std::vector<LedgerEntry>{};
    std::vector<LedgerEntry> out;
    out.reserve(n);
    auto r = impl_->storage->for_each([&](const LedgerEntry& e) -> bool {
        out.push_back(e);
        // Stop the scan once we've collected n entries.
        return out.size() < n;
    });
    if (!r) return r.error();
    return out;
}

Result<std::vector<LedgerEntry>>
Ledger::filter(std::string_view event_type, const std::string& tenant) const {
    if (event_type.empty()) {
        return Error::invalid("filter requires non-empty event_type");
    }
    std::vector<LedgerEntry> out;
    auto r = impl_->storage->for_each([&](const LedgerEntry& e) -> bool {
        if (e.header.event_type == event_type && e.header.tenant == tenant) {
            out.push_back(e);
        }
        return true;
    });
    if (!r) return r.error();
    return out;
}

Result<std::uint64_t> Ledger::byte_size_for_tenant(const std::string& tenant) const {
    const auto chain_len = impl_->length.load();
    if (chain_len == 0) return std::uint64_t{0};

    constexpr std::uint64_t kChunk = 1024;
    std::uint64_t total = 0;
    for (std::uint64_t start = 1; start <= chain_len; start += kChunk) {
        std::uint64_t end = std::min(chain_len + 1, start + kChunk);
        auto rng = impl_->storage->select_range_for_tenant(tenant, start, end);
        if (!rng) return rng.error();
        for (const auto& e : rng.value()) {
            total += e.body_json.size();
        }
    }
    return total;
}

Result<std::pair<std::uint64_t, std::uint64_t>>
Ledger::seq_range_for_tenant(const std::string& tenant) const {
    const auto chain_len = impl_->length.load();
    if (chain_len == 0) return std::pair<std::uint64_t, std::uint64_t>{0, 0};

    constexpr std::uint64_t kChunk = 1024;
    std::uint64_t oldest = 0;
    std::uint64_t newest = 0;
    for (std::uint64_t start = 1; start <= chain_len; start += kChunk) {
        std::uint64_t end = std::min(chain_len + 1, start + kChunk);
        auto rng = impl_->storage->select_range_for_tenant(tenant, start, end);
        if (!rng) return rng.error();
        for (const auto& e : rng.value()) {
            if (oldest == 0) oldest = e.header.seq;
            newest = e.header.seq;
        }
    }
    return std::pair<std::uint64_t, std::uint64_t>{oldest, newest};
}

Result<Ledger::HistoricalHead> Ledger::head_at_time(Time t) const {
    HistoricalHead h{};
    if (impl_->length.load() == 0) return h;
    auto r = impl_->storage->for_each([&](const LedgerEntry& e) -> bool {
        if (e.header.ts > t) return false;  // stop at first entry past t
        h.seq       = e.header.seq;
        h.head_hash = e.entry_hash();
        h.ts        = e.header.ts;
        return true;
    });
    if (!r) return r.error();
    return h;
}

Result<std::vector<LedgerEntry>>
Ledger::tail_by_actor(std::string_view actor, std::size_t n) const {
    if (actor.empty()) {
        return Error::invalid("tail_by_actor requires non-empty actor");
    }
    if (n == 0) return std::vector<LedgerEntry>{};

    // Ring buffer of the last `n` matches, oldest-first. We'll reverse on
    // return so callers see most-recent-first.
    std::vector<LedgerEntry> ring;
    ring.reserve(n);
    auto r = impl_->storage->for_each([&](const LedgerEntry& e) -> bool {
        if (e.header.actor != actor) return true;
        if (ring.size() < n) {
            ring.push_back(e);
        } else {
            // Slide: drop oldest, push newest.
            std::move(ring.begin() + 1, ring.end(), ring.begin());
            ring.back() = e;
        }
        return true;
    });
    if (!r) return r.error();
    std::reverse(ring.begin(), ring.end());
    return ring;
}

Result<LedgerEntry> Ledger::find_by_inference_id(std::string_view id) const {
    if (id.empty()) {
        return Error::invalid("find_by_inference_id requires non-empty id");
    }
    std::optional<LedgerEntry> hit;
    auto r = impl_->storage->for_each([&](const LedgerEntry& e) -> bool {
        // Cheap prefilter: skip entries whose body can't contain the id.
        if (e.body_json.find(id) == std::string::npos) return true;
        try {
            auto j = nlohmann::json::parse(e.body_json);
            auto it = j.find("inference_id");
            if (it != j.end() && it->is_string() &&
                it->get<std::string>() == id) {
                hit = e;
                return false;  // stop scanning
            }
        } catch (...) {
            // body is not JSON — ignore; ledger never appends non-JSON
            // bodies in normal flow, but defensive against future event
            // types.
        }
        return true;
    });
    if (!r) return r.error();
    if (!hit) {
        return Error::not_found("no ledger entry matches inference_id");
    }
    return std::move(*hit);
}

LedgerCheckpoint Ledger::checkpoint() const {
    LedgerCheckpoint cp;
    cp.seq        = impl_->length.load();
    cp.head_hash  = impl_->head;
    cp.ts         = Time::now();
    cp.key_id     = impl_->key_id;
    cp.public_key = impl_->public_key;
    auto inp      = checkpoint_sign_input(cp.seq, cp.head_hash, cp.ts, cp.key_id);
    cp.signature  = impl_->signer.sign(
        Bytes{reinterpret_cast<const std::uint8_t*>(inp.data()), inp.size()});
    return cp;
}

Result<void> verify_checkpoint(const LedgerCheckpoint& cp) {
    auto inp = checkpoint_sign_input(cp.seq, cp.head_hash, cp.ts, cp.key_id);
    if (!KeyStore::verify(
            Bytes{reinterpret_cast<const std::uint8_t*>(inp.data()), inp.size()},
            std::span<const std::uint8_t, KeyStore::sig_bytes>{
                cp.signature.data(), cp.signature.size()},
            std::span<const std::uint8_t, KeyStore::pk_bytes>{
                cp.public_key.data(), cp.public_key.size()})) {
        return Error::integrity("checkpoint signature does not match public key");
    }
    return Result<void>::ok();
}

// ---- Subscription -------------------------------------------------------

Ledger::Subscription::Subscription(Subscription&& other) noexcept
    : ledger_(other.ledger_), id_(other.id_) {
    other.ledger_ = nullptr;
    other.id_     = 0;
}

Ledger::Subscription& Ledger::Subscription::operator=(Subscription&& other) noexcept {
    if (this != &other) {
        if (ledger_ && id_) ledger_->unsubscribe(id_);
        ledger_       = other.ledger_;
        id_           = other.id_;
        other.ledger_ = nullptr;
        other.id_     = 0;
    }
    return *this;
}

Ledger::Subscription::~Subscription() {
    if (ledger_ && id_) ledger_->unsubscribe(id_);
}

Ledger::Subscription Ledger::subscribe(Subscriber cb) {
    std::lock_guard<std::mutex> lk(impl_->sub_mu);
    auto id = impl_->next_sub_id++;
    impl_->subs.emplace_back(id, std::move(cb));
    return Subscription{this, id};
}

Result<std::uint64_t> Ledger::cumulative_body_bytes() const {
    std::uint64_t total = 0;
    auto r = impl_->storage->for_each([&](const LedgerEntry& e) -> bool {
        total += e.body_json.size();
        return true;
    });
    if (!r) return r.error();
    return total;
}

Result<std::vector<LedgerEntry>>
Ledger::tail_in_seq_range(std::uint64_t start, std::uint64_t end,
                          std::size_t n) const {
    if (n == 0) return std::vector<LedgerEntry>{};
    if (start >= end) return std::vector<LedgerEntry>{};

    auto rng = impl_->storage->select_range(start, end);
    if (!rng) return rng.error();
    auto& entries = rng.value();

    // Keep only the last `n` matches via a bounded ring buffer to avoid
    // an O(range) copy when n << range. Mirrors tail_in_window.
    std::vector<LedgerEntry> ring;
    ring.reserve(std::min(n, entries.size()));
    for (auto& e : entries) {
        if (ring.size() < n) {
            ring.push_back(std::move(e));
        } else {
            std::move(ring.begin() + 1, ring.end(), ring.begin());
            ring.back() = std::move(e);
        }
    }
    std::reverse(ring.begin(), ring.end());
    return ring;
}

Result<std::vector<Hash>>
Ledger::merkle_proof_path(std::uint64_t seq) const {
    const auto len = impl_->length.load();
    if (seq == 0 || seq > len) {
        return Error::invalid("merkle_proof_path: seq out of range");
    }
    // [seq, len+1) covers the entry at `seq` plus every successor up
    // to the head, inclusive. Element 0 is the entry's own hash.
    auto rng = impl_->storage->select_range(seq, len + 1);
    if (!rng) return rng.error();

    std::vector<Hash> path;
    path.reserve(rng.value().size());
    for (const auto& e : rng.value()) {
        path.push_back(e.entry_hash());
    }
    return path;
}

Result<std::uint64_t>
Ledger::longest_run_of_event_type(std::string_view event_type) const {
    if (event_type.empty()) {
        return Error::invalid(
            "longest_run_of_event_type requires non-empty event_type");
    }
    std::uint64_t best    = 0;
    std::uint64_t current = 0;
    auto r = impl_->storage->for_each([&](const LedgerEntry& e) -> bool {
        if (e.header.event_type == event_type) {
            ++current;
            if (current > best) best = current;
        } else {
            current = 0;
        }
        return true;
    });
    if (!r) return r.error();
    return best;
}

void Ledger::unsubscribe(std::uint64_t id) {
    std::lock_guard<std::mutex> lk(impl_->sub_mu);
    auto it = std::remove_if(impl_->subs.begin(), impl_->subs.end(),
                             [id](const auto& p) { return p.first == id; });
    impl_->subs.erase(it, impl_->subs.end());
}

}  // namespace asclepius
