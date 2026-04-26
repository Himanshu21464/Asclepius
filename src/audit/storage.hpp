// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
//
// Internal storage interface for the ledger. The Ledger class is backed by
// a SQLite implementation (the only backend). Kept as an abstract base so
// the substrate can grow alternative on-disk formats later without
// reworking ledger.cpp.
//
// Not a public header — consumers should use the Ledger interface in
// include/asclepius/audit.hpp.

#ifndef ASCLEPIUS_AUDIT_STORAGE_HPP
#define ASCLEPIUS_AUDIT_STORAGE_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "asclepius/audit.hpp"
#include "asclepius/core.hpp"
#include "asclepius/hashing.hpp"

namespace asclepius::detail {

// One signed, chained, persisted entry plus its computed entry_hash.
struct StoredEntry {
    LedgerEntry entry;
    Hash        entry_hash;
};

// Abstract over the persistence layer. Every method returns Result so the
// Ledger can map backend errors uniformly without leaking SQLite/Postgres
// types into the public API.
class LedgerStorage {
public:
    virtual ~LedgerStorage() = default;

    // Idempotent — creates tables/indexes if absent.
    virtual Result<void> init_schema() = 0;

    // Append one entry. Storage is responsible for persistence and indexing.
    virtual Result<void> insert_entry(const LedgerEntry& e, const Hash& entry_hash) = 0;

    // Atomic transactional helpers. Backed by SQLite's
    // BEGIN IMMEDIATE / COMMIT / ROLLBACK.
    virtual Result<void> begin_transaction()   = 0;
    virtual Result<void> commit_transaction()  = 0;
    virtual Result<void> rollback_transaction() = 0;

    // Returns (seq, entry_hash) of the most recent row, or (0, Hash::zero()) if empty.
    virtual Result<std::pair<std::uint64_t, Hash>> read_tail() = 0;

    virtual Result<LedgerEntry>              select_at(std::uint64_t seq) = 0;
    virtual Result<std::vector<LedgerEntry>> select_tail(std::size_t n) = 0;
    virtual Result<std::vector<LedgerEntry>> select_range(std::uint64_t start, std::uint64_t end) = 0;
    virtual Result<std::vector<LedgerEntry>> select_all() = 0;

    // Time-range query: every entry with from_ns <= ts_ns < to_ns, seq ASC.
    virtual Result<std::vector<LedgerEntry>> select_time_range(std::int64_t from_ns,
                                                               std::int64_t to_ns) = 0;

    // Tenant-scoped reads: same shapes as the unscoped variants but
    // filter to a specific tenant. The "" tenant (default for entries
    // without a configured tenant) is its own scope; passing "" returns
    // only those entries, never all entries.
    virtual Result<std::vector<LedgerEntry>> select_tail_for_tenant(const std::string& tenant,
                                                                    std::size_t n) = 0;
    virtual Result<std::vector<LedgerEntry>> select_range_for_tenant(const std::string& tenant,
                                                                     std::uint64_t start,
                                                                     std::uint64_t end) = 0;

    // Streaming visit of the entire chain in seq ASC. Stops if the visitor
    // returns false. Lets verify() and large exports skip materialising the
    // whole chain in memory.
    virtual Result<void> for_each(std::function<bool(const LedgerEntry&)> visitor) = 0;
};

// Open a SQLite-backed storage at the given filesystem path.
Result<std::unique_ptr<LedgerStorage>> make_sqlite_storage(const std::string& path);

}  // namespace asclepius::detail

#endif  // ASCLEPIUS_AUDIT_STORAGE_HPP
