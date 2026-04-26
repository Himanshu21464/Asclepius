// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#ifndef ASCLEPIUS_AUDIT_HPP
#define ASCLEPIUS_AUDIT_HPP

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "asclepius/core.hpp"
#include "asclepius/hashing.hpp"

namespace asclepius {

// ---- KeyStore ------------------------------------------------------------
//
// An Ed25519 signing keypair. Keys are 32-byte public, 64-byte secret. The
// secret is held in locked memory where the platform supports it.

class KeyStore {
public:
    static constexpr std::size_t pk_bytes  = 32;
    static constexpr std::size_t sk_bytes  = 64;
    static constexpr std::size_t sig_bytes = 64;

    KeyStore(const KeyStore&)            = delete;
    KeyStore& operator=(const KeyStore&) = delete;
    KeyStore(KeyStore&&) noexcept;
    KeyStore& operator=(KeyStore&&) noexcept;
    ~KeyStore();

    // Generate a fresh keypair from a CSPRNG.
    static KeyStore generate();

    // Reconstruct from a 32-byte seed. The seed must be high-entropy.
    static Result<KeyStore> from_seed(std::span<const std::uint8_t, 32> seed);

    // Load from a PEM-style serialized form produced by serialize().
    static Result<KeyStore> deserialize(std::string_view encoded);
    std::string             serialize() const;

    // The key id is the hex of the public key truncated to 16 hex chars.
    std::string                            key_id() const;
    std::array<std::uint8_t, pk_bytes>     public_key() const;
    std::array<std::uint8_t, sig_bytes>    sign(Bytes message) const;

    static bool verify(Bytes                                message,
                       std::span<const std::uint8_t, sig_bytes> signature,
                       std::span<const std::uint8_t, pk_bytes>  pk);

private:
    KeyStore() = default;
    struct Impl;
    Impl* impl_ = nullptr;
};

// ---- Ledger entry --------------------------------------------------------

struct LedgerEntryHeader {
    std::uint64_t seq{};
    Time          ts{};
    Hash          prev_hash{};
    Hash          payload_hash{};
    std::string   actor;
    std::string   event_type;
    std::string   tenant;
};

struct LedgerEntry {
    LedgerEntryHeader                                  header;
    std::string                                        body_json;  // canonical JSON
    std::array<std::uint8_t, KeyStore::sig_bytes>      signature{};
    std::string                                        key_id;

    Hash entry_hash() const;  // hash(header || body || sig || key_id)
};

// ---- Checkpoint (declared before Ledger so Ledger::checkpoint() can name it) -

struct LedgerCheckpoint {
    std::uint64_t                                 seq{};
    Hash                                          head_hash{};
    Time                                          ts{};
    std::string                                   key_id;
    std::array<std::uint8_t, KeyStore::sig_bytes> signature{};
    std::array<std::uint8_t, KeyStore::pk_bytes>  public_key{};

    std::string to_json() const;
    static Result<LedgerCheckpoint> from_json(std::string_view s);
};

Result<void> verify_checkpoint(const LedgerCheckpoint& cp);

// ---- Ledger --------------------------------------------------------------
//
// An append-only, Merkle-chained, Ed25519-signed event log persisted to a
// SQLite database. The chain head can be checkpointed externally for
// off-system attestation.

class Ledger {
public:
    Ledger(const Ledger&)            = delete;
    Ledger& operator=(const Ledger&) = delete;
    Ledger(Ledger&&) noexcept;
    Ledger& operator=(Ledger&&) noexcept;
    ~Ledger();

    // Open or create a ledger. Two URL schemes:
    //   - filesystem path or "file://…" → SQLite-backed (the default)
    //   - "postgres://…" or "postgresql://…" → PostgreSQL-backed
    // If a key is not provided one is generated and stored alongside the
    // SQLite file (0600). For Postgres backends the key file lives next to
    // the cwd as <key_id>.key — see open_postgres notes in deploy.html.
    static Result<Ledger> open(std::filesystem::path path);
    static Result<Ledger> open(std::filesystem::path path, KeyStore key);
    static Result<Ledger> open_uri(const std::string& uri);
    static Result<Ledger> open_uri(const std::string& uri, KeyStore key);

    // Append a new event. body must be a canonicalizable JSON value.
    Result<LedgerEntry> append(std::string event_type,
                               std::string actor,
                               nlohmann::json body,
                               std::string tenant = "");

    // One spec per entry in a batch.
    struct AppendSpec {
        std::string    event_type;
        std::string    actor;
        nlohmann::json body;
        std::string    tenant;
    };

    // Append N entries atomically. Either all entries land in the chain
    // (ordered, gap-free, prev_hash-linked) or none of them do — the
    // backend transaction rolls back on failure. Returns the appended
    // entries in seq-ascending order. Subscribers fire once per entry,
    // after the entire batch has committed.
    //
    // Empty input is a valid no-op; returns an empty vector.
    Result<std::vector<LedgerEntry>> append_batch(std::vector<AppendSpec> specs);

    // Read an entry by sequence number.
    Result<LedgerEntry> at(std::uint64_t seq) const;

    // Read the last n entries (most recent first).
    Result<std::vector<LedgerEntry>> tail(std::size_t n) const;

