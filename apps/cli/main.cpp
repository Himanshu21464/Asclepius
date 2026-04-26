// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
//
// `asclepius` — CLI for inspecting, verifying, and exporting from a ledger.

#include <CLI/CLI.hpp>
#include <fmt/core.h>
#include <fmt/ostream.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "asclepius/asclepius.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>

using namespace asclepius;

namespace {

int cmd_ledger_verify(const std::string& db) {
    auto led = Ledger::open_uri(db);
    if (!led) {
        fmt::print(stderr, "open: {}\n", led.error().what());
        return 2;
    }
    auto v = led.value().verify();
    if (!v) {
        fmt::print(stderr, "INVALID: {} ({})\n",
                   v.error().what(), to_string(v.error().code()));
        return 1;
    }
    fmt::print("OK: {} entries, head={}, key_id={}\n",
               led.value().length(),
               led.value().head().hex(),
               led.value().key_id());
    return 0;
}

int cmd_ledger_stats(const std::string& db) {
    auto led = Ledger::open_uri(db);
    if (!led) {
        fmt::print(stderr, "open: {}\n", led.error().what());
        return 2;
    }
    auto s = led.value().stats();
    if (!s) {
        fmt::print(stderr, "stats: {}\n", s.error().what());
        return 2;
    }
    std::cout << s.value().to_json() << '\n';
    return 0;
}

int cmd_ledger_stats_tenant(const std::string& db, const std::string& tenant) {
    auto led = Ledger::open_uri(db);
    if (!led) {
        fmt::print(stderr, "open: {}\n", led.error().what());
        return 2;
    }
    auto s = led.value().stats_for_tenant(tenant);
    if (!s) {
        fmt::print(stderr, "stats-tenant: {}\n", s.error().what());
        return 2;
    }
    std::cout << s.value().to_json() << '\n';
    return 0;
}

int cmd_ledger_tail_actor(const std::string& db,
                          const std::string& actor,
                          std::size_t        n) {
    if (actor.empty()) {
        fmt::print(stderr, "tail-actor: actor is required\n");
        return 2;
    }
    auto led = Ledger::open_uri(db);
    if (!led) {
        fmt::print(stderr, "open: {}\n", led.error().what());
        return 2;
    }
    auto entries = led.value().tail_by_actor(actor, n);
    if (!entries) {
        fmt::print(stderr, "tail-actor: {}\n", entries.error().what());
        return 2;
    }
    for (const auto& e : entries.value()) {
        nlohmann::json out;
        out["seq"]        = e.header.seq;
        out["ts"]         = e.header.ts.iso8601();
        out["actor"]      = e.header.actor;
        out["event_type"] = e.header.event_type;
        out["tenant"]     = e.header.tenant;
        out["body"]       = nlohmann::json::parse(e.body_json,
                                                   nullptr,
                                                   false);
        out["entry_hash"] = e.entry_hash().hex();
        std::cout << out.dump() << '\n';
    }
    return 0;
}

int cmd_ledger_event_counts(const std::string& db) {
    auto led = Ledger::open_uri(db);
    if (!led) {
        fmt::print(stderr, "open: {}\n", led.error().what());
        return 2;
    }
    auto c = led.value().count_by_event_type();
    if (!c) {
        fmt::print(stderr, "event-counts: {}\n", c.error().what());
        return 2;
    }
    nlohmann::json out = nlohmann::json::object();
    for (const auto& [k, v] : c.value()) out[k] = v;
    std::cout << out.dump() << '\n';
    return 0;
}

int cmd_ledger_find(const std::string& db, const std::string& inference_id) {
    if (inference_id.empty()) {
        fmt::print(stderr, "find: inference_id is required\n");
        return 2;
    }
    auto led = Ledger::open_uri(db);
    if (!led) {
        fmt::print(stderr, "open: {}\n", led.error().what());
        return 2;
    }
    auto e = led.value().find_by_inference_id(inference_id);
    if (!e) {
        if (e.error().code() == ErrorCode::not_found) {
            fmt::print(stderr, "not found: {}\n", inference_id);
            return 1;
        }
        fmt::print(stderr, "find: {}\n", e.error().what());
        return 2;
    }
    nlohmann::json out;
    out["seq"]        = e.value().header.seq;
    out["ts"]         = e.value().header.ts.iso8601();
    out["actor"]      = e.value().header.actor;
    out["event_type"] = e.value().header.event_type;
    out["tenant"]     = e.value().header.tenant;
    out["body"]       = nlohmann::json::parse(e.value().body_json,
                                              nullptr,
                                              false);
    out["entry_hash"] = e.value().entry_hash().hex();
    std::cout << out.dump(2) << '\n';
    return 0;
}

int cmd_ledger_inspect(const std::string& db, std::size_t tail) {
    auto led = Ledger::open_uri(db);
    if (!led) {
        fmt::print(stderr, "open: {}\n", led.error().what());
        return 2;
    }
    auto entries = led.value().tail(tail);
    if (!entries) {
        fmt::print(stderr, "tail: {}\n", entries.error().what());
        return 2;
    }
    for (const auto& e : entries.value()) {
        nlohmann::json out;
        out["seq"]        = e.header.seq;
        out["ts"]         = e.header.ts.iso8601();
        out["actor"]      = e.header.actor;
        out["event_type"] = e.header.event_type;
        out["tenant"]     = e.header.tenant;
        out["body"]       = nlohmann::json::parse(e.body_json,
                                                   /*cb=*/nullptr,
                                                   /*throw=*/false);
        out["entry_hash"] = e.entry_hash().hex();
        std::cout << out.dump() << '\n';
    }
    return 0;
}

int cmd_ledger_migrate(const std::string& src_uri, const std::string& dst_uri) {
    if (src_uri.compare(0, 11, "postgres://") == 0
     || src_uri.compare(0, 13, "postgresql://") == 0) {
        fmt::print(stderr, "migrate: postgres-as-source not yet supported by CLI; "
                           "use the LedgerMigrator API directly with an explicit KeyStore\n");
        return 2;
    }

    // SQLite source: read the .key file beside the path.
    std::filesystem::path src_path{src_uri};
    auto src_key_path = src_path; src_key_path.replace_extension(".key");
    if (!std::filesystem::exists(src_key_path)) {
        fmt::print(stderr, "migrate: source key {} not found\n", src_key_path.string());
        return 2;
    }
    std::ifstream kf(src_key_path);
    std::string blob((std::istreambuf_iterator<char>(kf)), {});
    auto key = KeyStore::deserialize(blob);
    if (!key) { fmt::print(stderr, "migrate: bad key: {}\n", key.error().what()); return 2; }

    auto stats = LedgerMigrator::copy(src_uri, dst_uri, std::move(key.value()));
    if (!stats) { fmt::print(stderr, "migrate: {}\n", stats.error().what()); return 2; }

    // Copy the source's signing key to the destination's expected key path
    // so the operator can immediately verify the destination without
    // shuffling keys by hand. Same logic as Ledger::open_uri:
    //   postgres URI → ./<dbname>.key
    //   sqlite path  → <path>.key
    std::filesystem::path dst_key_path;
    if (dst_uri.compare(0, 11, "postgres://") == 0
     || dst_uri.compare(0, 13, "postgresql://") == 0) {
        std::string db = "asclepius";
        auto last_slash = dst_uri.rfind('/');
        if (last_slash != std::string::npos && last_slash + 1 < dst_uri.size()) {
            auto q = dst_uri.find('?', last_slash + 1);
            db = dst_uri.substr(last_slash + 1,
                                (q == std::string::npos ? dst_uri.size() : q) - last_slash - 1);
            if (db.empty()) db = "asclepius";
        }
        dst_key_path = db + ".key";
    } else {
        dst_key_path = std::filesystem::path{dst_uri};
        dst_key_path.replace_extension(".key");
    }
    std::filesystem::copy_file(src_key_path, dst_key_path,
                               std::filesystem::copy_options::overwrite_existing);

    fmt::print("OK: copied {} entries · src_head={} · dst_head={}\n"
               "    src_key {} -> dst_key {}\n",
               stats.value().entries_copied,
               stats.value().source_head.hex(),
               stats.value().dest_head.hex(),
               src_key_path.string(), dst_key_path.string());
    return 0;
}

int cmd_ledger_export_jsonl(const std::string& src_uri, const std::filesystem::path& out_path) {
    auto stats = LedgerJsonl::export_to(src_uri, out_path.string());
    if (!stats) { fmt::print(stderr, "export-jsonl: {}\n", stats.error().what()); return 2; }
    fmt::print("OK: wrote {} entries to {} · last_hash={}\n",
               stats.value().entries_written, out_path.string(),
               stats.value().last_entry_hash.hex());
    return 0;
}

int cmd_ledger_import_jsonl(const std::filesystem::path& in_path,
                            const std::string& dst_uri,
                            const std::filesystem::path& key_path) {
    if (!std::filesystem::exists(key_path)) {
        fmt::print(stderr, "import-jsonl: key file {} not found\n", key_path.string());
        return 2;
    }
    std::ifstream kf(key_path);
    std::string blob((std::istreambuf_iterator<char>(kf)), {});
    auto key = KeyStore::deserialize(blob);
    if (!key) { fmt::print(stderr, "import-jsonl: bad key: {}\n", key.error().what()); return 2; }

    auto stats = LedgerJsonl::import_to(in_path.string(), dst_uri, std::move(key.value()));
    if (!stats) { fmt::print(stderr, "import-jsonl: {}\n", stats.error().what()); return 2; }

    // Copy the signing key to the destination's expected key location so
    // subsequent `ledger verify` against the destination uses the same
    // public key the imported entries were signed with.
    std::filesystem::path dst_key_path;
    if (dst_uri.compare(0, 11, "postgres://") == 0
     || dst_uri.compare(0, 13, "postgresql://") == 0) {
        std::string db = "asclepius";
        auto last_slash = dst_uri.rfind('/');
        if (last_slash != std::string::npos && last_slash + 1 < dst_uri.size()) {
            auto q = dst_uri.find('?', last_slash + 1);
            db = dst_uri.substr(last_slash + 1,
                                (q == std::string::npos ? dst_uri.size() : q) - last_slash - 1);
            if (db.empty()) db = "asclepius";
        }
        dst_key_path = db + ".key";
    } else {
        dst_key_path = std::filesystem::path{dst_uri};
        dst_key_path.replace_extension(".key");
    }
    std::filesystem::copy_file(key_path, dst_key_path,
                               std::filesystem::copy_options::overwrite_existing);

    fmt::print("OK: imported {} entries · dst_head={}\n"
               "    src_key {} -> dst_key {}\n",
               stats.value().entries_imported, stats.value().dest_head.hex(),
               key_path.string(), dst_key_path.string());
    return 0;
}

int cmd_ledger_checkpoint(const std::string& db) {
    auto led = Ledger::open_uri(db);
    if (!led) { fmt::print(stderr, "open: {}\n", led.error().what()); return 2; }
    auto cp = led.value().checkpoint();
    fmt::print("{}\n", cp.to_json());
    return 0;
}

int cmd_ledger_verify_checkpoint(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) { fmt::print(stderr, "open: {}\n", path.string()); return 2; }
    std::string blob((std::istreambuf_iterator<char>(in)), {});
    auto cp = LedgerCheckpoint::from_json(blob);
    if (!cp) { fmt::print(stderr, "parse: {}\n", cp.error().what()); return 2; }
    auto v = verify_checkpoint(cp.value());
    if (!v) {
        fmt::print(stderr, "INVALID: {} ({})\n",
                   v.error().what(), to_string(v.error().code()));
        return 1;
    }
    fmt::print("OK: seq={} head={} ts={} key_id={}\n",
               cp.value().seq, cp.value().head_hash.hex(),
               cp.value().ts.iso8601(), cp.value().key_id);
    return 0;
}

