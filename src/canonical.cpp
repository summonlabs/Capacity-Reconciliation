// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/capacity_reconciliation/canonical.hpp"

#include "summon/capacity_reconciliation/identity.hpp"
#include "summon/capacity_reconciliation/limits.hpp"

namespace summon::capacity_reconciliation {

void CanonicalWriter::raw(const void* data, std::size_t length) {
  if (length == 0) {
    return;
  }
  buffer_.append(static_cast<const char*>(data), length);
}

void CanonicalWriter::u8(std::uint8_t value) { buffer_.push_back(static_cast<char>(value)); }

void CanonicalWriter::u16(std::uint16_t value) {
  buffer_.push_back(static_cast<char>(value & 0xFFu));
  buffer_.push_back(static_cast<char>((value >> 8u) & 0xFFu));
}

void CanonicalWriter::u32(std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    buffer_.push_back(static_cast<char>((value >> shift) & 0xFFu));
  }
}

void CanonicalWriter::u64(std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    buffer_.push_back(static_cast<char>((value >> shift) & 0xFFu));
  }
}

void CanonicalWriter::i64(std::int64_t value) {
  // Two's complement reinterpretation, written little-endian explicitly. This
  // is defined behaviour for the conversion, unlike reading through a punning
  // pointer.
  const auto bits = static_cast<std::uint64_t>(value);
  u64(bits);
}

void CanonicalWriter::boolean(bool value) { u8(value ? 1u : 0u); }

void CanonicalWriter::field(std::uint8_t tag) { u8(tag); }

void CanonicalWriter::bytes(std::string_view value) {
  u64(static_cast<std::uint64_t>(value.size()));
  raw(value.data(), value.size());
}

void CanonicalWriter::concat(const CanonicalWriter& other) {
  buffer_.append(other.buffer_);
}

Digest CanonicalWriter::digest(std::string_view domain) const noexcept {
  return digest_with_domain(domain, buffer_);
}

Status CanonicalReader::need(std::size_t count, const char* what) const {
  if (count > data_.size() - offset_) {
    return make_error(ErrorCode::Malformed,
                      std::string("canonical stream truncated while reading ") + what,
                      "truncated");
  }
  return Status::success();
}

Result<std::uint8_t> CanonicalReader::u8() {
  auto status = need(1, "u8");
  if (!status.ok()) {
    return status.error();
  }
  const auto value = static_cast<std::uint8_t>(data_[offset_]);
  offset_ += 1;
  return value;
}

Result<std::uint16_t> CanonicalReader::u16() {
  auto status = need(2, "u16");
  if (!status.ok()) {
    return status.error();
  }
  std::uint16_t value = 0;
  value |= static_cast<std::uint16_t>(static_cast<std::uint8_t>(data_[offset_]));
  value |= static_cast<std::uint16_t>(static_cast<std::uint16_t>(
               static_cast<std::uint8_t>(data_[offset_ + 1]))
               << 8u);
  offset_ += 2;
  return value;
}

Result<std::uint32_t> CanonicalReader::u32() {
  auto status = need(4, "u32");
  if (!status.ok()) {
    return status.error();
  }
  std::uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(data_[offset_ + i]))
             << (i * 8u);
  }
  offset_ += 4;
  return value;
}

Result<std::uint64_t> CanonicalReader::u64() {
  auto status = need(8, "u64");
  if (!status.ok()) {
    return status.error();
  }
  std::uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(data_[offset_ + i]))
             << (i * 8u);
  }
  offset_ += 8;
  return value;
}

Result<std::int64_t> CanonicalReader::i64() {
  auto raw = u64();
  if (!raw.ok()) {
    return raw.error();
  }
  return static_cast<std::int64_t>(raw.value());
}

Result<bool> CanonicalReader::boolean() {
  auto raw = u8();
  if (!raw.ok()) {
    return raw.error();
  }
  if (raw.value() > 1u) {
    return make_error(ErrorCode::Malformed,
                      "canonical stream contains a non-boolean where a boolean was expected",
                      "boolean");
  }
  return raw.value() == 1u;
}

Status CanonicalReader::field(std::uint8_t expected, const char* what) {
  auto status = need(1, what);
  if (!status.ok()) {
    return status;
  }
  const auto actual = static_cast<std::uint8_t>(data_[offset_]);
  offset_ += 1;
  if (actual != expected) {
    return make_error(ErrorCode::Malformed,
                      std::string("canonical stream field tag mismatch for ") + what +
                          ": expected " + std::to_string(expected) + ", read " +
                          std::to_string(actual),
                      "field_tag");
  }
  return Status::success();
}

Result<std::string> CanonicalReader::bytes(std::size_t max_bytes, const char* what) {
  auto length = u64();
  if (!length.ok()) {
    return length.error();
  }
  if (length.value() > static_cast<std::uint64_t>(max_bytes)) {
    return make_error(ErrorCode::LimitExceeded,
                      std::string("declared length ") + std::to_string(length.value()) +
                          " exceeds the limit for " + what,
                      "declared_length");
  }
  if (length.value() > static_cast<std::uint64_t>(remaining())) {
    return make_error(ErrorCode::Malformed,
                      std::string("declared length ") + std::to_string(length.value()) +
                          " exceeds the remaining stream for " + what,
                      "truncated");
  }
  const std::size_t count = static_cast<std::size_t>(length.value());
  std::string out(data_.substr(offset_, count));
  offset_ += count;
  return out;
}

Result<std::string> CanonicalReader::token(std::size_t max_bytes, const char* what) {
  auto value = bytes(max_bytes, what);
  if (!value.ok()) {
    return value.error();
  }
  if (!is_valid_token(value.value(), max_bytes)) {
    return make_error(ErrorCode::Malformed,
                      std::string("stored token for ") + what + " is not a valid token",
                      "token");
  }
  return value;
}

Status CanonicalReader::exhausted(const char* what) const {
  if (offset_ != data_.size()) {
    return make_error(ErrorCode::Malformed,
                      std::string("canonical stream for ") + what + " has " +
                          std::to_string(data_.size() - offset_) + " trailing bytes",
                      "trailing_bytes");
  }
  return Status::success();
}

}  // namespace summon::capacity_reconciliation