    // Read entries within [start, end) seq range.
    Result<std::vector<LedgerEntry>> range(std::uint64_t start, std::uint64_t end) const;

    // Read entries by ts_ns in [from, to). Half-open. Index-backed.
    Result<std::vector<LedgerEntry>> range_by_time(Time from, Time to) const;

    // Tenant-scoped reads. Filter to entries whose header.tenant matches
    // the supplied string. The empty tenant ("") is its own scope; passing
    // "" returns only those entries, not all entries. Uses an index on
    // (tenant, seq) for O(log n + k) on each backend.
    Result<std::vector<LedgerEntry>> tail_for_tenant(const std::string& tenant,
                                                     std::size_t n) const;
    Result<std::vector<LedgerEntry>> range_for_tenant(const std::string& tenant,
                                                      std::uint64_t start,
                                                      std::uint64_t end) const;

    // Verify the entire chain: each entry's prev_hash matches the previous,
    // each signature matches the registered public key, payload_hash matches.
    // Streams through entries one at a time so memory usage is O(1) regardless
    // of chain length.
    Result<void> verify() const;

    // Current chain head (hash of the most recent entry, or zero if empty).
    Hash          head() const;
    std::uint64_t length() const;

    // Public verification key bytes for this ledger's signer.
    std::array<std::uint8_t, KeyStore::pk_bytes> public_key() const;
    std::string                                  key_id() const;

    // Sign an arbitrary attestation (e.g., an evidence-bundle manifest) with
    // the same key that signs ledger entries. The signature can be verified
    // against public_key() with KeyStore::verify. This is intentionally
    // narrow: forging a ledger entry would still require matching seq,
    // prev_hash, and the canonical entry encoding, none of which this method
    // generates.
    std::array<std::uint8_t, KeyStore::sig_bytes> sign_attestation(Bytes message) const;

    // Produce a checkpoint for the current chain head. Self-contained
    // proof that this signing key (whose pubkey is embedded) attested
    // to the seq/head_hash pair at the returned ts.
    LedgerCheckpoint checkpoint() const;

    // ---- Subscription ---------------------------------------------------
    //
    // Register a callback that fires after each successful append, on the
    // appender's thread. Returned handle owns the registration; destroying
    // it unsubscribes. Multiple subscribers are supported and called in
    // registration order.
    //
    // Callbacks must not call back into Ledger::append from the same
    // thread (would re-enter the append mutex and deadlock). They MAY
    // hand the entry to a queue / thread-pool / external system.

    using Subscriber = std::function<void(const LedgerEntry&)>;

    class Subscription {
    public:
        Subscription() = default;
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        Subscription(Subscription&&) noexcept;
        Subscription& operator=(Subscription&&) noexcept;
        ~Subscription();

    private:
        friend class Ledger;
        Subscription(Ledger* ledger, std::uint64_t id) : ledger_(ledger), id_(id) {}
        Ledger*       ledger_ = nullptr;
        std::uint64_t id_     = 0;
    };

    Subscription subscribe(Subscriber cb);
    void         unsubscribe(std::uint64_t id);

private:
    Ledger() = default;
    struct Impl;
    Impl* impl_ = nullptr;
};

// ---- JSONL export / import ----------------------------------------------
//
// One JSON object per line, emitted in seq-ascending order. Each line is
// the canonical-JSON form of a LedgerEntry: every header field, the body,
// the hex signature, the key id. Suitable for `jq`-driven auditing,
// cold-storage archive, or substrate-agnostic transfer.
//
// Importing into a fresh ledger preserves seq, prev_hash, signatures,
// entry hashes — i.e. the destination's verify() passes against the
// same KeyStore the source used. Refuses non-empty destinations.

struct LedgerExportStats {
    std::uint64_t entries_written = 0;
    Hash          last_entry_hash{};
};

struct LedgerImportStats {
    std::uint64_t entries_imported = 0;
    Hash          dest_head{};
};

class LedgerJsonl {
public:
    // Stream every entry in src, seq ASC, to out_path as JSONL. The file
    // is created (or truncated) and closed cleanly even on early exit.
    static Result<LedgerExportStats>
    export_to(const std::string& src_uri, const std::string& out_path);

    // Read JSONL from in_path, verify each entry's signature against the
    // supplied KeyStore's public key, and append to dst. Stops on the
    // first verification failure with an integrity error.
    static Result<LedgerImportStats>
    import_to(const std::string& in_path, const std::string& dst_uri, KeyStore key);
};

// ---- LedgerMigrator -----------------------------------------------------
//
// Copy a chain from one backend to another, byte-for-byte. The destination
// receives the same entries in the same order with the same signatures, so
// the destination's verify() passes against the same public key the source
// used. Useful for SQLite → Postgres migrations and for cold-storage
// snapshots.

struct LedgerMigrationStats {
    std::uint64_t entries_copied = 0;
    Hash          source_head{};
    Hash          dest_head{};
};

class LedgerMigrator {
public:
    // Copy every entry from src to dst. dst must be empty; otherwise the
    // chain would fork at the first entry. Verifies the source mid-stream
    // and stops with an integrity error if the source chain is broken.
    static Result<LedgerMigrationStats>
    copy(const std::string& src_uri, const std::string& dst_uri, KeyStore key);
};

}  // namespace asclepius

#endif  // ASCLEPIUS_AUDIT_HPP
