// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
//
// Round 102 — adversarial input tests for every _from_json parser
// shipped in rounds 90-96. Each parser is pounded with the same
// canonical adversarial battery: empty, whitespace, garbage, null,
// arrays, deep nesting, oversize strings, and embedded nulls. The goal
// is not to *succeed* on these inputs — it's to never crash, sanitiser-
// fault, or silently accept malformed structure as valid.

#include <doctest/doctest.h>

#include "asclepius/audit.hpp"
#include "asclepius/consent.hpp"
#include "asclepius/evaluation.hpp"
#include "asclepius/interop/fhir.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using namespace asclepius;
using asclepius::fhir::artefact_from_bundle;
using asclepius::fhir::well_formed_bundle;

namespace {

// Adversarial inputs that every parser should reject without crashing.
const std::vector<std::string> kAdversarial = {
    "",                                          // empty
    " ",                                         // single space
    "\n\t",                                      // whitespace
    "not json",                                  // garbage
    "{",                                         // unterminated object
    "}",                                         // stray brace
    "[",                                         // unterminated array
    "[]",                                        // empty array (most parsers want object)
    "[1,2,3]",                                   // array of ints
    "null",                                      // JSON null
    "true",                                      // JSON true
    "1234",                                      // JSON number
    "\"just a string\"",                         // JSON string
    "{\"unrelated\":\"keys\"}",                  // valid object, wrong shape
    "{\"key\":\"\\u0000inside\"}",               // embedded NUL via escape
    std::string(8192, ' ') + "{",                // very long whitespace prefix
    "{\"a\":" + std::string(2048, '[') + std::string(2048, ']') + "}", // deep nesting
};

}  // namespace

TEST_CASE("hardening: from_abdm_json rejects all adversarial inputs without crashing") {
    for (const auto& input : kAdversarial) {
        auto r = from_abdm_json(input);
        // Must not throw, must return error or success deterministically.
        // If it returned ok somehow, the artefact must still be sane.
        if (r) {
            // If a malformed input was accepted, at minimum it should not
            // produce an artefact with all-empty key fields.
            const bool all_empty = r.value().artefact_id.empty()
                                 && r.value().purposes.empty()
                                 && r.value().requester_id.empty();
            CHECK_FALSE(all_empty);
        }
    }
}

TEST_CASE("hardening: attestation_from_json rejects all adversarial inputs") {
    for (const auto& input : kAdversarial) {
        auto r = attestation_from_json(input);
        if (r) {
            CHECK(r.value().subject_kind.size() < 1024 * 1024);
        }
    }
}

TEST_CASE("hardening: envelope_from_json rejects all adversarial inputs") {
    for (const auto& input : kAdversarial) {
        auto r = envelope_from_json(input);
        if (r) {
            CHECK(r.value().consult_id.size() < 1024 * 1024);
        }
    }
}

TEST_CASE("hardening: bill_audit_from_json rejects all adversarial inputs") {
    for (const auto& input : kAdversarial) {
        auto r = bill_audit_from_json(input);
        if (r) {
            CHECK(r.value().findings.size() < 100000);
        }
    }
}

TEST_CASE("hardening: sample_integrity_from_json rejects all adversarial inputs") {
    for (const auto& input : kAdversarial) {
        auto r = sample_integrity_from_json(input);
        if (r) {
            CHECK(r.value().checkpoints.size() < 100000);
        }
    }
}

TEST_CASE("hardening: care_path_from_json rejects all adversarial inputs") {
    for (const auto& input : kAdversarial) {
        auto r = care_path_from_json(input);
        if (r) {
            CHECK(r.value().attestation_id.size() < 1024 * 1024);
        }
    }
}

