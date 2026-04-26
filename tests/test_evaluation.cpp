// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include <doctest/doctest.h>

#include "asclepius/asclepius.hpp"

#include <filesystem>
#include <fstream>
#include <random>

using namespace asclepius;
using namespace std::chrono_literals;

namespace {

std::filesystem::path tmp_db(const char* tag) {
    auto p = std::filesystem::temp_directory_path()
           / ("asclepius_ev_" + std::string{tag} + "_"
              + std::to_string(std::random_device{}()) + ".db");
    std::filesystem::remove(p);
    std::filesystem::remove(std::filesystem::path{p}.replace_extension(".key"));
    return p;
}

std::filesystem::path tmp_bundle(const char* tag) {
    return std::filesystem::temp_directory_path()
         / ("asclepius_bundle_" + std::string{tag} + "_"
            + std::to_string(std::random_device{}()) + ".tar");
}

}  // namespace

TEST_CASE("Evidence bundle exports and verifies") {
    auto rt = Runtime::open(tmp_db("bundle")).value();

    auto pid = PatientId::pseudonymous("p_bundle");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h);
    REQUIRE(tok);

    Time started = Time::now();

    for (int i = 0; i < 5; ++i) {
        auto inf = rt.begin_inference({
            .model            = ModelId{"scribe", "v3"},
            .actor            = ActorId::clinician("smith"),
            .patient          = pid,
            .encounter        = EncounterId::make(),
            .purpose          = Purpose::ambient_documentation,
            .tenant           = TenantId{},
            .consent_token_id = tok.value().token_id,
        });
        REQUIRE(inf);
        REQUIRE(inf.value().run("hello world",
                                [](std::string s) -> Result<std::string> { return s; }));
        REQUIRE(inf.value().commit());
    }

    auto out = tmp_bundle("ok");
    auto bundle = rt.evaluation().export_bundle(
        EvaluationWindow{started - std::chrono::hours{1}, Time::now() + std::chrono::hours{1}},
        out);
    REQUIRE(bundle);

    CHECK(std::filesystem::exists(out));
    CHECK(std::filesystem::file_size(out) > 0);

    auto v = EvaluationHarness::verify_bundle(out);
    REQUIRE(v);
    CHECK(v.value());
}

TEST_CASE("Tampered bundle fails verification") {
    auto rt = Runtime::open(tmp_db("tampered_bundle")).value();
    auto pid = PatientId::pseudonymous("p_t");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h);
    REQUIRE(tok);

    auto inf = rt.begin_inference({
        .model            = ModelId{"scribe", "v3"},
        .actor            = ActorId::clinician("smith"),
        .patient          = pid,
        .encounter        = EncounterId::make(),
        .purpose          = Purpose::ambient_documentation,
        .tenant           = TenantId{},
        .consent_token_id = tok.value().token_id,
    });
    REQUIRE(inf);
    REQUIRE(inf.value().run("hi", [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(inf.value().commit());

    auto path = tmp_bundle("tamp");
    auto b = rt.evaluation().export_bundle(
        EvaluationWindow{Time::now() - std::chrono::hours{1}, Time::now() + std::chrono::hours{1}},
        path);
    REQUIRE(b);

    // Mutate one byte inside the manifest's data-file region (between the
    // 512-byte manifest header and the trailing manifest.sig + public_key
    // entries). Anywhere in the middle of a content block invalidates the
    // file's hash declared in manifest.json.
    std::fstream f{path, std::ios::in | std::ios::out | std::ios::binary};
    REQUIRE(f);
    f.seekg(0, std::ios::end);
    auto sz = f.tellg();
    REQUIRE(sz > 4096);
    // Seek to ~25% in — that lands well inside the manifest's data files.
    auto target = static_cast<std::streamoff>(sz) / 4;
    f.seekg(target);
    char c = 0;
    f.read(&c, 1);
    c ^= static_cast<char>(0x55);
    f.seekp(target);
    f.write(&c, 1);
    f.close();

    auto v = EvaluationHarness::verify_bundle(path);
    CHECK(!v);
}
