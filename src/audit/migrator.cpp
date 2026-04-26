// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
//
// Cross-backend chain copy. Reads entries from one storage, writes them
// to another byte-for-byte. The destination ends up with an identical
// chain (same signatures, same entry hashes) so the same KeyStore that
// signed the source verifies the destination.

#include "asclepius/audit.hpp"

#include "storage.hpp"

#include <fmt/core.h>
#include <nlohmann/json.hpp>

namespace asclepius {

using nlohmann::json;

namespace {

// We need to recompute the entry_hash to insert it (the storage interface
// takes both the entry and its hash). The hash is deterministic from the
// canonical-JSON body + header, so this matches the source by construction.
Hash recompute_entry_hash(const LedgerEntry& e) {
    return e.entry_hash();
}

}  // namespace

Result<LedgerMigrationStats>
LedgerMigrator::copy(const std::string& src_uri,
                     const std::string& dst_uri,
                     KeyStore           /*key*/) {
    LedgerMigrationStats stats{};

    auto src = detail::make_storage(src_uri);
    if (!src) return src.error();
    auto dst = detail::make_storage(dst_uri);
    if (!dst) return dst.error();

    // Refuse to write into a non-empty destination. A migration into a
    // populated chain would fork the seq numbering and break verify().
    auto dst_tail = dst.value()->read_tail();
    if (!dst_tail) return dst_tail.error();
    if (dst_tail.value().first > 0) {
        return Error::invalid(fmt::format(
            "destination chain already has {} entries; refusing to copy",
            dst_tail.value().first));
    }

    // Walk source in seq order, copy each entry, also verify chain
    // integrity along the way so a broken source surfaces immediately.
    Hash          expected_prev = Hash::zero();
    std::uint64_t expected_seq  = 1;
    std::optional<Error> visit_err;

    auto r = src.value()->for_each([&](const LedgerEntry& e) -> bool {
        if (e.header.seq != expected_seq) {
            visit_err = Error::integrity(fmt::format(
                "source ledger gap at seq {} (expected {})",
                e.header.seq, expected_seq));
            return false;
        }
        if (!(e.header.prev_hash == expected_prev)) {
            visit_err = Error::integrity(fmt::format(
                "source chain break at seq {}", e.header.seq));
            return false;
        }
        Hash h = recompute_entry_hash(e);
        auto ins = dst.value()->insert_entry(e, h);
        if (!ins) {
            visit_err = ins.error();
            return false;
        }
        expected_prev = h;
        ++expected_seq;
        ++stats.entries_copied;
        stats.dest_head = h;
        return true;
    });
    if (!r)         return r.error();
    if (visit_err)  return visit_err.value();

    stats.source_head = expected_prev;
    return stats;
}

}  // namespace asclepius
