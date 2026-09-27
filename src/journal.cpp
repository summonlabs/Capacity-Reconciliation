// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/capacity_reconciliation/journal.hpp"

#include <cstring>

#include "summon/capacity_reconciliation/limits.hpp"

namespace summon::capacity_reconciliation {
namespace {

void write_u32(std::string* out, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    out->push_back(static_cast<char>((value >> shift) & 0xFFu));
  }
}

void write_u64(std::string* out, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    out->push_back(static_cast<char>((value >> shift) & 0xFFu));
  }
}

std::uint32_t read_u32(std::string_view bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + i])) << (i * 8u);
  }
  return value;
}

std::uint64_t read_u64(std::string_view bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(bytes[offset + i])) << (i * 8u);
  }
  return value;
}

}  // namespace

std::string RecordHeader::encode() const {
  std::string out;
  out.reserve(kRecordHeaderBytes);
  out.append(kRecordMagic);
  write_u32(&out, format_version);
  write_u32(&out, kEndianMarker);
  write_u64(&out, payload_length);
  out.append(reinterpret_cast<const char*>(payload_digest.data()), payload_digest.size());
  out.append(reinterpret_cast<const char*>(previous_record_digest.data()),
             previous_record_digest.size());
  out.append(reinterpret_cast<const char*>(store_identity.data()), store_identity.size());
  write_u64(&out, generation.value());
  write_u64(&out, attempt.value());
  return out;
}

Result<RecordHeader> RecordHeader::decode(std::string_view header_bytes) {
  if (header_bytes.size() != kRecordHeaderBytes) {
    return make_error(ErrorCode::Malformed,
                      "record header is " + std::to_string(header_bytes.size()) +
                          " bytes, expected " + std::to_string(kRecordHeaderBytes),
                      "header_size");
  }
  if (header_bytes.substr(0, kRecordMagic.size()) != kRecordMagic) {
    return make_error(ErrorCode::Malformed,
                      "record magic does not match: this is not a capacity reconciliation record",
                      "magic");
  }
  RecordHeader header;
  header.format_version = read_u32(header_bytes, 8);
  const std::uint32_t endian = read_u32(header_bytes, 12);
  if (endian != kEndianMarker) {
    return make_error(ErrorCode::IncompatibleVersion,
                      "record endian marker is 0x" + [&endian] {
                        static const char* kHex = "0123456789abcdef";
                        std::string text;
                        for (int shift = 28; shift >= 0; shift -= 4) {
                          text.push_back(kHex[(endian >> shift) & 0xFu]);
                        }
                        return text;
                      }() + "; this record was written by a different architecture",
                      "endian");
  }
  if (header.format_version != kRecordFormatVersion) {
    return make_error(ErrorCode::IncompatibleVersion,
                      "record format version " + std::to_string(header.format_version) +
                          " is not the version this build reads (" +
                          std::to_string(kRecordFormatVersion) + ")",
                      "format_version");
  }
  header.payload_length = read_u64(header_bytes, 16);
  if (header.payload_length > static_cast<std::uint64_t>(kMaxRecordBytes)) {
    return make_error(ErrorCode::LimitExceeded,
                      "record declares a payload of " + std::to_string(header.payload_length) +
                          " bytes, above the maximum of " + std::to_string(kMaxRecordBytes),
                      "declared_length");
  }
  std::memcpy(header.payload_digest.data(), header_bytes.data() + 24, 32);
  std::memcpy(header.previous_record_digest.data(), header_bytes.data() + 56, 32);
  std::memcpy(header.store_identity.data(), header_bytes.data() + 88, 32);
  header.generation = Generation{read_u64(header_bytes, 120)};
  header.attempt = AttemptId{read_u64(header_bytes, 128)};
  return header;
}

Result<std::string> frame_record(const RecordHeader& header, std::string_view payload) {
  if (payload.size() > kMaxRecordBytes) {
    return make_error(ErrorCode::LimitExceeded,
                      "payload of " + std::to_string(payload.size()) +
                          " bytes exceeds the record limit of " + std::to_string(kMaxRecordBytes),
                      "payload_length");
  }
  RecordHeader local = header;
  local.payload_length = static_cast<std::uint64_t>(payload.size());
  local.payload_digest = Sha256::hash(payload);
  std::string out = local.encode();
  out.append(payload);
  return out;
}

Result<std::string> unframe_record(std::string_view file_bytes, RecordHeader* header_out) {
  if (file_bytes.size() < kRecordHeaderBytes) {
    return make_error(ErrorCode::Corruption,
                      "record file is " + std::to_string(file_bytes.size()) +
                          " bytes, shorter than the " + std::to_string(kRecordHeaderBytes) +
                          " byte header: the write was truncated",
                      "truncated");
  }
  auto header = RecordHeader::decode(file_bytes.substr(0, kRecordHeaderBytes));
  if (!header.ok()) {
    return header.error();
  }
  const std::uint64_t declared = header.value().payload_length;
  const std::uint64_t available =
      static_cast<std::uint64_t>(file_bytes.size() - kRecordHeaderBytes);
  if (declared != available) {
    return make_error(ErrorCode::Corruption,
                      "record declares a payload of " + std::to_string(declared) +
                          " bytes but carries " + std::to_string(available) +
                          ": the file is torn or padded",
                      "length_mismatch");
  }
  const std::string_view payload = file_bytes.substr(kRecordHeaderBytes);
  const Digest actual = Sha256::hash(payload);
  if (!(actual == header.value().payload_digest)) {
    return make_error(ErrorCode::Corruption,
                      "record payload digest does not match: expected " +
                          header.value().payload_digest.to_hex() + ", computed " +
                          actual.to_hex(),
                      "payload_digest");
  }
  if (header_out != nullptr) {
    *header_out = header.value();
  }
  return std::string(payload);
}

}  // namespace summon::capacity_reconciliation
