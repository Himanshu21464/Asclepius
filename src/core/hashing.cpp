// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include "asclepius/hashing.hpp"

#include <sodium.h>

#include <cstring>
#include <mutex>
#include <stdexcept>

namespace asclepius {

namespace {

void ensure_sodium_initialized() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        if (sodium_init() == -1) {
            // Sodium init can fail only on system-level failure (no RNG, etc).
            // We surface this loudly because everything downstream depends on it.
            std::fprintf(stderr, "asclepius: sodium_init failed\n");
            std::abort();
        }
    });
}

constexpr std::array<char, 16> kHexDigits = {
    '0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','f'
};

}  // namespace

bool Hash::is_zero() const noexcept {
    for (auto b : bytes) if (b != 0) return false;
    return true;
}

std::string Hash::hex() const {
    std::string out(size * 2, '\0');
    for (std::size_t i = 0; i < size; ++i) {
        out[2 * i + 0] = kHexDigits[(bytes[i] >> 4) & 0xf];
        out[2 * i + 1] = kHexDigits[(bytes[i]     ) & 0xf];
    }
    return out;
}

Result<Hash> Hash::from_hex(std::string_view s) {
    if (s.size() != size * 2) {
        return Error::invalid("hash hex length must be 64");
    }
    auto nyb = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };
    Hash h{};
    for (std::size_t i = 0; i < size; ++i) {
        int hi = nyb(s[2 * i + 0]);
        int lo = nyb(s[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return Error::invalid("hash hex contains non-hex character");
        }
        h.bytes[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return h;
}

// ---- Hasher --------------------------------------------------------------

struct Hasher::Impl {
    crypto_generichash_state state;
};

Hasher::Hasher() : impl_(new Impl) {
    ensure_sodium_initialized();
    crypto_generichash_init(&impl_->state, nullptr, 0, Hash::size);
}

Hasher::Hasher(Hasher&& other) noexcept : impl_(other.impl_) {
    other.impl_ = nullptr;
}
Hasher& Hasher::operator=(Hasher&& other) noexcept {
    if (this != &other) {
        delete impl_;
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

Hasher::~Hasher() {
    delete impl_;
}

Hasher& Hasher::update(Bytes data) {
    crypto_generichash_update(&impl_->state, data.data(), data.size());
    return *this;
}

Hasher& Hasher::update(std::string_view sv) {
    crypto_generichash_update(&impl_->state,
                              reinterpret_cast<const std::uint8_t*>(sv.data()),
                              sv.size());
    return *this;
}

Hash Hasher::finalize() {
    Hash h{};
    crypto_generichash_final(&impl_->state, h.bytes.data(), h.bytes.size());
    return h;
}

Hash hash(Bytes data) {
    ensure_sodium_initialized();
    Hash h{};
    crypto_generichash(h.bytes.data(), h.bytes.size(),
                       data.data(),    data.size(),
                       nullptr, 0);
    return h;
}

Hash hash(std::string_view sv) {
    return hash(Bytes{reinterpret_cast<const std::uint8_t*>(sv.data()), sv.size()});
}

}  // namespace asclepius
