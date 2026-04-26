// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include <doctest/doctest.h>

#include "asclepius/interop/fhir.hpp"

#include <nlohmann/json.hpp>

using namespace asclepius;
using namespace asclepius::fhir;

TEST_CASE("FHIR parse_reference splits resourceType/id") {
    auto r = parse_reference("Patient/abc123");
    REQUIRE(r);
    CHECK(r.value().resource_type == "Patient");
    CHECK(r.value().id == "abc123");
}

TEST_CASE("FHIR parse_reference rejects bad input") {
    CHECK(!parse_reference("nope"));
    CHECK(!parse_reference("/abc"));
    CHECK(!parse_reference("Patient/"));
}

TEST_CASE("FHIR extract_scope finds Patient and Encounter") {
    auto bundle = nlohmann::json::parse(R"({
        "resourceType":"Bundle",
        "entry":[
          {"resource":{"resourceType":"Patient",  "id":"pat-9"}},
          {"resource":{"resourceType":"Encounter","id":"enc-1"}}
        ]
    })");
    auto s = extract_scope(bundle);
    REQUIRE(s);
    REQUIRE(s.value().patient.has_value());
    REQUIRE(s.value().encounter.has_value());
    CHECK(s.value().patient->str()   == "fhir:Patient/pat-9");
    CHECK(s.value().encounter->str() == "fhir:Encounter/enc-1");
}

TEST_CASE("FHIR purpose_from_v3_code maps known codes") {
    CHECK(purpose_from_v3_code("TREAT")   .value() == Purpose::ambient_documentation);
    CHECK(purpose_from_v3_code("DIAGNOST").value() == Purpose::diagnostic_suggestion);
    CHECK(purpose_from_v3_code("RXMG")    .value() == Purpose::medication_review);
    CHECK(!purpose_from_v3_code("UNKNOWN"));
}
