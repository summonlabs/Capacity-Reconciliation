// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Record framing and integrity.
//
// A persisted record is a self-describing, integrity-checked frame:
//
//   offset  size  field
//   0       8     magic "CRRECORD"
//   8       4     format version (u32, little endian, explicit)
//   12      4     endian marker 0x01020304
//   16      8     payload length (u64)
//   24      32    payload SHA-256
//   56      32    previous record digest (chain link; zero for the first)
//   88      32    store identity digest
//   120     8     generation
//   128     8     attempt id
//   136     ...   payload
//
// Because every multi-byte field is written byte-by-byte in little-endian
// order and the endian marker is verified, a record written by a foreign
// endianness or a foreign format is *detected* rather than misread. Because the
// payload digest and the chain link are both verified, a torn write, a bit
// flip, a truncated file or an interior rewrite is detected rather than
// partially adopted.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "summon/capacity_reconciliation/hash.hpp"
#include "summon/capacity_reconciliation/identity.hpp"
#include "summon/capacity_reconciliation/status.hpp"
#include "summon/capacity_reconciliation/version.hpp"

namespace summon::capacity_reconciliation {

/// Fixed header size in bytes: 8 magic + 4 version + 4 endian + 8 length +
/// 32 payload digest + 32 chain link + 32 store identity + 8 generation +
/// 8 attempt.
inline constexpr std::size_t kRecordHeaderBytes = 136;

/// The record magic, exactly 8 bytes.
inline constexpr std::string_view kRecordMagic = "CRRECORD";

/// Encoded endian marker value.
inline constexpr std::uint32_t kEndianMarker = 0x01020304u;

/// Header of a framed record.
struct RecordHeader {
  std::uint32_t format_version = kRecordFormatVersion;
  std::uint64_t payload_length = 0;
  Digest payload_digest;
  Digest previous_record_digest;
  Digest store_identity;
  Generation generation;
  AttemptId attempt;

  [[nodiscard]] std::string encode() const;
  [[nodiscard]] static Result<RecordHeader> decode(std::string_view header_bytes);
};

/// Frame a payload. Returns header + payload concatenated.
[[nodiscard]] Result<std::string> frame_record(const RecordHeader& header, std::string_view payload);

/// Split and verify a framed record. Refuses oversized payloads before
/// allocating, and verifies magic, version, endian marker, length, digest and
/// that the framing consumed exactly the whole file.
[[nodiscard]] Result<std::string> unframe_record(std::string_view file_bytes,
                                                 RecordHeader* header_out);

}  // namespace summon::capacity_reconciliation
