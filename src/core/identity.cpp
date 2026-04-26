// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include "asclepius/identity.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>

namespace asclepius {

namespace {

// Crockford's base32 alphabet, used in ULIDs and similar time-sortable ids.
constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

}  // namespace

EncounterId EncounterId::make() {
    using namespace std::chrono;
    const auto ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();

    char ts[11] = {0};
    auto v = static_cast<std::uint64_t>(ms);
    for (int i = 9; i >= 0; --i) {
        ts[i] = kAlphabet[v & 0x1f];
        v >>= 5;
    }

    thread_local std::mt19937_64 rng{std::random_device{}()};
    char r[17] = {0};
    auto a = rng();
    auto b = rng();
    for (int i = 7; i >= 0; --i) { r[i]     = kAlphabet[a & 0x1f]; a >>= 5; }
    for (int i = 15; i >= 8; --i){ r[i]     = kAlphabet[b & 0x1f]; b >>= 5; }

    char out[27];
    std::snprintf(out, sizeof(out), "%s%s", ts, r);
    return EncounterId{std::string{"enc:"} + out};
}

}  // namespace asclepius
