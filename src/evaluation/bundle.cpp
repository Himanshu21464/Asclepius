// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include "asclepius/evaluation.hpp"

#include "asclepius/audit.hpp"
#include "asclepius/hashing.hpp"
#include "asclepius/telemetry.hpp"
#include "asclepius/version.hpp"

#include <fmt/core.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace asclepius {

namespace {

// ---- Minimal USTAR tar writer -------------------------------------------
//
// We embed a trivial tar writer rather than depending on libarchive. The
// evidence bundle is intended to be opened by `tar xf`, jq, and openssl.
// USTAR is the lowest-common-denominator format that all of those handle.

constexpr std::size_t kBlock = 512;

struct TarHeader {
    char name    [100];
    char mode    [8];
    char uid     [8];
    char gid     [8];
    char size    [12];
    char mtime   [12];
    char chksum  [8];
    char typeflag;
    char linkname[100];
    char magic   [6];
    char version [2];
    char uname   [32];
    char gname   [32];
    char devmajor[8];
    char devminor[8];
    char prefix  [155];
    char pad     [12];
};
static_assert(sizeof(TarHeader) == kBlock, "TarHeader must be 512 bytes");

void octal(char* dst, std::size_t n, std::uint64_t v) {
    // Writes a NUL-terminated zero-padded octal string of length n-1.
    std::memset(dst, '0', n - 1);
    dst[n - 1] = '\0';
    char buf[32];
    std::size_t len = std::snprintf(buf, sizeof(buf), "%llo",
                                     static_cast<unsigned long long>(v));
    if (len >= n) len = n - 1;
    std::memcpy(dst + (n - 1 - len), buf, len);
}

std::uint32_t tar_checksum(const TarHeader& h) {
    std::uint32_t s = 0;
    auto* p = reinterpret_cast<const std::uint8_t*>(&h);
    for (std::size_t i = 0; i < sizeof(TarHeader); ++i) s += p[i];
    return s;
}

void write_file_entry(std::ostream& out,
                      std::string_view name,
                      Bytes data) {
    TarHeader h{};
    std::memset(&h, 0, sizeof(h));
    auto copy_name = name.size() < sizeof(h.name) ? name : name.substr(0, sizeof(h.name) - 1);
    std::memcpy(h.name, copy_name.data(), copy_name.size());
    octal(h.mode,  sizeof(h.mode),  0644);
    octal(h.uid,   sizeof(h.uid),   0);
    octal(h.gid,   sizeof(h.gid),   0);
    octal(h.size,  sizeof(h.size),  static_cast<std::uint64_t>(data.size()));
    octal(h.mtime, sizeof(h.mtime), 0);
    h.typeflag = '0';
    std::memcpy(h.magic,   "ustar", 5);
    std::memcpy(h.version, "00",    2);
    // Checksum: write spaces, sum, then write octal.
    std::memset(h.chksum, ' ', sizeof(h.chksum));
    auto s = tar_checksum(h);
    octal(h.chksum, sizeof(h.chksum) - 1, s);
    h.chksum[sizeof(h.chksum) - 1] = ' ';

    out.write(reinterpret_cast<const char*>(&h), sizeof(h));
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    if (auto rem = data.size() % kBlock; rem != 0) {
        char pad[kBlock] = {0};
        out.write(pad, static_cast<std::streamsize>(kBlock - rem));
    }
}

void write_eof(std::ostream& out) {
    char zero[kBlock * 2] = {0};
    out.write(zero, sizeof(zero));
}

std::string ledger_excerpt_jsonl(const Ledger& l, EvaluationWindow w) {
    auto entries = l.range(1, l.length() + 1);
    if (!entries) return {};
    std::string out;
    for (const auto& e : entries.value()) {
        if (e.header.ts < w.start || e.header.ts >= w.end) continue;
        nlohmann::json j;
        j["seq"]          = e.header.seq;
        j["ts"]           = e.header.ts.iso8601();
        j["actor"]        = e.header.actor;
        j["event_type"]   = e.header.event_type;
        j["tenant"]       = e.header.tenant;
        j["body"]         = nlohmann::json::parse(e.body_json,
                                                  /*cb=*/nullptr,
                                                  /*throw=*/false,
                                                  /*ignore_comments=*/true);
        j["payload_hash"] = e.header.payload_hash.hex();
        j["prev_hash"]    = e.header.prev_hash.hex();
        out += j.dump();
        out += '\n';
    }
    return out;
}

}  // namespace