int cmd_metrics_export(const std::string& db) {
    // Open the ledger, walk it once to derive counter values from event
    // types, and emit Prometheus exposition format. Histograms (latency
    // distributions etc.) only exist for live runtimes; this is a static
    // export of what the chain itself contains, which is all an operator
    // can derive from a stopped process. For live histograms, scrape the
    // /metrics endpoint of an asclepius-svc instance.
    auto led = Ledger::open_uri(db);
    if (!led) { fmt::print(stderr, "open: {}\n", led.error().what()); return 2; }

    MetricRegistry m;
    auto r = led.value().subscribe([&](const LedgerEntry&) { /* unused for static walk */ });
    (void)r;

    auto all = led.value().range(1, led.value().length() + 1);
    if (!all) { fmt::print(stderr, "range: {}\n", all.error().what()); return 2; }
    for (const auto& e : all.value()) {
        m.inc("ledger.entries.total");
        m.inc("ledger.entries." + e.header.event_type);
        if (!e.header.tenant.empty()) m.inc("ledger.entries.tenant_scoped");
    }
    m.inc("ledger.length", led.value().length());
    std::cout << m.snapshot_prometheus();
    return 0;
}

int cmd_drift_report(const std::string& /*db*/) {
    // Drift state is in-process; reading drift from a long-lived sidecar is
    // a future enhancement. For now we emit an empty report so scripts have
    // something stable to consume.
    fmt::print("[]\n");
    return 0;
}

