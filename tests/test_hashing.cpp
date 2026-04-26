// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include <doctest/doctest.h>

#include "asclepius/hashing.hpp"

using namespace asclepius;

TEST_CASE("hash() is deterministic for identical inputs") {
    auto a = hash(std::string_view{"hello world"});
    auto b = hash(std::string_view{"hello world"});
    CHECK(a == b);
}

TEST_CASE("hash() differs for different inputs") {
    auto a = hash(std::string_view{"hello"});
    auto b = hash(std::string_view{"hello!"});
    CHECK(a != b);
}

TEST_CASE("Hash::hex is 64 chars and round-trips") {
    auto h    = hash(std::string_view{"asclepius"});
    auto hex  = h.hex();
    REQUIRE(hex.size() == 64);
    auto back = Hash::from_hex(hex);
    REQUIRE(back);
    CHECK(back.value() == h);
}

TEST_CASE("Hash::from_hex rejects malformed input") {
    auto bad  = Hash::from_hex("not-a-hash");
    CHECK(!bad);
    auto bad2 = Hash::from_hex(std::string(63, 'a'));
    CHECK(!bad2);
}

TEST_CASE("Streaming hasher matches one-shot") {
    Hasher h;
    h.update(std::string_view{"hello "}).update(std::string_view{"world"});
    auto streamed = h.finalize();
    auto direct   = hash(std::string_view{"hello world"});
    CHECK(streamed == direct);
}

TEST_CASE("Hash::zero is all zeros") {
    auto z = Hash::zero();
    CHECK(z.is_zero());
    CHECK(z.hex() == std::string(64, '0'));
}
