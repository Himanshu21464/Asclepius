// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include <doctest/doctest.h>

#include "asclepius/identity.hpp"

#include <unordered_set>

using namespace asclepius;

TEST_CASE("Strong ids are not interconvertible (compile-time)") {
    ActorId   a = ActorId::clinician("smith");
    PatientId p = PatientId::pseudonymous("p9");
    // Uncommenting the next line should fail to compile (different types).
    // bool same = (a == p);
    CHECK(a.str() == "clinician:smith");
    CHECK(p.str() == "pat:p9");
}

TEST_CASE("EncounterId::make returns unique values") {
    std::unordered_set<std::string> seen;
    for (int i = 0; i < 200; ++i) {
        seen.insert(std::string{EncounterId::make().str()});
    }
    CHECK(seen.size() == 200);
}

TEST_CASE("ModelId composes name and version") {
    ModelId m{"scribe", "v3"};
    CHECK(m.str() == "scribe@v3");
}

TEST_CASE("Equality and hashing for ActorId") {
    ActorId a1 = ActorId::clinician("smith");
    ActorId a2 = ActorId::clinician("smith");
    CHECK(a1 == a2);
    std::hash<ActorId> h;
    CHECK(h(a1) == h(a2));
}
