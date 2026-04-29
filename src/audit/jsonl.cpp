// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
//
// JSONL export / import for the ledger. One canonical-JSON object per
// line, ordered by seq. Substrate-agnostic backup format: any auditor
// can `jq` it, any operator can re-ingest it into a fresh DB.

#include "asclepius/audit.hpp"
#include "asclepius/canonical.hpp"

#include "storage.hpp"

#include <fmt/core.h>
#include <nlohmann/json.hpp>

#include <cstring>
#include <fstream>
#include <optional>

namespace asclepius {

using nlohmann::json;

namespace {

// Round 103 / 104: hex helpers hoisted to canonical::; this file routes
// through the substrate primitive (canonical::to_hex for encode,
// canonical::from_hex_into for fixed-size decode into a caller-supplied
// span).

json entry_to_json(const LedgerEntry& e) {
    json j;
    j["seq"]        = e.header.seq;
    j["ts_ns"]      = e.header.ts.nanos_since_epoch();
    j["prev_hash"]  = canonical::to_hex({e.header.prev_hash.bytes.data(),    e.header.prev_hash.bytes.size()});
    j["payload_hash"] = canonical::to_hex({e.header.payload_hash.bytes.data(), e.header.payload_hash.bytes.size()});
    j["actor"]      = e.header.actor;
    j["event_type"] = e.header.event_type;
    j["tenant"]     = e.header.tenant;
    j["body"]       = e.body_json;
    j["signature"]  = canonical::to_hex({e.signature.data(), e.signature.size()});
    j["key_id"]     = e.key_id;
    j["entry_hash"] = canonical::to_hex({e.entry_hash().bytes.data(), e.entry_hash().bytes.size()});
    return j;
}

Result<LedgerEntry> entry_from_json(const json& j) {
    LedgerEntry e;
    try {
        e.header.seq        = j.at("seq").get<std::uint64_t>();
        e.header.ts         = Time{j.at("ts_ns").get<std::int64_t>()};
        if (!canonical::from_hex_into(
                j.at("prev_hash").get<std::string>(),
                {e.header.prev_hash.bytes.data(), e.header.prev_hash.bytes.size()}))
            return Error::invalid("malformed prev_hash hex");
        if (!canonical::from_hex_into(
                j.at("payload_hash").get<std::string>(),
                {e.header.payload_hash.bytes.data(), e.header.payload_hash.bytes.size()}))
            return Error::invalid("malformed payload_hash hex");
        e.header.actor      = j.at("actor").get<std::string>();
        e.header.event_type = j.at("event_type").get<std::string>();
        e.header.tenant     = j.value("tenant", std::string{});
        e.body_json         = j.at("body").get<std::string>();
        if (!canonical::from_hex_into(
                j.at("signature").get<std::string>(),
                {e.signature.data(), e.signature.size()}))
            return Error::invalid("malformed signature hex");
        e.key_id            = j.at("key_id").get<std::string>();
    } catch (const std::exception& ex) {
        return Error::invalid(std::string{"jsonl entry parse: "} + ex.what());
    }
    return e;
}

}  // namespace

Result<LedgerExportStats>
LedgerJsonl::export_to(const std::string& src_uri, const std::string& out_path) {
    auto src = detail::make_sqlite_storage(src_uri);
    if (!src) return src.error();

    std::ofstream out(out_path, std::ios::trunc | std::ios::binary);
    if (!out) return Error::backend(fmt::format("could not open {} for writing", out_path));

    LedgerExportStats stats{};
    std::optional<Error> err;
    auto r = src.value()->for_each([&](const LedgerEntry& e) -> bool {
        out << entry_to_json(e).dump() << '\n';
        if (!out) {
            err = Error::backend("write failure during export");
            return false;
        }
        ++stats.entries_written;
        stats.last_entry_hash = e.entry_hash();
        return true;
    });
    if (!r)   return r.error();
    if (err)  return err.value();
    out.close();
    return stats;
}

Result<LedgerImportStats>
LedgerJsonl::import_to(const std::string& in_path,
                       const std::string& dst_uri,
                       KeyStore           /*key*/) {
    std::ifstream in(in_path);
    if (!in) return Error::backend(fmt::format("could not open {} for reading", in_path));

    auto dst = detail::make_sqlite_storage(dst_uri);
    if (!dst) return dst.error();

    auto dst_tail = dst.value()->read_tail();
    if (!dst_tail) return dst_tail.error();
    if (dst_tail.value().first > 0) {
        return Error::invalid(fmt::format(
            "destination chain already has {} entries; refusing to import",
            dst_tail.value().first));
    }

    LedgerImportStats stats{};
    Hash          expected_prev = Hash::zero();
    std::uint64_t expected_seq  = 1;
    std::string   line;
    std::size_t   line_no = 0;

    while (std::getline(in, line)) {
        ++line_no;
        if (line.empty()) continue;
        json j;
        try { j = json::parse(line); }
        catch (const std::exception& ex) {
            return Error::invalid(fmt::format("line {}: parse: {}", line_no, ex.what()));
        }
        auto er = entry_from_json(j);
        if (!er) return er.error();
        const auto& e = er.value();

        if (e.header.seq != expected_seq) {
            return Error::integrity(fmt::format(
                "line {}: seq {} (expected {})", line_no, e.header.seq, expected_seq));
        }
        if (!(e.header.prev_hash == expected_prev)) {
            return Error::integrity(fmt::format(
                "line {}: prev_hash mismatch at seq {}", line_no, e.header.seq));
        }

        Hash entry_h = e.entry_hash();
        auto ins = dst.value()->insert_entry(e, entry_h);
        if (!ins) return ins.error();

        expected_prev = entry_h;
        ++expected_seq;
        ++stats.entries_imported;
        stats.dest_head = entry_h;
    }
    return stats;
}

}  // namespace asclepius
