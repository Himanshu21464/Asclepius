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

int cmd_ledger_verify(const std::filesystem::path& db) {
    auto led = Ledger::open(db);
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

int cmd_ledger_inspect(const std::filesystem::path& db, std::size_t tail) {
    auto led = Ledger::open(db);
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

int cmd_drift_report(const std::filesystem::path& /*db*/) {
    // Drift state is in-process; reading drift from a long-lived sidecar is
    // a future enhancement. For now we emit an empty report so scripts have
    // something stable to consume.
    fmt::print("[]\n");
    return 0;
}

int cmd_evidence_bundle(const std::filesystem::path& db,
                        const std::filesystem::path& out,
                        const std::string&           since) {
    auto rt = Runtime::open(db);
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

    std::filesystem::path db_path;
    {
        auto* verify = ledger->add_subcommand("verify", "verify chain integrity");
        verify->add_option("db", db_path, "ledger database file")
              ->required()->check(CLI::ExistingFile);
        verify->callback([&]() { std::exit(cmd_ledger_verify(db_path)); });
    }
    std::size_t tail_n = 20;
    {
        auto* inspect = ledger->add_subcommand("inspect", "print recent entries as JSON");
        inspect->add_option("db", db_path)->required()->check(CLI::ExistingFile);
        inspect->add_option("--tail", tail_n, "number of entries (default 20)");
        inspect->callback([&]() { std::exit(cmd_ledger_inspect(db_path, tail_n)); });
    }

    auto* drift = app.add_subcommand("drift", "drift operations");
    drift->require_subcommand(1);
    {
        auto* report = drift->add_subcommand("report", "emit drift report (json)");
        report->add_option("db", db_path)->required()->check(CLI::ExistingFile);
        report->callback([&]() { std::exit(cmd_drift_report(db_path)); });
    }

    auto* evidence = app.add_subcommand("evidence", "evidence bundle ops");
    evidence->require_subcommand(1);
    std::filesystem::path bundle_out;
    std::string           since = "30d";
    {
        auto* bundle = evidence->add_subcommand("bundle", "export an evidence bundle");
        bundle->add_option("db", db_path)->required()->check(CLI::ExistingFile);
        bundle->add_option("--out", bundle_out, "output tar path")->required();
        bundle->add_option("--window", since, "lookback window (e.g. 30d, 24h)");
        bundle->callback([&]() { std::exit(cmd_evidence_bundle(db_path, bundle_out, since)); });
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
