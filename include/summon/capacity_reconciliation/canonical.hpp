// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Canonical byte encoding.
//
// Every digest, every persisted record and every machine-readable rendering is
// produced by this writer. The encoding is:
//
//   * explicit little-endian for fixed-width integers -- no memcpy of native
//     representations, so a store written on one endianness is *detected* as
//     foreign rather than misread;
//   * length-prefixed for every variable-length field, so no delimiter can be
//     forged out of payload bytes;
//   * self-describing for enums via their stable token, not their ordinal, so
//     reordering an enum cannot silently reinterpret stored state;
//   * free of floating point, locale, timestamps and hash-map iteration order.
//
// Two encoders that are given equal values always emit byte-identical output.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "summon/capacity_reconciliation/hash.hpp"
#include "summon/capacity_reconciliation/quantity.hpp"
#include "summon/capacity_reconciliation/status.hpp"

namespace summon::capacity_reconciliation {

/// Append-only canonical byte sink.
class CanonicalWriter {
 public:
  CanonicalWriter() = default;

  void u8(std::uint8_t value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void i64(std::int64_t value);
  void boolean(bool value);

  /// Tagged field: 1-byte tag then payload. The tag makes field order part of
  /// the encoding, so a truncated or reordered payload fails to decode.
  void field(std::uint8_t tag);

  /// Length-prefixed bytes.
  void bytes(std::string_view value);

  /// Length-prefixed token (bounded ASCII). Callers must have validated it.
  void token(std::string_view value) { bytes(value); }

  void raw(const void* data, std::size_t length);
  void concat(const CanonicalWriter& other);

  [[nodiscard]] const std::string& buffer() const noexcept { return buffer_; }
  [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
  [[nodiscard]] bool empty() const noexcept { return buffer_.empty(); }

  [[nodiscard]] std::string take() && { return std::move(buffer_); }

  /// Digest of everything written so far, with a domain separator.
  [[nodiscard]] Digest digest(std::string_view domain) const noexcept;

 private:
  std::string buffer_;
};

/// Bounded, strict canonical reader. Every read is checked against the
/// remaining length; running off the end yields MALFORMED, never a default.
class CanonicalReader {
 public:
  explicit CanonicalReader(std::string_view data) noexcept : data_(data) {}

  [[nodiscard]] bool at_end() const noexcept { return offset_ == data_.size(); }
  [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - offset_; }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

  [[nodiscard]] Result<std::uint8_t> u8();
  [[nodiscard]] Result<std::uint16_t> u16();
  [[nodiscard]] Result<std::uint32_t> u32();
  [[nodiscard]] Result<std::uint64_t> u64();
  [[nodiscard]] Result<std::int64_t> i64();
  [[nodiscard]] Result<bool> boolean();

  /// Reads a tag and checks it against `expected`.
  [[nodiscard]] Status field(std::uint8_t expected, const char* what);

  /// Reads a length-prefixed byte string, refusing anything longer than
  /// `max_bytes` before allocating.
  [[nodiscard]] Result<std::string> bytes(std::size_t max_bytes, const char* what);

  /// Reads a bounded token and validates its character class.
  [[nodiscard]] Result<std::string> token(std::size_t max_bytes, const char* what);

  /// Requires that the reader is exhausted.
  [[nodiscard]] Status exhausted(const char* what) const;

 private:
  [[nodiscard]] Status need(std::size_t count, const char* what) const;

  std::string_view data_;
  std::size_t offset_ = 0;
};

}  // namespace summon::capacity_reconciliation