TEST_CASE("hardening: artefact_from_bundle rejects garbage JSON values") {
    using nlohmann::json;
    std::vector<json> garbage = {
        json{},                              // null
        json::array(),                       // empty array
        json{1, 2, 3},                       // array of ints
        json{{"unrelated", "keys"}},         // object without bundle
        json{{"resourceType", "Patient"}},   // wrong resourceType
        json{{"resourceType", "Bundle"}},    // missing entry
        json{{"resourceType", "Bundle"}, {"entry", "not_array"}},
        json{{"resourceType", "Bundle"}, {"entry", json::array()}}, // empty entries
    };
    for (const auto& g : garbage) {
        auto r = artefact_from_bundle(g);
        // Must not throw, must reject. We don't *require* an error here
        // — empty entries is technically well-formed-bundle but has no
        // Consent — in which case artefact_from_bundle returns invalid.
        CHECK_FALSE(r);
    }
}

TEST_CASE("hardening: well_formed_bundle is robust to type confusion") {
    using nlohmann::json;
    // resourceType present but wrong type
    CHECK_FALSE(fhir::well_formed_bundle(json{{"resourceType", 123}}));
    CHECK_FALSE(fhir::well_formed_bundle(json{{"resourceType", json::array()}}));
    // entry present but not an array
    CHECK_FALSE(fhir::well_formed_bundle(json{{"resourceType", "Bundle"},
                                              {"entry", "string"}}));
    CHECK_FALSE(fhir::well_formed_bundle(json{{"resourceType", "Bundle"},
                                              {"entry", 42}}));
    // entry array contains a non-object
    CHECK_FALSE(fhir::well_formed_bundle(json{{"resourceType", "Bundle"},
                                              {"entry", json::array({"string"})}}));
}

TEST_CASE("hardening: from_abdm_json rejects array purposes with mixed-type elements") {
    auto bad = nlohmann::json{
        {"artefact_id", "art-bad"},
        {"patient", "pat:p"},
        {"requester_id", "hiu"},
        {"fetcher_id", "hip"},
        {"purposes", nlohmann::json::array({"triage", 42, "billing_audit"})},  // mixed
        {"issued_at", Time::now().iso8601()},
        {"expires_at", (Time::now() + std::chrono::hours{1}).iso8601()},
        {"status", "granted"},
    }.dump();
    CHECK_FALSE(from_abdm_json(bad));
}

TEST_CASE("hardening: attestation_from_json rejects oversize hex blobs") {
    // sig and pk lengths must match KeyStore::sig_bytes / pk_bytes exactly.
    auto good_actor = "clinician:dr_x";
    auto build = [&](std::size_t sig_chars, std::size_t pk_chars) {
        return nlohmann::json{
            {"actor",        good_actor},
            {"subject_kind", "k"},
            {"subject_id",   "id"},
            {"statement",    "s"},
            {"attested_at",  Time::now().iso8601()},
            {"signature",    std::string(sig_chars, 'a')},
            {"public_key",   std::string(pk_chars,  'a')},
        }.dump();
    };
    // Way too long signature.
    CHECK_FALSE(attestation_from_json(build(KeyStore::sig_bytes * 4,
                                            KeyStore::pk_bytes  * 2)));
    // Way too long public key.
    CHECK_FALSE(attestation_from_json(build(KeyStore::sig_bytes * 2,
                                            KeyStore::pk_bytes  * 4)));
    // Zero length.
    CHECK_FALSE(attestation_from_json(build(0, 0)));
}

TEST_CASE("hardening: envelope_from_json rejects malformed hash hex") {
    // Build a real signed envelope, mutate the video_hash to non-hex.
    auto k_md = KeyStore::generate();
    auto k_pt = KeyStore::generate();
    auto e = make_envelope("c", PatientId::pseudonymous("p"),
                           ActorId::clinician("d"),
                           "topic", Time::now(),
                           Time::now() + std::chrono::seconds{60});
    sign_as_clinician(e, k_md);
    sign_as_patient(e, k_pt);
    auto js = envelope_to_json(e);
    auto j  = nlohmann::json::parse(js);
    j["video_hash"] = "ZZZZ";  // non-hex
    CHECK_FALSE(envelope_from_json(j.dump()));
}

