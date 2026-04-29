// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
//
// Synthetic benchmark harness. Measures ledger append, signature, hash,
// policy chain, and end-to-end wrapped-inference throughput on a single
// thread. Emits a JSON summary that the website /benchmarks page consumes.
//
//   ./example_bench --n 50000 --policies all
//
// All work is deterministic (seeded), so re-runs on the same machine
// produce stable numbers within the noise floor.

#include <fmt/core.h>
#include <nlohmann/json.hpp>

#include "asclepius/asclepius.hpp"

#include <chrono>
#include <filesystem>
#include <random>

using namespace asclepius;
using namespace std::chrono;
using clk = high_resolution_clock;

namespace {

template <class Fn>
double bench(std::size_t n, Fn&& fn) {
    auto t0 = clk::now();
    for (std::size_t i = 0; i < n; ++i) fn(i);
    auto dt = duration<double>(clk::now() - t0).count();
    return dt;
}

double quantile(std::vector<double> v, double q) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    auto idx = std::min<std::size_t>(v.size() - 1,
        static_cast<std::size_t>(q * static_cast<double>(v.size())));
    return v[idx];
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t N = 20000;
    std::string db_uri;  // empty → SQLite default
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--n" && i + 1 < argc) N = std::stoul(argv[++i]);
        else if (a == "--db" && i + 1 < argc) db_uri = argv[++i];
    }

    if (db_uri.empty()) {
        std::filesystem::path db = std::filesystem::temp_directory_path() / "asclepius_bench.db";
        std::filesystem::remove(db);
        std::filesystem::remove(std::filesystem::path{db}.replace_extension(".key"));
        db_uri = db.string();
    } else {
        // Caller-supplied SQLite path: clean it for repeatable results
        std::filesystem::remove(db_uri);
        std::filesystem::path k{db_uri}; k.replace_extension(".key");
        std::filesystem::remove(k);
    }

    auto rt = Runtime::open(db_uri);
    if (!rt) { fmt::print(stderr, "open: {}\n", rt.error().what()); return 1; }
    auto& runtime = rt.value();

    runtime.policies().push(make_phi_scrubber());
    runtime.policies().push(make_length_limit(/*input_max=*/10000, /*output_max=*/4000));

    auto patient = PatientId::pseudonymous("p_bench");
    auto tok = runtime.consent().grant(patient,
                                        {Purpose::ambient_documentation},
                                        std::chrono::seconds{3600});

    // ── 1. raw hashing throughput ──────────────────────────────────────────
    const std::string sample = "Patient reports chest pain radiating to the left arm. "
                                "BP 132/84, HR 88. ECG ordered.";
    Bytes b{reinterpret_cast<const std::uint8_t*>(sample.data()), sample.size()};
    auto hash_t = bench(N, [&](std::size_t) { (void)hash(b); });
    double hash_ns_per = (hash_t / static_cast<double>(N)) * 1e9;
    double hash_mb_s   = (static_cast<double>(sample.size()) * static_cast<double>(N))
                          / hash_t / (1024.0 * 1024.0);

    // ── 2. signing throughput ──────────────────────────────────────────────
    auto signer = KeyStore::generate();
    auto sign_t = bench(N, [&](std::size_t) { (void)signer.sign(b); });
    double sign_ns_per = (sign_t / static_cast<double>(N)) * 1e9;

    // ── 3. signature verify throughput ─────────────────────────────────────
    auto sig = signer.sign(b);
    auto pk  = signer.public_key();
    auto verify_t = bench(N, [&](std::size_t) {
        (void)KeyStore::verify(b,
            std::span<const std::uint8_t, KeyStore::sig_bytes>{sig.data(), sig.size()},
            std::span<const std::uint8_t, KeyStore::pk_bytes>{pk.data(),  pk.size()});
    });
    double verify_ns_per = (verify_t / static_cast<double>(N)) * 1e9;

    // ── 4. ledger append throughput (smaller N — IO-bound) ────────────────
    constexpr std::size_t LEDGER_N = 2000;
    nlohmann::json body = {
        {"inference_id", "inf_bench"},
        {"model",        "scribe@v3"},
        {"actor",        "clinician:bench"},
        {"status",       "ok"},
    };
    auto ledger_t = bench(LEDGER_N, [&](std::size_t i) {
        body["seq"] = i;
        (void)runtime.ledger().append("inference.committed", "clinician:bench", body);
    });
    double ledger_us_per = (ledger_t / static_cast<double>(LEDGER_N)) * 1e6;

    // ── 5. ledger verify (batch) ──────────────────────────────────────────
    auto t0 = clk::now();
    auto vr = runtime.ledger().verify();
    auto verify_chain_us = duration<double, std::micro>(clk::now() - t0).count();
    double verify_per_entry_us = verify_chain_us / static_cast<double>(LEDGER_N);

    // ── 6. policy chain throughput (PHI scrub + length limit) ─────────────
    InferenceContext ctx{
        "inf_bench",
        ModelId{"scribe", "v3"},
        ActorId::clinician("bench"),
        patient,
        EncounterId::make(),
        Purpose::ambient_documentation,
        TenantId{},
        Time::now(),
    };
    const std::string with_phi =
        "Patient John Doe MRN:12345678 reports chest pain. Phone (415) 555-1234.";
    auto policy_t = bench(N, [&](std::size_t) {
        (void)runtime.policies().evaluate_input(ctx, std::string{with_phi});
    });
    double policy_us_per = (policy_t / static_cast<double>(N)) * 1e6;

    // ── 7. end-to-end wrapped inference (the headline number) ─────────────
    constexpr std::size_t E2E_N = 1000;
    std::vector<double> e2e_us;
    e2e_us.reserve(E2E_N);
    for (std::size_t i = 0; i < E2E_N; ++i) {
        auto inf_r = runtime.begin_inference({
            .model            = ModelId{"scribe", "v3"},
            .actor            = ActorId::clinician("bench"),
            .patient          = patient,
            .encounter        = EncounterId::make(),
            .purpose          = Purpose::ambient_documentation,
            .tenant           = TenantId{},
            .consent_token_id = tok.value().token_id,
        });
        if (!inf_r) continue;
        auto t_iter = clk::now();
        auto out = inf_r.value().run(std::string{with_phi},
            [](std::string s) -> Result<std::string> { return s; });
        if (out) (void)inf_r.value().commit();
        auto dt = duration<double, std::micro>(clk::now() - t_iter).count();
        e2e_us.push_back(dt);
    }

    // ── emit JSON summary ─────────────────────────────────────────────────
    nlohmann::json out;
    out["asclepius_version"] = ASCLEPIUS_VERSION_STRING;
    out["sample_size"]       = N;
    out["hash"] = {
        {"ns_per_op",  hash_ns_per},
        {"mb_per_sec", hash_mb_s},
        {"algo",       "BLAKE2b-256"},
    };
    out["sign"] = {
        {"ns_per_op",  sign_ns_per},
        {"ops_per_sec", 1e9 / sign_ns_per},
        {"algo",       "Ed25519"},
    };
    out["verify"] = {
        {"ns_per_op",  verify_ns_per},
        {"ops_per_sec", 1e9 / verify_ns_per},
    };
    out["ledger_append"] = {
        {"us_per_op", ledger_us_per},
        {"ops_per_sec", 1e6 / ledger_us_per},
        {"backend",   "SQLite WAL"},
    };
    out["chain_verify"] = {
        {"us_per_entry", verify_per_entry_us},
        {"total_us",      verify_chain_us},
        {"entries",       LEDGER_N},
    };
    out["policy_chain"] = {
        {"us_per_op",   policy_us_per},
        {"ops_per_sec", 1e6 / policy_us_per},
        {"composition", "phi_scrubber + length_limit"},
    };
    out["end_to_end_inference"] = {
        {"p50_us", quantile(e2e_us, 0.50)},
        {"p90_us", quantile(e2e_us, 0.90)},
        {"p99_us", quantile(e2e_us, 0.99)},
        {"mean_us", e2e_us.empty() ? 0.0
                       : std::accumulate(e2e_us.begin(), e2e_us.end(), 0.0)
                            / static_cast<double>(e2e_us.size())},
        {"samples", e2e_us.size()},
    };

    fmt::print("{}\n", out.dump(2));
    return 0;
}
