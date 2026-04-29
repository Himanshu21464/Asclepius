// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
//
// Round 103 — canonicalisation primitive.
//
// The five evidence shapes shipped in rounds 92–96 (HumanAttestation,
// TeleConsultEnvelope, BillAuditBundle, SampleIntegrityBundle,
// CarePathAttestation) all share a four-step signing pattern:
//
//   1. Build a canonical byte sequence from the typed fields
//      (field-concatenation, not JSON, to avoid serialiser drift).
//   2. Sign those bytes with a KeyStore (Ed25519 over libsodium).
//   3. To round-trip the signature on the wire, hex-encode it; on parse,
//      hex-decode and validate length.
//   4. Verify by recomputing the canonical bytes and checking the
//      signature against the embedded public key.
//
// Step (1) is intrinsically per-shape (each evidence type has different
// fields). Steps (2)–(4) are not. This module hoists the shape-agnostic
// half into one substrate primitive so the five impl files don't carry
// copy-paste hex / sign / verify helpers.

#ifndef ASCLEPIUS_CANONICAL_HPP
#define ASCLEPIUS_CANONICAL_HPP

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "asclepius/audit.hpp"
#include "asclepius/core.hpp"

namespace asclepius::canonical {

// ---- Hex round-trip ------------------------------------------------------

// Encode bytes as lowercase hex. No separators, no prefix.
std::string to_hex(std::span<const std::uint8_t> bytes);

// Decode hex. Returns Error::invalid on odd length or non-hex character.
// The decoded vector has size == s.size() / 2.
Result<std::vector<std::uint8_t>> from_hex(std::string_view s);

// Fixed-size decode variant: decodes into a caller-supplied output
// buffer, returning true on success and false on length mismatch or
// non-hex content. Used by callers that already own the destination
// (e.g. ledger fields with fixed-size Hash / signature arrays). No
// allocations; no exceptions.
bool from_hex_into(std::string_view s, std::span<std::uint8_t> out) noexcept;

// ---- Sign / verify -------------------------------------------------------

// Sign canonical bytes with `signer`. Wraps KeyStore::sign over a span
// constructed from the supplied vector or array, eliminating the
// `Bytes{vec.data(), vec.size()}` boilerplate that every evidence-shape
// impl repeated.
std::array<std::uint8_t, KeyStore::sig_bytes>
sign(const KeyStore& signer, std::span<const std::uint8_t> canonical);

// Verify a signature over canonical bytes against the supplied public
// key. noexcept; libsodium failure is reported as `false`.
bool verify(std::span<const std::uint8_t>                       canonical,
            std::span<const std::uint8_t, KeyStore::sig_bytes>  signature,
            std::span<const std::uint8_t, KeyStore::pk_bytes>   public_key) noexcept;

// ---- JSON-shape sign/key helpers ----------------------------------------
//
// Convenience that round-trips a (signature, public_key) pair through
// hex strings, the format every evidence shape uses on the wire. The
// caller passes the JSON-decoded hex strings; the helper validates
// lengths and copies into the typed arrays. Returns Error::invalid on
// length mismatch or non-hex content.

struct SignatureFields {
    std::array<std::uint8_t, KeyStore::sig_bytes> signature{};
    std::array<std::uint8_t, KeyStore::pk_bytes>  public_key{};
};

Result<SignatureFields>
parse_signature_fields(std::string_view sig_hex, std::string_view pk_hex);

}  // namespace asclepius::canonical

#endif  // ASCLEPIUS_CANONICAL_HPP
