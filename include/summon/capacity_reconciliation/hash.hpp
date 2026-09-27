// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Self-contained SHA-256.
//
// The runtime needs one cryptographic-strength digest for three jobs:
//   1. content addressing of evidence / attribution items;
//   2. integrity verification of persisted records;
//   3. chaining records so an interior rewrite is detectable.
// A checksum would not be sufficient for job 3, and a third-party dependency
// is not justified for a single well-specified primitive, so SHA-256 is
// implemented here and verified against the FIPS 180-4 test vectors in the
// test suite.
//
// This is *not* a general purpose crypto library: there is no HMAC, no key
// management and no authentication. The digest proves integrity against
// corruption, not against an adversary who can rewrite the whole store.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "summon/capacity_reconciliation/status.hpp"

namespace summon::capacity_reconciliation {

/// 256-bit digest value.
class Digest {
 public:
  constexpr Digest() noexcept = default;

  [[nodiscard]] static constexpr std::size_t size() noexcept { return 32; }

  [[nodiscard]] const std::array<std::uint8_t, 32>& bytes() const noexcept { return bytes_; }

  [[nodiscard]] std::uint8_t* data() noexcept { return bytes_.data(); }
  [[nodiscard]] const std::uint8_t* data() const noexcept { return bytes_.data(); }

  [[nodiscard]] std::string to_hex() const;

  /// Strict parse of 64 lowercase hex digits. Rejects uppercase, wrong length
  /// and non-hex input.
  [[nodiscard]] static std::optional<Digest> from_hex(std::string_view text) noexcept;

  /// True when every byte is zero, i.e. the digest was never computed.
  [[nodiscard]] bool is_zero() const noexcept;

  friend bool operator==(const Digest& a, const Digest& b) noexcept {
    return a.bytes_ == b.bytes_;
  }
  friend bool operator!=(const Digest& a, const Digest& b) noexcept { return !(a == b); }
  friend bool operator<(const Digest& a, const Digest& b) noexcept {
    return a.bytes_ < b.bytes_;
  }

 private:
  friend class Sha256;
  std::array<std::uint8_t, 32> bytes_{};
};

/// Streaming SHA-256.
class Sha256 {
 public:
  Sha256() noexcept;

  void update(const void* data, std::size_t length) noexcept;
  void update(std::string_view text) noexcept;

  /// Finalise. The object must not be reused afterwards without reset().
  [[nodiscard]] Digest finish() noexcept;

  void reset() noexcept;

  /// One-shot convenience.
  [[nodiscard]] static Digest hash(std::string_view data) noexcept;
  [[nodiscard]] static Digest hash(const void* data, std::size_t length) noexcept;

 private:
  void compress(const std::uint8_t block[64]) noexcept;

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::uint64_t total_bytes_ = 0;
  std::size_t buffered_ = 0;
};

/// Domain separator for content addresses. Prepending a domain makes it
/// impossible for a digest computed over one kind of object to be replayed as
/// the digest of another kind.
[[nodiscard]] Digest digest_with_domain(std::string_view domain, std::string_view payload) noexcept;

}  // namespace summon::capacity_reconciliation