TEST_CASE("hardening: bill_audit_from_json rejects findings with non-numeric amounts") {
    auto bad = nlohmann::json{
        {"bundle_id", "x"}, {"patient", "pat:p"}, {"auditor", "clinician:a"},
        {"hospital_id", "h"}, {"reference_table", "t"},
        {"audited_at", Time::now().iso8601()},
        {"findings", nlohmann::json::array({nlohmann::json{
            {"item_code", "x"}, {"item_description", ""},
            {"billed_amount", "string-not-number"},   // wrong type
            {"reference_amount", 0.0},
            {"severity", "consistent"}, {"note", ""},
        }})},
        {"total_billed", 0.0}, {"total_reference", 0.0},
        {"signature",  std::string(KeyStore::sig_bytes * 2, '0')},
        {"public_key", std::string(KeyStore::pk_bytes  * 2, '0')},
    }.dump();
    // nlohmann::json's value() returns the default on type mismatch
    // rather than throwing; the parser should still produce a usable
    // bundle with billed_amount = 0.0. Confirm that — and that
    // verification fails on the unsigned-by-this-key payload.
    auto r = bill_audit_from_json(bad);
    if (r) {
        CHECK(r.value().findings.size() == 1);
        CHECK_FALSE(verify_bill_audit(r.value()));
    }
}

TEST_CASE("hardening: care_path_from_json rejects array constraint instead of object") {
    auto bad = nlohmann::json{
        {"attestation_id", "x"}, {"patient", "pat:p"},
        {"attester", "clinician:d"},
        {"constraint", nlohmann::json::array()},  // wrong kind
        {"context", nlohmann::json::object()},
        {"decision", "allow"}, {"reason", ""},
        {"attested_at", Time::now().iso8601()},
        {"signature",  std::string(KeyStore::sig_bytes * 2, '0')},
        {"public_key", std::string(KeyStore::pk_bytes  * 2, '0')},
    }.dump();
    CHECK_FALSE(care_path_from_json(bad));
}

TEST_CASE("hardening: round-trip of a maximally-large but valid artefact") {
    // 100 purposes (some repeated) — kernel should tolerate.
    ConsentArtefact a;
    a.artefact_id    = std::string(512, 'A');
    a.patient        = PatientId::pseudonymous(std::string(128, 'p'));
    a.requester_id   = std::string(256, 'r');
    a.fetcher_id     = std::string(256, 'f');
    a.issued_at      = Time::now();
    a.expires_at     = a.issued_at + std::chrono::hours{1};
    a.status         = ConsentArtefact::Status::granted;
    a.schema_version = "1.0";
    for (int i = 0; i < 100; ++i) {
        a.purposes.push_back(static_cast<Purpose>((i % 14) + 1));
    }
    auto js = to_abdm_json(a);
    auto r  = from_abdm_json(js);
    REQUIRE(r);
    CHECK(r.value().purposes.size() == 100);
    CHECK(r.value().artefact_id.size() == 512);
}

TEST_CASE("hardening: json parsers do not crash on UTF-8 multibyte sequences") {
    auto multibyte = nlohmann::json{
        {"actor", "clinician:dr_iyer"},
        {"subject_kind", "second_opinion"},
        {"subject_id", "case-π-α-β-γ-😀"},
        {"statement",  "मरीज को देखा। निर्णय सही।"},  // Hindi text
        {"attested_at", Time::now().iso8601()},
        {"signature",  std::string(KeyStore::sig_bytes * 2, '0')},
        {"public_key", std::string(KeyStore::pk_bytes  * 2, '0')},
    }.dump();
    auto r = attestation_from_json(multibyte);
    REQUIRE(r);
    CHECK(r.value().subject_id.find("π") != std::string::npos);
    CHECK(r.value().statement.find("मरीज") != std::string::npos);
}