int cmd_evidence_bundle(const std::string&           db,
                        const std::filesystem::path& out,
                        const std::string&           since) {
    auto rt = Runtime::open_uri(db);
    if (!rt) { fmt::print(stderr, "open: {}\n", rt.error().what()); return 2; }

    // Parse the "since" duration (e.g. "30d", "24h"). Default 30 days.
    auto parse_dur = [](std::string_view s) -> std::chrono::nanoseconds {
        if (s.empty()) return std::chrono::hours{24 * 30};
        char unit = s.back();
        long long n = std::stoll(std::string{s.substr(0, s.size() - 1)});
        switch (unit) {
            case 'd': return std::chrono::hours{24 * n};
            case 'h': return std::chrono::hours{n};
            case 'm': return std::chrono::minutes{n};
            default:  return std::chrono::hours{24 * 30};
        }
    };
    auto window = EvaluationWindow{
        Time::now() - parse_dur(since),
        Time::now() + std::chrono::seconds{1},
    };

    auto b = rt.value().evaluation().export_bundle(window, out);
    if (!b) {
        fmt::print(stderr, "export: {}\n", b.error().what());
        return 2;
    }
    fmt::print("wrote {}, root_hash={}, models={}\n",
               b.value().path.string(),
               b.value().root_hash.hex(),
               b.value().per_model.size());
    return 0;
}

