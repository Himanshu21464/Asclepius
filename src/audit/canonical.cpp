// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
//
// Round 103 — canonicalisation primitive (impl).

#include "asclepius/canonical.hpp"

#include <algorithm>
#include <cstdint>

namespace asclepius::canonical {

// ---- Hex round-trip ------------------------------------------------------

std::string to_hex(std::span<const std::uint8_t> bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (auto b : bytes) {
        out.push_back(kHex[(b >> 4) & 0xF]);
        out.push_back(kHex[b & 0xF]);
    }
    return out;
}

namespace {
inline int hex_val(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}  // namespace

Result<std::vector<std::uint8_t>> from_hex(std::string_view s) {
    if (s.size() % 2 != 0) {
        return Error::invalid("hex string must have even length");
    }
    std::vector<std::uint8_t> out;
    out.reserve(s.size() / 2);
    for (std::size_t i = 0; i < s.size(); i += 2) {
        int hi = hex_val(s[i]);
        int lo = hex_val(s[i + 1]);
        if (hi < 0 || lo < 0) {
            return Error::invalid("hex string has non-hex character");
        }
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return out;
}

bool from_hex_into(std::string_view s, std::span<std::uint8_t> out) noexcept {
    if (s.size() != out.size() * 2) return false;
    for (std::size_t i = 0; i < out.size(); ++i) {
        int hi = hex_val(s[2 * i]);
        int lo = hex_val(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return true;
}

// ---- Sign / verify -------------------------------------------------------

std::array<std::uint8_t, KeyStore::sig_bytes>
sign(const KeyStore& signer, std::span<const std::uint8_t> canonical) {
    return signer.sign(Bytes{canonical.data(), canonical.size()});
}

bool verify(std::span<const std::uint8_t>                       canonical,
            std::span<const std::uint8_t, KeyStore::sig_bytes>  signature,
            std::span<const std::uint8_t, KeyStore::pk_bytes>   public_key) noexcept {
    try {
        return KeyStore::verify(
            Bytes{canonical.data(), canonical.size()},
            signature,
            public_key);
    } catch (...) {
        return false;
    }
}

// ---- Signature-fields JSON helper ---------------------------------------

Result<SignatureFields>
parse_signature_fields(std::string_view sig_hex, std::string_view pk_hex) {
    auto sig_bytes = from_hex(sig_hex);
    if (!sig_bytes) return sig_bytes.error();
    auto pk_bytes = from_hex(pk_hex);
    if (!pk_bytes) return pk_bytes.error();

    if (sig_bytes.value().size() != KeyStore::sig_bytes) {
        return Error::invalid("signature wrong byte length");
    }
    if (pk_bytes.value().size() != KeyStore::pk_bytes) {
        return Error::invalid("public_key wrong byte length");
    }

    SignatureFields out;
    std::copy(sig_bytes.value().begin(), sig_bytes.value().end(),
              out.signature.begin());
    std::copy(pk_bytes.value().begin(), pk_bytes.value().end(),
              out.public_key.begin());
    return out;
}

}  // namespace asclepius::canonical
