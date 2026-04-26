// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#ifndef ASCLEPIUS_HASHING_HPP
#define ASCLEPIUS_HASHING_HPP

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "asclepius/core.hpp"

namespace asclepius {

// 32-byte digest. We use BLAKE2b-256 throughout; chosen for speed, security
// margin, and first-class libsodium support. The digest is content-addressed:
// equal Hash values mean byte-equal inputs.
struct Hash {
    static constexpr std::size_t size = 32;
    std::array<std::uint8_t, size> bytes{};

    bool operator==(const Hash&) const = default;
    bool operator!=(const Hash&) const = default;

    bool is_zero() const noexcept;

    std::string         hex() const;
    static Result<Hash> from_hex(std::string_view);

    static Hash zero() noexcept { return Hash{}; }
};

// Streaming hasher. Construct, feed bytes via update(), finalize() once.
// Each instance is one-shot.
class Hasher {
public:
    Hasher();
    Hasher(const Hasher&)            = delete;
    Hasher& operator=(const Hasher&) = delete;
    Hasher(Hasher&&) noexcept;
    Hasher& operator=(Hasher&&) noexcept;
    ~Hasher();

    Hasher& update(Bytes data);
    Hasher& update(std::string_view sv);

    Hash finalize();

private:
    struct Impl;
    Impl* impl_;
};

// Convenience one-shots.
Hash hash(Bytes data);
Hash hash(std::string_view sv);

// Standard library hash specialization for use in unordered containers.
struct HashHasher {
    std::size_t operator()(const Hash& h) const noexcept {
        std::size_t out = 0;
        for (std::size_t i = 0; i < sizeof(out); ++i) {
            out = (out << 8) | h.bytes[i];
        }
        return out;
    }
};

}  // namespace asclepius

namespace std {
template <>
struct hash<asclepius::Hash> {
    std::size_t operator()(const asclepius::Hash& h) const noexcept {
        return asclepius::HashHasher{}(h);
    }
};
}  // namespace std

#endif  // ASCLEPIUS_HASHING_HPP