Result<EvidenceBundle> EvaluationHarness::export_bundle(
    EvaluationWindow      window,
    std::filesystem::path out_path) const {

    auto excerpt = ledger_excerpt_jsonl(ledger_, window);
    auto excerpt_hash = hash(std::string_view{excerpt});

    auto m = metrics(window);
    if (!m) return m.error();

    nlohmann::json metrics_j = nlohmann::json::array();
    for (const auto& mm : m.value()) {
        nlohmann::json one;
        one["model"]         = mm.model;
        one["n_inferences"]  = mm.n_inferences;
        one["n_overrides"]   = mm.n_overrides;
        one["n_blocked"]     = mm.n_blocked;
        one["n_with_truth"]  = mm.n_with_truth;
        one["accuracy"]      = mm.accuracy;
        one["sensitivity"]   = mm.sensitivity;
        one["specificity"]   = mm.specificity;
        one["override_rate"] = mm.override_rate;
        metrics_j.push_back(std::move(one));
    }
    std::string metrics_text = metrics_j.dump(2);
    auto metrics_hash = hash(std::string_view{metrics_text});

    nlohmann::json drift_j = nlohmann::json::array();
    for (const auto& r : drift_.report()) {
        nlohmann::json one;
        one["feature"]      = r.feature;
        one["psi"]          = r.psi;
        one["ks_statistic"] = r.ks_statistic;
        one["emd"]          = r.emd;
        one["severity"]     = to_string(r.severity);
        one["reference_n"]  = r.reference_n;
        one["current_n"]    = r.current_n;
        one["computed_at"]  = r.computed_at.iso8601();
        drift_j.push_back(std::move(one));
    }
    std::string drift_text = drift_j.dump(2);
    auto drift_hash = hash(std::string_view{drift_text});

    nlohmann::json overrides_j = nlohmann::json::array();
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& ev : overrides_) {
            nlohmann::json one;
            one["inference_id"] = ev.inference_id;
            one["rationale"]    = ev.rationale;
            one["corrected"]    = ev.corrected;
            one["clinician"]    = std::string(ev.clinician.str());
            one["occurred_at"]  = ev.occurred_at.iso8601();
            overrides_j.push_back(std::move(one));
        }
    }
    std::string overrides_text = overrides_j.dump(2);
    auto overrides_hash = hash(std::string_view{overrides_text});

    nlohmann::json gt_j = nlohmann::json::array();
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& [_, t] : ground_truth_) {
            nlohmann::json one;
            one["inference_id"] = t.inference_id;
            one["truth"]        = t.truth;
            one["source"]       = t.source;
            one["captured_at"]  = t.captured_at.iso8601();
            gt_j.push_back(std::move(one));
        }
    }
    std::string gt_text = gt_j.dump(2);
    auto gt_hash = hash(std::string_view{gt_text});

    nlohmann::json manifest;
    manifest["asclepius_version"] = ASCLEPIUS_VERSION_STRING;
    manifest["window"]            = {
        {"start", window.start.iso8601()},
        {"end",   window.end.iso8601()},
    };
    manifest["files"] = {
        {"ledger_excerpt.jsonl", excerpt_hash.hex()},
        {"metrics.json",         metrics_hash.hex()},
        {"drift.json",           drift_hash.hex()},
        {"overrides.jsonl",      overrides_hash.hex()},
        {"ground_truth.jsonl",   gt_hash.hex()},
    };
    manifest["ledger_head"] = ledger_.head().hex();
    manifest["key_id"]      = ledger_.key_id();
    manifest["exported_at"] = Time::now().iso8601();

    std::string manifest_text = manifest.dump(2);
    auto manifest_hash = hash(std::string_view{manifest_text});

    // The signature is over the manifest, by the same key that signs ledger
    // entries. A verifier holding only the public key can confirm the
    // manifest, then re-hash each contained file from the bundle and check
    // it matches what the manifest declares.
    auto sig = ledger_.sign_attestation(Bytes{
        reinterpret_cast<const std::uint8_t*>(manifest_text.data()),
        manifest_text.size()});
    auto pk  = ledger_.public_key();

    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) return Error::backend("cannot open evidence bundle for write");

    auto write = [&](std::string_view name, std::string_view text) {
        write_file_entry(out, name,
                         Bytes{reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
    };

    write("manifest.json",         manifest_text);
    write("ledger_excerpt.jsonl",  excerpt);
    write("metrics.json",          metrics_text);
    write("drift.json",            drift_text);
    write("overrides.jsonl",       overrides_text);
    write("ground_truth.jsonl",    gt_text);
    write_file_entry(out, "manifest.sig",
                     Bytes{sig.data(), sig.size()});
    write_file_entry(out, "public_key",
                     Bytes{pk.data(),  pk.size()});

    write_eof(out);
    out.close();

    EvidenceBundle b;
    b.path        = out_path;
    b.root_hash   = manifest_hash;
    b.exported_at = Time::now();
    b.window      = window;
    b.per_model   = std::move(m).value();
    return b;
}

Result<bool> EvaluationHarness::verify_bundle(std::filesystem::path bundle_path) {
    // Minimal verifier: extract manifest.json, manifest.sig, public_key,
    // confirm the signature; then re-hash each declared file and check
    // against the manifest.
    std::ifstream in(bundle_path, std::ios::binary);
    if (!in) return Error::backend("cannot open evidence bundle");

    std::unordered_map<std::string, std::string> files;
    while (in) {
        TarHeader h{};
        in.read(reinterpret_cast<char*>(&h), sizeof(h));
        if (in.gcount() != sizeof(h)) break;
        if (h.name[0] == 0) break;  // EOF marker

        std::uint64_t size = 0;
        for (std::size_t i = 0; i < sizeof(h.size) && h.size[i]; ++i) {
            if (h.size[i] >= '0' && h.size[i] <= '7') {
                size = size * 8 + static_cast<std::uint64_t>(h.size[i] - '0');
            }
        }
        std::string body(size, '\0');
        in.read(body.data(), static_cast<std::streamsize>(size));
        if (auto rem = size % kBlock; rem != 0) {
            char pad[kBlock];
            in.read(pad, static_cast<std::streamsize>(kBlock - rem));
        }
        std::string name{h.name};
        files[name] = std::move(body);
    }

    auto get = [&](const std::string& k) -> std::string* {
        auto it = files.find(k);
        return it == files.end() ? nullptr : &it->second;
    };

    auto* manifest_text = get("manifest.json");
    auto* sig_blob      = get("manifest.sig");
    auto* pk_blob       = get("public_key");
    if (!manifest_text || !sig_blob || !pk_blob) {
        return Error::integrity("bundle missing manifest, signature, or public key");
    }
    if (sig_blob->size() != KeyStore::sig_bytes ||
        pk_blob->size()  != KeyStore::pk_bytes) {
        return Error::integrity("malformed signature or public key");
    }

    std::span<const std::uint8_t, KeyStore::sig_bytes> sig_span{
        reinterpret_cast<const std::uint8_t*>(sig_blob->data()), KeyStore::sig_bytes};
    std::span<const std::uint8_t, KeyStore::pk_bytes>  pk_span{
        reinterpret_cast<const std::uint8_t*>(pk_blob->data()),  KeyStore::pk_bytes};

    if (!KeyStore::verify(
            Bytes{reinterpret_cast<const std::uint8_t*>(manifest_text->data()),
                  manifest_text->size()},
            sig_span, pk_span)) {
        return Error::integrity("manifest signature verification failed");
    }

    nlohmann::json manifest = nlohmann::json::parse(*manifest_text);
    if (manifest.contains("files") && manifest["files"].is_object()) {
        for (auto it = manifest["files"].begin(); it != manifest["files"].end(); ++it) {
            auto* body = get(it.key());
            if (!body) {
                return Error::integrity(fmt::format("bundle missing {}", it.key()));
            }
            auto h = hash(std::string_view{*body});
            if (h.hex() != it.value().get<std::string>()) {
                return Error::integrity(fmt::format("hash mismatch for {}", it.key()));
            }
        }
    }
    return true;
}

}  // namespace asclepius