int cmd_evidence_verify(const std::filesystem::path& bundle) {
    auto v = EvaluationHarness::verify_bundle(bundle);
    if (!v) {
        fmt::print(stderr, "INVALID: {} ({})\n",
                   v.error().what(), to_string(v.error().code()));
        return 1;
    }
    fmt::print("OK\n");
    return 0;
}

int cmd_policy_lint(const std::filesystem::path& path) {
    // Stub: a future linter will validate a YAML/JSON policy spec against
    // the registered policy DSL. For now we emit a hint.
    fmt::print(stderr, "policy-lint is not yet implemented for {}\n", path.string());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

    CLI::App app{"asclepius — clinical-AI trust substrate CLI"};
    app.set_version_flag("--version", std::string{"asclepius "} + ASCLEPIUS_VERSION_STRING);
    app.require_subcommand(1);

    auto* ledger = app.add_subcommand("ledger", "ledger operations");
    ledger->require_subcommand(1);

    // db can be a SQLite filesystem path or a postgres:// URI; we don't apply
    // CLI::ExistingFile because postgres URIs aren't files.
    std::string db_uri;
    {
        auto* verify = ledger->add_subcommand("verify", "verify chain integrity");
        verify->add_option("db", db_uri,
                           "ledger: SQLite path or postgres://user:pass@host/dbname")
              ->required();
        verify->callback([&]() { std::exit(cmd_ledger_verify(db_uri)); });
    }
    std::size_t tail_n = 20;
    {
        auto* inspect = ledger->add_subcommand("inspect", "print recent entries as JSON");
        inspect->add_option("db", db_uri,
                            "ledger: SQLite path or postgres://...")
               ->required();
        inspect->add_option("--tail", tail_n, "number of entries (default 20)");
        inspect->callback([&]() { std::exit(cmd_ledger_inspect(db_uri, tail_n)); });
    }
    std::string mig_src, mig_dst;
    {
        auto* migrate = ledger->add_subcommand("migrate",
            "copy a chain from one backend to another (e.g. SQLite → Postgres)");
        migrate->add_option("src", mig_src, "source ledger URI (SQLite path)")->required();
        migrate->add_option("dst", mig_dst, "destination ledger URI")->required();
        migrate->callback([&]() { std::exit(cmd_ledger_migrate(mig_src, mig_dst)); });
    }
    {
        auto* metrics = ledger->add_subcommand("metrics",
            "emit Prometheus exposition derived from the chain");
        metrics->add_option("db", db_uri,
                            "ledger: SQLite path or postgres://...")->required();
        metrics->callback([&]() { std::exit(cmd_metrics_export(db_uri)); });
    }
    {
        auto* stats = ledger->add_subcommand("stats",
            "emit chain summary as JSON (count, head, oldest/newest, key id)");
        stats->add_option("db", db_uri,
                          "ledger: SQLite path or postgres://...")->required();
        stats->callback([&]() { std::exit(cmd_ledger_stats(db_uri)); });
    }
    std::string tenant_arg;
    {
        auto* st = ledger->add_subcommand("stats-tenant",
            "per-tenant chain summary as JSON");
        st->add_option("db", db_uri,
                       "ledger: SQLite path or postgres://...")->required();
        st->add_option("tenant", tenant_arg,
                       "tenant to scope to (use empty string for default scope)")
          ->required();
        st->callback([&]() { std::exit(cmd_ledger_stats_tenant(db_uri, tenant_arg)); });
    }
    {
        auto* ec = ledger->add_subcommand("event-counts",
            "emit {event_type: count} JSON map for the chain");
        ec->add_option("db", db_uri,
                       "ledger: SQLite path or postgres://...")->required();
        ec->callback([&]() { std::exit(cmd_ledger_event_counts(db_uri)); });
    }
    std::string actor_arg;
    std::size_t actor_n = 50;
    {
        auto* ta = ledger->add_subcommand("tail-actor",
            "print the last N entries by a given actor (most recent first)");
        ta->add_option("db", db_uri,
                       "ledger: SQLite path or postgres://...")->required();
        ta->add_option("actor", actor_arg,
                       "actor string to scope to "
                       "(e.g. 'system:drift_monitor')")->required();
        ta->add_option("--n", actor_n, "how many entries (default 50)");
        ta->callback([&]() {
            std::exit(cmd_ledger_tail_actor(db_uri, actor_arg, actor_n));
        });
    }
    std::string find_id;
    {
        auto* find = ledger->add_subcommand("find",
            "look up a single ledger entry by inference_id (incident response)");
        find->add_option("db", db_uri,
                         "ledger: SQLite path or postgres://...")->required();
        find->add_option("inference_id", find_id,
                         "the inference_id to locate")->required();
        find->callback([&]() { std::exit(cmd_ledger_find(db_uri, find_id)); });
    }
    std::filesystem::path jsonl_path;
    {
        auto* exp = ledger->add_subcommand("export-jsonl",
            "stream the chain as JSONL (one entry per line)");
        exp->add_option("src", db_uri, "ledger URI")->required();
        exp->add_option("out", jsonl_path, "output JSONL path")->required();
        exp->callback([&]() { std::exit(cmd_ledger_export_jsonl(db_uri, jsonl_path)); });
    }
    {
        auto* cp = ledger->add_subcommand("checkpoint",
            "emit a tiny signed attestation of the chain head (CT-style beacon)");
        cp->add_option("db", db_uri, "ledger URI")->required();
        cp->callback([&]() { std::exit(cmd_ledger_checkpoint(db_uri)); });
    }
    std::filesystem::path cp_path;
    {
        auto* vc = ledger->add_subcommand("verify-checkpoint",
            "verify a checkpoint signature (no ledger needed)");
        vc->add_option("checkpoint", cp_path, "checkpoint JSON file")
          ->required()->check(CLI::ExistingFile);
        vc->callback([&]() { std::exit(cmd_ledger_verify_checkpoint(cp_path)); });
    }
    std::filesystem::path key_path;
    {
        auto* imp = ledger->add_subcommand("import-jsonl",
            "ingest a JSONL stream into a fresh ledger (signatures verified)");
        imp->add_option("src",  jsonl_path, "input JSONL file")->required()->check(CLI::ExistingFile);
        imp->add_option("dst",  db_uri,     "destination ledger URI (must be empty)")->required();
        imp->add_option("--key", key_path,
                        "signing key (PEM-shaped); required to validate the import")->required();
        imp->callback([&]() { std::exit(cmd_ledger_import_jsonl(jsonl_path, db_uri, key_path)); });
    }

    auto* drift = app.add_subcommand("drift", "drift operations");
    drift->require_subcommand(1);
    {
        auto* report = drift->add_subcommand("report", "emit drift report (json)");
        report->add_option("db", db_uri)->required();
        report->callback([&]() { std::exit(cmd_drift_report(db_uri)); });
    }

    auto* evidence = app.add_subcommand("evidence", "evidence bundle ops");
    evidence->require_subcommand(1);
    std::filesystem::path bundle_out;
    std::string           since = "30d";
    {
        auto* bundle = evidence->add_subcommand("bundle", "export an evidence bundle");
        bundle->add_option("db", db_uri,
                           "ledger: SQLite path or postgres://...")
              ->required();
        bundle->add_option("--out", bundle_out, "output tar path")->required();
        bundle->add_option("--window", since, "lookback window (e.g. 30d, 24h)");
        bundle->callback([&]() { std::exit(cmd_evidence_bundle(db_uri, bundle_out, since)); });
    }
    {
        auto* verify = evidence->add_subcommand("verify", "verify a bundle file");
        verify->add_option("bundle", bundle_out)->required()->check(CLI::ExistingFile);
        verify->callback([&]() { std::exit(cmd_evidence_verify(bundle_out)); });
    }

    auto* policy = app.add_subcommand("policy", "policy operations");
    policy->require_subcommand(1);
    std::filesystem::path policy_file;
    {
        auto* lint = policy->add_subcommand("lint", "lint a policy spec");
        lint->add_option("file", policy_file)->required()->check(CLI::ExistingFile);
        lint->callback([&]() { std::exit(cmd_policy_lint(policy_file)); });
    }

    CLI11_PARSE(app, argc, argv);
    return 0;
}
