// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include "asclepius/audit.hpp"
#include "asclepius/hashing.hpp"

#include <sodium.h>

#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>

namespace asclepius {

namespace {

void ensure_sodium_initialized() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        if (sodium_init() == -1) {
            std::fprintf(stderr, "asclepius: sodium_init failed\n");
            std::abort();
        }
    });
}

constexpr std::array<char, 16> kHexDigits = {
    '0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','f'
};

std::string hex_of(const std::uint8_t* p, std::size_t n) {
    std::string out(n * 2, '\0');
    for (std::size_t i = 0; i < n; ++i) {
        out[2 * i + 0] = kHexDigits[(p[i] >> 4) & 0xf];
        out[2 * i + 1] = kHexDigits[(p[i]     ) & 0xf];
    }
    return out;
}

}  // namespace

struct KeyStore::Impl {
    std::array<std::uint8_t, KeyStore::pk_bytes> pk{};
    std::array<std::uint8_t, KeyStore::sk_bytes> sk{};

    Impl() = default;
    ~Impl() {
        sodium_memzero(sk.data(), sk.size());
    }
};

KeyStore::KeyStore(KeyStore&& other) noexcept : impl_(other.impl_) {
    other.impl_ = nullptr;
}
KeyStore& KeyStore::operator=(KeyStore&& other) noexcept {
    if (this != &other) {
        delete impl_;
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}
KeyStore::~KeyStore() {
    delete impl_;
}

KeyStore KeyStore::generate() {
    ensure_sodium_initialized();
    KeyStore ks;
    ks.impl_ = new Impl;
    crypto_sign_keypair(ks.impl_->pk.data(), ks.impl_->sk.data());
    return ks;
}

Result<KeyStore> KeyStore::from_seed(std::span<const std::uint8_t, 32> seed) {
    ensure_sodium_initialized();
    KeyStore ks;
    ks.impl_ = new Impl;
    if (crypto_sign_seed_keypair(ks.impl_->pk.data(),
                                 ks.impl_->sk.data(),
                                 seed.data()) != 0) {
        delete ks.impl_;
        return Error::internal("crypto_sign_seed_keypair failed");
    }
    return ks;
}

std::string KeyStore::serialize() const {
    // Format: "ASCLEPIUS-KEY-v1:<hex(pk)>:<hex(sk)>"
    return std::string{"ASCLEPIUS-KEY-v1:"}
         + hex_of(impl_->pk.data(), impl_->pk.size())
         + ":"
         + hex_of(impl_->sk.data(), impl_->sk.size());
}

namespace {
int unhex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}
bool unhex_into(std::string_view s, std::uint8_t* out, std::size_t n) {
    if (s.size() != n * 2) return false;
    for (std::size_t i = 0; i < n; ++i) {
        int hi = unhex(s[2 * i]);
        int lo = unhex(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return true;
}
}  // namespace

Result<KeyStore> KeyStore::deserialize(std::string_view encoded) {
    constexpr std::string_view prefix = "ASCLEPIUS-KEY-v1:";
    if (encoded.substr(0, prefix.size()) != prefix) {
        return Error::invalid("not an asclepius key blob");
    }
    auto rest = encoded.substr(prefix.size());
    auto sep = rest.find(':');
    if (sep == std::string_view::npos) {
        return Error::invalid("malformed key blob");
    }
    auto pk_hex = rest.substr(0, sep);
    auto sk_hex = rest.substr(sep + 1);
    KeyStore ks;
    ks.impl_ = new Impl;
    if (!unhex_into(pk_hex, ks.impl_->pk.data(), ks.impl_->pk.size()) ||
        !unhex_into(sk_hex, ks.impl_->sk.data(), ks.impl_->sk.size())) {
        delete ks.impl_;
        return Error::invalid("malformed key hex");
    }
    return ks;
}

std::string KeyStore::key_id() const {
    return hex_of(impl_->pk.data(), 8);  // first 16 hex chars
}

std::string KeyStore::fingerprint() const {
    auto h = hash(Bytes{impl_->pk.data(), impl_->pk.size()});
    return hex_of(h.bytes.data(), 8);
}

std::array<std::uint8_t, KeyStore::pk_bytes> KeyStore::public_key() const {
    return impl_->pk;
}

std::array<std::uint8_t, KeyStore::sig_bytes> KeyStore::sign(Bytes message) const {
    std::array<std::uint8_t, sig_bytes> sig{};
    unsigned long long sig_len = 0;
    crypto_sign_detached(sig.data(), &sig_len,
                         message.data(), message.size(),
                         impl_->sk.data());
    return sig;
}

bool KeyStore::verify(Bytes                                       message,
                      std::span<const std::uint8_t, sig_bytes>   signature,
                      std::span<const std::uint8_t, pk_bytes>    pk) {
    ensure_sodium_initialized();
    return crypto_sign_verify_detached(signature.data(),
                                       message.data(), message.size(),
                                       pk.data()) == 0;
}

}  // namespace asclepius
