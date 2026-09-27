// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/capacity_reconciliation/store.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include "reconcile.hpp"
#include "summon/capacity_reconciliation/journal.hpp"
#include "summon/capacity_reconciliation/platform.hpp"
#include "summon/capacity_reconciliation/version.hpp"

namespace summon::capacity_reconciliation {
namespace {

constexpr std::string_view kMarkerMagic = "CAPACITY-RECONCILIATION-STORE";
constexpr std::string_view kHeadMagic = "CRHEAD01";
constexpr std::string_view kMarkerDomain = "capacity-reconciliation/store-marker/v1";
constexpr std::string_view kHeadDomain = "capacity-reconciliation/store-head/v1";
constexpr std::string_view kImageDomain = "capacity-reconciliation/store-image/v1";
constexpr std::string_view kRecordFilePrefix = "record-";
constexpr std::string_view kRecordFileSuffix = ".crr";

enum MarkerTag : std::uint8_t {
  kMarkerMagicTag = 1,
  kMarkerFormat = 2,
  kMarkerIdentity = 3,
  kMarkerCreatedAt = 4,
  kMarkerSelfDigest = 5,
  kMarkerEnd = 0x7F,
};

enum HeadTag : std::uint8_t {
  kHeadMagicTag = 1,
  kHeadFormat = 2,
  kHeadIdentity = 3,
  kHeadGeneration = 4,
  kHeadAttempt = 5,
  kHeadIncarnation = 6,
  kHeadEpoch = 7,
  kHeadRecordedAt = 8,
  kHeadClockDomain = 9,
  kHeadRecordDigest = 10,
  kHeadRecordLength = 11,
  kHeadSelfDigest = 12,
  kHeadEnd = 0x7F,
};

enum ImageTag : std::uint8_t {
  kImageFormat = 1,
  kImageIdentity = 2,
  kImageGeneration = 3,
  kImageAttempt = 4,
  kImageIncarnation = 5,
  kImageEpoch = 6,
  kImageRecordedAt = 7,
  kImageClockDomain = 8,
  kImagePreviousRecord = 9,
  kImageRecovered = 10,
  kImageRecoveryNote = 11,
  kImageEvidenceCount = 12,
  kImageEvidence = 13,
  kImageAttributionCount = 14,
  kImageAttribution = 15,
  kImageRunCount = 16,
  kImageRun = 17,
  kImageHistoryCount = 18,
  kImageHistory = 19,
  kImageHistorySequence = 20,
  kImageEnd = 0x7F,
};

enum HistoryTag : std::uint8_t {
  kHistSequence = 1,
  kHistKind = 2,
  kHistGeneration = 3,
  kHistAttempt = 4,
  kHistHasRun = 5,
  kHistRun = 6,
  kHistHasRelated = 7,
  kHistRelated = 8,
  kHistSubject = 9,
  kHistDetail = 10,
  kHistRecordedAt = 11,
  kHistEnd = 0x7F,
};

constexpr std::size_t kMaxDetailBytes = 160;

struct StoredMarker {
  std::uint32_t format_version = kStoreFormatVersion;
  Digest identity;
  Tick created_at;
};

struct StoredHead {
  std::uint32_t format_version = kStoreFormatVersion;
  Digest identity;
  Generation generation;
  AttemptId attempt;
  IncarnationId incarnation;
  Epoch epoch;
  Tick recorded_at;
  ClockDomain clock_domain;
  Digest record_digest;
  std::uint64_t record_length = 0;
};

Result<std::string> encode_marker(const StoredMarker& marker) {
  CanonicalWriter writer;
  writer.field(kMarkerMagicTag);
  writer.bytes(kMarkerMagic);
  writer.field(kMarkerFormat);
  writer.u32(marker.format_version);
  writer.field(kMarkerIdentity);
  writer.bytes(marker.identity.to_hex());
  writer.field(kMarkerCreatedAt);
  writer.u64(marker.created_at.value());
  const Digest self = writer.digest(kMarkerDomain);
  writer.field(kMarkerSelfDigest);
  writer.bytes(self.to_hex());
  writer.field(kMarkerEnd);
  return std::move(writer).take();
}

Result<StoredMarker> decode_marker(std::string_view bytes) {
  CanonicalReader reader(bytes);
  auto status = reader.field(kMarkerMagicTag, "marker magic");
  if (!status.ok()) {
    return status.error();
  }
  auto magic = reader.bytes(kMarkerMagic.size(), "marker magic");
  if (!magic.ok()) {
    return magic.error();
  }
  if (magic.value() != kMarkerMagic) {
    return make_error(ErrorCode::Malformed,
                      "store marker magic does not match: this directory is not a capacity "
                      "reconciliation store",
                      "magic");
  }
  StoredMarker marker;
  status = reader.field(kMarkerFormat, "marker format version");
  if (!status.ok()) {
    return status.error();
  }
  auto format = reader.u32();
  if (!format.ok()) {
    return format.error();
  }
  marker.format_version = format.value();
  if (marker.format_version != kStoreFormatVersion) {
    return make_error(ErrorCode::IncompatibleVersion,
                      "store format version " + std::to_string(marker.format_version) +
                          " is not the version this build reads (" +
                          std::to_string(kStoreFormatVersion) + ")",
                      "format_version");
  }
  status = reader.field(kMarkerIdentity, "marker identity");
  if (!status.ok()) {
    return status.error();
  }
  auto identity_text = reader.bytes(64, "marker identity");
  if (!identity_text.ok()) {
    return identity_text.error();
  }
  auto identity = Digest::from_hex(identity_text.value());
  if (!identity.has_value()) {
    return make_error(ErrorCode::Corruption, "store marker identity is malformed", "identity");
  }
  marker.identity = *identity;

  status = reader.field(kMarkerCreatedAt, "marker created_at");
  if (!status.ok()) {
    return status.error();
  }
  auto created = reader.u64();
  if (!created.ok()) {
    return created.error();
  }
  marker.created_at = Tick{created.value()};

  status = reader.field(kMarkerSelfDigest, "marker self digest");
  if (!status.ok()) {
    return status.error();
  }
  auto self_text = reader.bytes(64, "marker self digest");
  if (!self_text.ok()) {
    return self_text.error();
  }
  auto self = Digest::from_hex(self_text.value());
  if (!self.has_value()) {
    return make_error(ErrorCode::Corruption, "store marker digest is malformed", "digest");
  }
  status = reader.field(kMarkerEnd, "marker end marker");
  if (!status.ok()) {
    return status.error();
  }
  status = reader.exhausted("store marker");
  if (!status.ok()) {
    return status.error();
  }
  // Canonical round-trip: re-encoding the decoded value must reproduce the
  // stored bytes exactly. This detects any non-canonical or reordered encoding
  // that a field-by-field read would accept.
  auto reencoded = encode_marker(marker);
  if (!reencoded.ok()) {
    return reencoded.error();
  }
  if (reencoded.value() != bytes) {
    return make_error(ErrorCode::Corruption,
                      "store marker is not in canonical form; the file was modified after it "
                      "was written",
                      "canonical_form");
  }
  return marker;
}

Result<std::string> encode_head(const StoredHead& head) {
  CanonicalWriter writer;
  writer.field(kHeadMagicTag);
  writer.bytes(kHeadMagic);
  writer.field(kHeadFormat);
  writer.u32(head.format_version);
  writer.field(kHeadIdentity);
  writer.bytes(head.identity.to_hex());
  writer.field(kHeadGeneration);
  writer.u64(head.generation.value());
  writer.field(kHeadAttempt);
  writer.u64(head.attempt.value());
  writer.field(kHeadIncarnation);
  writer.bytes(head.incarnation.to_string());
  writer.field(kHeadEpoch);
  writer.u64(head.epoch.value());
  writer.field(kHeadRecordedAt);
  writer.u64(head.recorded_at.value());
  writer.field(kHeadClockDomain);
  writer.token(head.clock_domain.name());
  writer.field(kHeadRecordDigest);
  writer.bytes(head.record_digest.to_hex());
  writer.field(kHeadRecordLength);
  writer.u64(head.record_length);
  const Digest self = writer.digest(kHeadDomain);
  writer.field(kHeadSelfDigest);
  writer.bytes(self.to_hex());
  writer.field(kHeadEnd);
  return std::move(writer).take();
}

Result<StoredHead> decode_head(std::string_view bytes) {
  CanonicalReader reader(bytes);
  auto status = reader.field(kHeadMagicTag, "head magic");
  if (!status.ok()) {
    return status.error();
  }
  auto magic = reader.bytes(kHeadMagic.size(), "head magic");
  if (!magic.ok()) {
    return magic.error();
  }
  if (magic.value() != kHeadMagic) {
    return make_error(ErrorCode::Malformed,
                      "published manifest magic does not match: the commit point is not a "
                      "capacity reconciliation manifest",
                      "magic");
  }
  StoredHead head;
  status = reader.field(kHeadFormat, "head format version");
  if (!status.ok()) {
    return status.error();
  }
  auto format = reader.u32();
  if (!format.ok()) {
    return format.error();
  }
  head.format_version = format.value();
  if (head.format_version != kStoreFormatVersion) {
    return make_error(ErrorCode::IncompatibleVersion,
                      "published manifest format version " +
                          std::to_string(head.format_version) + " is not readable by this build",
                      "format_version");
  }

  status = reader.field(kHeadIdentity, "head identity");
  if (!status.ok()) {
    return status.error();
  }
  auto identity_text = reader.bytes(64, "head identity");
  if (!identity_text.ok()) {
    return identity_text.error();
  }
  auto identity = Digest::from_hex(identity_text.value());
  if (!identity.has_value()) {
    return make_error(ErrorCode::Corruption, "published manifest identity is malformed",
                      "identity");
  }
  head.identity = *identity;

  status = reader.field(kHeadGeneration, "head generation");
  if (!status.ok()) {
    return status.error();
  }
  auto generation = reader.u64();
  if (!generation.ok()) {
    return generation.error();
  }
  head.generation = Generation{generation.value()};

  status = reader.field(kHeadAttempt, "head attempt");
  if (!status.ok()) {
    return status.error();
  }
  auto attempt = reader.u64();
  if (!attempt.ok()) {
    return attempt.error();
  }
  head.attempt = AttemptId{attempt.value()};

  status = reader.field(kHeadIncarnation, "head incarnation");
  if (!status.ok()) {
    return status.error();
  }
  auto incarnation_text = reader.bytes(64, "head incarnation");
  if (!incarnation_text.ok()) {
    return incarnation_text.error();
  }
  auto incarnation = IncarnationId::parse(incarnation_text.value());
  if (!incarnation.has_value()) {
    return make_error(ErrorCode::Corruption, "published manifest incarnation is malformed",
                      "incarnation");
  }
  head.incarnation = *incarnation;

  status = reader.field(kHeadEpoch, "head epoch");
  if (!status.ok()) {
    return status.error();
  }
  auto epoch = reader.u64();
  if (!epoch.ok()) {
    return epoch.error();
  }
  head.epoch = Epoch{epoch.value()};

  status = reader.field(kHeadRecordedAt, "head recorded_at");
  if (!status.ok()) {
    return status.error();
  }
  auto recorded = reader.u64();
  if (!recorded.ok()) {
    return recorded.error();
  }
  head.recorded_at = Tick{recorded.value()};

  status = reader.field(kHeadClockDomain, "head clock domain");
  if (!status.ok()) {
    return status.error();
  }
  auto domain = reader.token(kMaxClockDomainBytes, "clock domain");
  if (!domain.ok()) {
    return domain.error();
  }
  auto parsed_domain = ClockDomain::create(domain.value());
  if (!parsed_domain.ok()) {
    return parsed_domain.error();
  }
  head.clock_domain = parsed_domain.value();

  status = reader.field(kHeadRecordDigest, "head record digest");
  if (!status.ok()) {
    return status.error();
  }
  auto digest_text = reader.bytes(64, "head record digest");
  if (!digest_text.ok()) {
    return digest_text.error();
  }
  auto digest = Digest::from_hex(digest_text.value());
  if (!digest.has_value()) {
    return make_error(ErrorCode::Corruption, "published manifest record digest is malformed",
                      "digest");
  }
  head.record_digest = *digest;

  status = reader.field(kHeadRecordLength, "head record length");
  if (!status.ok()) {
    return status.error();
  }
  auto length = reader.u64();
  if (!length.ok()) {
    return length.error();
  }
  head.record_length = length.value();
  if (head.record_length > static_cast<std::uint64_t>(kMaxRecordBytes) + kRecordHeaderBytes) {
    return make_error(ErrorCode::LimitExceeded,
                      "published manifest declares a record of " +
                          std::to_string(head.record_length) +
                          " bytes, above the maximum this build will read",
                      "declared_length");
  }

  status = reader.field(kHeadSelfDigest, "head self digest");
  if (!status.ok()) {
    return status.error();
  }
  auto self_text = reader.bytes(64, "head self digest");
  if (!self_text.ok()) {
    return self_text.error();
  }
  auto self = Digest::from_hex(self_text.value());
  if (!self.has_value()) {
    return make_error(ErrorCode::Corruption, "published manifest digest is malformed", "digest");
  }

  status = reader.field(kHeadEnd, "head end marker");
  if (!status.ok()) {
    return status.error();
  }
  status = reader.exhausted("published manifest");
  if (!status.ok()) {
    return status.error();
  }
  // Canonical round-trip, as for the marker.
  auto reencoded = encode_head(head);
  if (!reencoded.ok()) {
    return reencoded.error();
  }
  if (reencoded.value() != bytes) {
    return make_error(ErrorCode::Corruption,
                      "published manifest is not in canonical form; the file was modified after "
                      "it was written",
                      "canonical_form");
  }
  return head;
}

/// Parse the generation out of `record-<decimal>.crr`. Returns nullopt for any
/// name that does not match exactly, so a stray file is never mistaken for a
/// record.
std::optional<Generation> generation_from_record_filename(std::string_view name) {
  if (name.size() <= kRecordFilePrefix.size() + kRecordFileSuffix.size()) {
    return std::nullopt;
  }
  if (name.substr(0, kRecordFilePrefix.size()) != kRecordFilePrefix) {
    return std::nullopt;
  }
  if (name.substr(name.size() - kRecordFileSuffix.size()) != kRecordFileSuffix) {
    return std::nullopt;
  }
  const std::string_view digits =
      name.substr(kRecordFilePrefix.size(),
                  name.size() - kRecordFilePrefix.size() - kRecordFileSuffix.size());
  if (digits.empty() || digits.size() > 20) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  for (char c : digits) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u) {
      return std::nullopt;
    }
    value = (value * 10u) + digit;
  }
  return Generation{value};
}

std::string record_filename(Generation generation) {
  const std::string digits = std::to_string(generation.value());
  return std::string(kRecordFilePrefix) + std::string(20 - digits.size(), '0') + digits +
         std::string(kRecordFileSuffix);
}

/// Verify the digest chain link from a record back to its predecessor, when the
/// predecessor is still on disk. A retired predecessor is not an error: the
/// chain is allowed to have a floor. An unreadable or mismatching predecessor
/// is an error, because it means the committed history was rewritten.
Status verify_chain_link(const std::filesystem::path& root, const StoreImage& image,
                         const Limits& limits) {
  if (image.previous_record_digest.is_zero() || image.generation.value() == 0) {
    return Status::success();
  }
  const Generation previous{image.generation.value() - 1};
  const auto path = Store::record_path(root, previous);
  if (!platform::is_regular_file_no_reparse(path)) {
    return Status::success();
  }
  auto bytes = platform::read_file_bounded(path, limits.max_read_bytes);
  if (!bytes.ok()) {
    return bytes.error();
  }
  RecordHeader header;
  auto payload = unframe_record(bytes.value(), &header);
  if (!payload.ok()) {
    return payload.error();
  }
  if (!(header.payload_digest == image.previous_record_digest)) {
    return make_error(ErrorCode::Corruption,
                      "the chain link from generation " +
                          std::to_string(image.generation.value()) +
                          " to its predecessor does not match: a committed record was replaced",
                      "chain");
  }
  return Status::success();
}

std::vector<std::pair<Generation, std::filesystem::path>> list_records(
    const std::filesystem::path& directory) {
  std::vector<std::pair<Generation, std::filesystem::path>> found;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    const std::string name = entry.path().filename().string();
    auto generation = generation_from_record_filename(name);
    if (generation.has_value()) {
      found.emplace_back(*generation, entry.path());
    }
  }
  std::sort(found.begin(), found.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  return found;
}

}  // namespace

const char* to_string(RecoveryOutcome outcome) noexcept {
  switch (outcome) {
    case RecoveryOutcome::Created:
      return "created";
    case RecoveryOutcome::OpenedClean:
      return "opened_clean";
    case RecoveryOutcome::RecoveredFromPreviousCommit:
      return "recovered_from_previous_commit";
  }
  return "unrecognised";
}

// ---------------------------------------------------------------------------
// StoreImage
// ---------------------------------------------------------------------------

std::string StoreImage::canonical_bytes() const {
  CanonicalWriter writer;
  writer.field(kImageFormat);
  writer.u32(format_version);
  writer.field(kImageIdentity);
  writer.bytes(store_identity.to_hex());
  writer.field(kImageGeneration);
  writer.u64(generation.value());
  writer.field(kImageAttempt);
  writer.u64(attempt.value());
  writer.field(kImageIncarnation);
  writer.bytes(incarnation.to_string());
  writer.field(kImageEpoch);
  writer.u64(epoch.value());
  writer.field(kImageRecordedAt);
  writer.u64(recorded_at.value());
  writer.field(kImageClockDomain);
  writer.token(clock_domain.name());
  writer.field(kImagePreviousRecord);
  writer.bytes(previous_record_digest.to_hex());
  writer.field(kImageRecovered);
  writer.boolean(recovered_from_previous_commit);
  writer.field(kImageRecoveryNote);
  writer.token(recovery_note.empty() ? std::string_view("none")
                                     : std::string_view(recovery_note));

  writer.field(kImageEvidenceCount);
  writer.u64(static_cast<std::uint64_t>(evidence.size()));
  for (const EvidenceItem& item : evidence) {
    writer.field(kImageEvidence);
    writer.bytes(item.canonical_bytes());
  }

  writer.field(kImageAttributionCount);
  writer.u64(static_cast<std::uint64_t>(attributions.size()));
  for (const AttributionItem& item : attributions) {
    writer.field(kImageAttribution);
    writer.bytes(item.canonical_bytes());
  }

  writer.field(kImageRunCount);
  writer.u64(static_cast<std::uint64_t>(runs.size()));
  for (const ReconciliationRun& run : runs) {
    writer.field(kImageRun);
    encode_run_envelope(writer, run);
  }

  writer.field(kImageHistoryCount);
  writer.u64(static_cast<std::uint64_t>(history.size()));
  for (const HistoryEntry& entry : history) {
    writer.field(kImageHistory);
    writer.field(kHistSequence);
    writer.u64(entry.sequence);
    writer.field(kHistKind);
    writer.token(to_string(entry.kind));
    writer.field(kHistGeneration);
    writer.u64(entry.generation.value());
    writer.field(kHistAttempt);
    writer.u64(entry.attempt.value());
    writer.field(kHistHasRun);
    writer.boolean(entry.run.has_value());
    if (entry.run.has_value()) {
      writer.field(kHistRun);
      writer.bytes(entry.run->to_string());
    }
    writer.field(kHistHasRelated);
    writer.boolean(entry.related_run.has_value());
    if (entry.related_run.has_value()) {
      writer.field(kHistRelated);
      writer.bytes(entry.related_run->to_string());
    }
    writer.field(kHistSubject);
    writer.bytes(entry.subject_digest.to_hex());
    writer.field(kHistDetail);
    writer.token(entry.detail.empty() ? std::string_view("none")
                                      : std::string_view(entry.detail));
    writer.field(kHistRecordedAt);
    writer.u64(entry.recorded_at.value());
    writer.field(kHistEnd);
  }
  writer.field(kImageHistorySequence);
  writer.u64(history_sequence);
  writer.field(kImageEnd);
  return std::move(writer).take();
}

Result<StoreImage> StoreImage::decode(std::string_view bytes, const Limits& limits) {
  CanonicalReader reader(bytes);
  StoreImage image;

  auto status = reader.field(kImageFormat, "image format");
  if (!status.ok()) {
    return status.error();
  }
  auto format = reader.u32();
  if (!format.ok()) {
    return format.error();
  }
  image.format_version = format.value();
  if (image.format_version != kStoreFormatVersion) {
    return make_error(ErrorCode::IncompatibleVersion,
                      "store image format version " + std::to_string(image.format_version) +
                          " is not readable by this build",
                      "format_version");
  }

  status = reader.field(kImageIdentity, "image identity");
  if (!status.ok()) {
    return status.error();
  }
  auto identity_text = reader.bytes(64, "image identity");
  if (!identity_text.ok()) {
    return identity_text.error();
  }
  auto identity = Digest::from_hex(identity_text.value());
  if (!identity.has_value()) {
    return make_error(ErrorCode::Corruption, "store image identity is malformed", "identity");
  }
  image.store_identity = *identity;

  status = reader.field(kImageGeneration, "image generation");
  if (!status.ok()) {
    return status.error();
  }
  auto generation = reader.u64();
  if (!generation.ok()) {
    return generation.error();
  }
  image.generation = Generation{generation.value()};

  status = reader.field(kImageAttempt, "image attempt");
  if (!status.ok()) {
    return status.error();
  }
  auto attempt = reader.u64();
  if (!attempt.ok()) {
    return attempt.error();
  }
  image.attempt = AttemptId{attempt.value()};

  status = reader.field(kImageIncarnation, "image incarnation");
  if (!status.ok()) {
    return status.error();
  }
  auto incarnation_text = reader.bytes(64, "image incarnation");
  if (!incarnation_text.ok()) {
    return incarnation_text.error();
  }
  auto incarnation = IncarnationId::parse(incarnation_text.value());
  if (!incarnation.has_value()) {
    return make_error(ErrorCode::Corruption, "store image incarnation is malformed",
                      "incarnation");
  }
  image.incarnation = *incarnation;

  status = reader.field(kImageEpoch, "image epoch");
  if (!status.ok()) {
    return status.error();
  }
  auto epoch = reader.u64();
  if (!epoch.ok()) {
    return epoch.error();
  }
  image.epoch = Epoch{epoch.value()};

  status = reader.field(kImageRecordedAt, "image recorded_at");
  if (!status.ok()) {
    return status.error();
  }
  auto recorded = reader.u64();
  if (!recorded.ok()) {
    return recorded.error();
  }
  image.recorded_at = Tick{recorded.value()};

  status = reader.field(kImageClockDomain, "image clock domain");
  if (!status.ok()) {
    return status.error();
  }
  auto domain = reader.token(kMaxClockDomainBytes, "clock domain");
  if (!domain.ok()) {
    return domain.error();
  }
  auto parsed_domain = ClockDomain::create(domain.value());
  if (!parsed_domain.ok()) {
    return parsed_domain.error();
  }
  image.clock_domain = parsed_domain.value();

  status = reader.field(kImagePreviousRecord, "image previous record digest");
  if (!status.ok()) {
    return status.error();
  }
  auto previous_text = reader.bytes(64, "previous record digest");
  if (!previous_text.ok()) {
    return previous_text.error();
  }
  auto previous = Digest::from_hex(previous_text.value());
  if (!previous.has_value()) {
    return make_error(ErrorCode::Corruption, "store image chain digest is malformed", "digest");
  }
  image.previous_record_digest = *previous;

  status = reader.field(kImageRecovered, "image recovered flag");
  if (!status.ok()) {
    return status.error();
  }
  auto recovered = reader.boolean();
  if (!recovered.ok()) {
    return recovered.error();
  }
  image.recovered_from_previous_commit = recovered.value();

  status = reader.field(kImageRecoveryNote, "image recovery note");
  if (!status.ok()) {
    return status.error();
  }
  auto note = reader.token(kMaxDetailBytes, "recovery note");
  if (!note.ok()) {
    return note.error();
  }
  image.recovery_note = note.value() == "none" ? std::string() : note.value();

  status = reader.field(kImageEvidenceCount, "evidence count");
  if (!status.ok()) {
    return status.error();
  }
  auto evidence_count = reader.u64();
  if (!evidence_count.ok()) {
    return evidence_count.error();
  }
  if (evidence_count.value() > static_cast<std::uint64_t>(limits.max_evidence_items)) {
    return make_error(ErrorCode::LimitExceeded,
                      "store image declares " + std::to_string(evidence_count.value()) +
                          " evidence items, above the limit of " +
                          std::to_string(limits.max_evidence_items),
                      "evidence_items");
  }
  image.evidence.reserve(static_cast<std::size_t>(evidence_count.value()));
  for (std::uint64_t i = 0; i < evidence_count.value(); ++i) {
    status = reader.field(kImageEvidence, "evidence item");
    if (!status.ok()) {
      return status.error();
    }
    auto raw = reader.bytes(limits.max_read_bytes, "evidence item");
    if (!raw.ok()) {
      return raw.error();
    }
    auto decoded = EvidenceItem::decode(raw.value());
    if (!decoded.ok()) {
      return decoded.error();
    }
    image.evidence.push_back(std::move(decoded.value()));
  }

  status = reader.field(kImageAttributionCount, "attribution count");
  if (!status.ok()) {
    return status.error();
  }
  auto attribution_count = reader.u64();
  if (!attribution_count.ok()) {
    return attribution_count.error();
  }
  if (attribution_count.value() > static_cast<std::uint64_t>(limits.max_attribution_items)) {
    return make_error(ErrorCode::LimitExceeded,
                      "store image declares " + std::to_string(attribution_count.value()) +
                          " attribution items, above the limit",
                      "attribution_items");
  }
  image.attributions.reserve(static_cast<std::size_t>(attribution_count.value()));
  for (std::uint64_t i = 0; i < attribution_count.value(); ++i) {
    status = reader.field(kImageAttribution, "attribution item");
    if (!status.ok()) {
      return status.error();
    }
    auto raw = reader.bytes(limits.max_read_bytes, "attribution item");
    if (!raw.ok()) {
      return raw.error();
    }
    auto decoded = AttributionItem::decode(raw.value());
    if (!decoded.ok()) {
      return decoded.error();
    }
    image.attributions.push_back(std::move(decoded.value()));
  }

  status = reader.field(kImageRunCount, "run count");
  if (!status.ok()) {
    return status.error();
  }
  auto run_count = reader.u64();
  if (!run_count.ok()) {
    return run_count.error();
  }
  if (run_count.value() > static_cast<std::uint64_t>(limits.max_runs_retained)) {
    return make_error(ErrorCode::LimitExceeded,
                      "store image declares " + std::to_string(run_count.value()) +
                          " runs, above the retained limit of " +
                          std::to_string(limits.max_runs_retained),
                      "runs");
  }
  image.runs.reserve(static_cast<std::size_t>(run_count.value()));
  for (std::uint64_t i = 0; i < run_count.value(); ++i) {
    status = reader.field(kImageRun, "run envelope");
    if (!status.ok()) {
      return status.error();
    }
    auto decoded = ReconciliationRun::decode_envelope(reader, limits);
    if (!decoded.ok()) {
      return decoded.error();
    }
    image.runs.push_back(std::move(decoded.value()));
  }

  status = reader.field(kImageHistoryCount, "history count");
  if (!status.ok()) {
    return status.error();
  }
  auto history_count = reader.u64();
  if (!history_count.ok()) {
    return history_count.error();
  }
  if (history_count.value() > static_cast<std::uint64_t>(limits.max_history_retained)) {
    return make_error(ErrorCode::LimitExceeded,
                      "store image declares " + std::to_string(history_count.value()) +
                          " history entries, above the retained limit",
                      "history");
  }
  image.history.reserve(static_cast<std::size_t>(history_count.value()));
  for (std::uint64_t i = 0; i < history_count.value(); ++i) {
    status = reader.field(kImageHistory, "history entry");
    if (!status.ok()) {
      return status.error();
    }
    HistoryEntry entry;

    status = reader.field(kHistSequence, "history sequence");
    if (!status.ok()) {
      return status.error();
    }
    auto sequence = reader.u64();
    if (!sequence.ok()) {
      return sequence.error();
    }
    entry.sequence = sequence.value();

    status = reader.field(kHistKind, "history kind");
    if (!status.ok()) {
      return status.error();
    }
    auto kind_text = reader.token(48, "history kind");
    if (!kind_text.ok()) {
      return kind_text.error();
    }
    auto kind = history_event_kind_from_string(kind_text.value());
    if (!kind.has_value()) {
      return make_error(ErrorCode::Malformed, "stored history kind is not recognised",
                        "history_kind");
    }
    entry.kind = *kind;

    status = reader.field(kHistGeneration, "history generation");
    if (!status.ok()) {
      return status.error();
    }
    auto hist_generation = reader.u64();
    if (!hist_generation.ok()) {
      return hist_generation.error();
    }
    entry.generation = Generation{hist_generation.value()};

    status = reader.field(kHistAttempt, "history attempt");
    if (!status.ok()) {
      return status.error();
    }
    auto hist_attempt = reader.u64();
    if (!hist_attempt.ok()) {
      return hist_attempt.error();
    }
    entry.attempt = AttemptId{hist_attempt.value()};

    status = reader.field(kHistHasRun, "history run presence");
    if (!status.ok()) {
      return status.error();
    }
    auto has_run = reader.boolean();
    if (!has_run.ok()) {
      return has_run.error();
    }
    if (has_run.value()) {
      status = reader.field(kHistRun, "history run id");
      if (!status.ok()) {
        return status.error();
      }
      auto run_text = reader.bytes(64, "history run id");
      if (!run_text.ok()) {
        return run_text.error();
      }
      auto run_id = ReconciliationRunId::parse(run_text.value());
      if (!run_id.has_value()) {
        return make_error(ErrorCode::Corruption, "stored history run identity is malformed",
                          "run_id");
      }
      entry.run = *run_id;
    }

    status = reader.field(kHistHasRelated, "history related presence");
    if (!status.ok()) {
      return status.error();
    }
    auto has_related = reader.boolean();
    if (!has_related.ok()) {
      return has_related.error();
    }
    if (has_related.value()) {
      status = reader.field(kHistRelated, "history related run id");
      if (!status.ok()) {
        return status.error();
      }
      auto related_text = reader.bytes(64, "history related run id");
      if (!related_text.ok()) {
        return related_text.error();
      }
      auto related = ReconciliationRunId::parse(related_text.value());
      if (!related.has_value()) {
        return make_error(ErrorCode::Corruption,
                          "stored history related identity is malformed", "run_id");
      }
      entry.related_run = *related;
    }

    status = reader.field(kHistSubject, "history subject digest");
    if (!status.ok()) {
      return status.error();
    }
    auto subject = reader.bytes(64, "history subject digest");
    if (!subject.ok()) {
      return subject.error();
    }
    auto subject_digest = Digest::from_hex(subject.value());
    if (!subject_digest.has_value()) {
      return make_error(ErrorCode::Corruption, "stored history subject digest is malformed",
                        "digest");
    }
    entry.subject_digest = *subject_digest;

    status = reader.field(kHistDetail, "history detail");
    if (!status.ok()) {
      return status.error();
    }
    auto detail = reader.token(kMaxDetailBytes, "history detail");
    if (!detail.ok()) {
      return detail.error();
    }
    entry.detail = detail.value() == "none" ? std::string() : detail.value();

    status = reader.field(kHistRecordedAt, "history recorded_at");
    if (!status.ok()) {
      return status.error();
    }
    auto recorded_at = reader.u64();
    if (!recorded_at.ok()) {
      return recorded_at.error();
    }
    entry.recorded_at = Tick{recorded_at.value()};

    status = reader.field(kHistEnd, "history end marker");
    if (!status.ok()) {
      return status.error();
    }
    image.history.push_back(std::move(entry));
  }

  status = reader.field(kImageHistorySequence, "history sequence");
  if (!status.ok()) {
    return status.error();
  }
  auto sequence = reader.u64();
  if (!sequence.ok()) {
    return sequence.error();
  }
  image.history_sequence = sequence.value();

  status = reader.field(kImageEnd, "image end marker");
  if (!status.ok()) {
    return status.error();
  }
  status = reader.exhausted("store image");
  if (!status.ok()) {
    return status.error();
  }

  auto validated = image.validate(limits);
  if (!validated.ok()) {
    return validated.error();
  }
  return image;
}

Status StoreImage::validate(const Limits& limits) const {
  if (format_version != kStoreFormatVersion) {
    return make_error(ErrorCode::IncompatibleVersion, "store image version is not current",
                      "format_version");
  }
  if (store_identity.is_zero()) {
    return make_error(ErrorCode::Corruption, "store image carries no store identity", "identity");
  }
  if (clock_domain.empty()) {
    return make_error(ErrorCode::Corruption, "store image carries no clock domain",
                      "clock_domain");
  }
  if (evidence.size() > limits.max_evidence_items) {
    return make_error(ErrorCode::LimitExceeded, "too many evidence items", "evidence_items");
  }
  if (attributions.size() > limits.max_attribution_items) {
    return make_error(ErrorCode::LimitExceeded, "too many attribution items",
                      "attribution_items");
  }
  if (runs.size() > limits.max_runs_retained) {
    return make_error(ErrorCode::LimitExceeded, "too many runs", "runs");
  }
  if (history.size() > limits.max_history_retained) {
    return make_error(ErrorCode::LimitExceeded, "too many history entries", "history");
  }
  for (std::size_t i = 1; i < evidence.size(); ++i) {
    if (!evidence_less(evidence[i - 1], evidence[i])) {
      return make_error(ErrorCode::Corruption,
                        "store image evidence is not strictly ordered or holds a duplicate",
                        "evidence_order");
    }
  }
  for (std::size_t i = 1; i < attributions.size(); ++i) {
    if (!(attributions[i - 1] < attributions[i])) {
      return make_error(ErrorCode::Corruption,
                        "store image attributions are not strictly ordered or hold a duplicate",
                        "attribution_order");
    }
  }
  for (std::size_t i = 0; i < runs.size(); ++i) {
    for (std::size_t j = i + 1; j < runs.size(); ++j) {
      if (runs[i].id() == runs[j].id()) {
        return make_error(ErrorCode::Corruption, "store image holds two runs with one identity",
                          "run_id");
      }
    }
    auto status = runs[i].validate(limits.max_cells_per_run);
    if (!status.ok()) {
      return status;
    }
    if (runs[i].committed_generation().has_value() &&
        runs[i].committed_generation()->value() > generation.value()) {
      return make_error(ErrorCode::Corruption,
                        "store image holds a run committed at a generation beyond the image",
                        "generation");
    }
  }
  for (std::size_t i = 1; i < history.size(); ++i) {
    if (history[i].sequence <= history[i - 1].sequence) {
      return make_error(ErrorCode::Corruption,
                        "store image history is not strictly ordered by sequence",
                        "history_order");
    }
  }
  if (history_sequence != (history.empty() ? 0u : history.back().sequence)) {
    return make_error(ErrorCode::Corruption,
                      "store image history sequence does not match the last entry",
                      "history_sequence");
  }
  if (recovered_from_previous_commit && recovery_note.empty()) {
    return make_error(ErrorCode::Corruption,
                      "store image records a recovery without a note explaining it",
                      "recovery_note");
  }
  return Status::success();
}

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------

struct Store::LockHolder {
  platform::ExclusiveFileLock lock;
};

Store::~Store() = default;
Store::Store(Store&&) noexcept = default;
Store& Store::operator=(Store&&) noexcept = default;

std::filesystem::path Store::marker_path(const std::filesystem::path& root) {
  return root / "CAPACITY-RECONCILIATION-STORE";
}
std::filesystem::path Store::lock_path(const std::filesystem::path& root) {
  return root / "LOCK";
}
std::filesystem::path Store::head_path(const std::filesystem::path& root) {
  return root / "HEAD";
}
std::filesystem::path Store::head_prev_path(const std::filesystem::path& root) {
  return root / "HEAD.prev";
}
std::filesystem::path Store::records_dir(const std::filesystem::path& root) {
  return root / "records";
}
std::filesystem::path Store::staging_dir(const std::filesystem::path& root) {
  return root / "staging";
}
std::filesystem::path Store::record_path(const std::filesystem::path& root, Generation generation) {
  return records_dir(root) / record_filename(generation);
}

Status Store::write_marker(const Limits& limits) {
  StoredMarker marker;
  marker.format_version = kStoreFormatVersion;
  marker.identity = image_.store_identity;
  marker.created_at = image_.recorded_at;
  auto encoded = encode_marker(marker);
  if (!encoded.ok()) {
    return encoded.error();
  }
  if (encoded.value().size() > kMaxMarkerBytes) {
    return make_error(ErrorCode::LimitExceeded, "store marker exceeds the size limit",
                      "marker_size");
  }
  auto path = marker_path(root_);
  auto status = platform::validate_child_path(root_, path);
  if (!status.ok()) {
    return status;
  }
  status = platform::write_file_durable(path, encoded.value(), true);
  if (!status.ok() && status.code() == ErrorCode::AlreadyExists) {
    // Another process created the marker between our existence check and our
    // write. Re-reading is correct: the marker is immutable after creation.
    auto existing = platform::read_file_bounded(path, limits.max_read_bytes < kMaxMarkerBytes
                                                          ? limits.max_read_bytes
                                                          : kMaxMarkerBytes);
    if (!existing.ok()) {
      return existing.error();
    }
    auto decoded = decode_marker(existing.value());
    if (!decoded.ok()) {
      return decoded.error();
    }
    if (!(decoded.value().identity == image_.store_identity)) {
      return make_error(ErrorCode::Conflict,
                        "another writer created this store with a different identity",
                        "identity");
    }
    return Status::success();
  }
  return status;
}

Status Store::adopt_image(StoreImage image, RecoveryReport* report) {
  auto status = image.validate(limits_);
  if (!status.ok()) {
    return status;
  }
  image_ = std::move(image);
  // Fresh incarnation: authority granted before this open is fenced. Nothing
  // recovered is treated as freshly observed; freshness is carried by evidence
  // stamps and re-evaluated by the kernel at the caller's evaluation instant.
  image_.incarnation = IncarnationId::generate();
  if (report != nullptr) {
    report->generation = image_.generation;
    report->attempt = image_.attempt;
  }
  return Status::success();
}

Result<std::pair<RecoveryReport, StoreImage>> read_published(const std::filesystem::path& root,
                                                             const Limits& limits) {
  RecoveryReport report;
  auto head_bytes = platform::read_file_bounded(Store::head_path(root), kMaxHeadBytes);
  if (!head_bytes.ok()) {
    return head_bytes.error();
  }
  auto head = decode_head(head_bytes.value());
  if (!head.ok()) {
    return head.error();
  }
  report.generation = head.value().generation;
  report.attempt = head.value().attempt;
  report.record_digest = head.value().record_digest;

  auto record_bytes =
      platform::read_file_bounded(Store::record_path(root, head.value().generation),
                                  limits.max_read_bytes);
  if (!record_bytes.ok()) {
    return record_bytes.error();
  }
  if (static_cast<std::uint64_t>(record_bytes.value().size()) != head.value().record_length) {
    return make_error(ErrorCode::Corruption,
                      "published record length does not match the published manifest",
                      "length_mismatch");
  }
  RecordHeader header;
  auto payload = unframe_record(record_bytes.value(), &header);
  if (!payload.ok()) {
    return payload.error();
  }
  if (!(header.payload_digest == head.value().record_digest)) {
    return make_error(ErrorCode::Corruption,
                      "published record content does not match the published manifest digest",
                      "payload_digest");
  }
  if (!(header.store_identity == head.value().identity)) {
    return make_error(ErrorCode::Conflict,
                      "record belongs to a different store identity than the manifest that "
                      "publishes it",
                      "identity");
  }
  if (!(header.generation == head.value().generation) ||
      !(header.attempt == head.value().attempt)) {
    return make_error(ErrorCode::Corruption,
                      "record generation or attempt does not match the published manifest",
                      "generation");
  }
  auto image = StoreImage::decode(payload.value(), limits);
  if (image.ok() && !(header.previous_record_digest == image.value().previous_record_digest)) {
    return make_error(ErrorCode::Corruption,
                      "record header chain link does not match the payload's chain link",
                      "chain");
  }
  if (!image.ok()) {
    return image.error();
  }
  if (!(image.value().store_identity == head.value().identity)) {
    return make_error(ErrorCode::Conflict,
                      "store image identity does not match the published manifest", "identity");
  }
  if (!(image.value().generation == head.value().generation)) {
    return make_error(ErrorCode::Corruption,
                      "store image generation does not match the published manifest",
                      "generation");
  }
  auto chain = verify_chain_link(root, image.value(), limits);
  if (!chain.ok()) {
    return chain.error();
  }
  return std::make_pair(report, std::move(image.value()));
}

Status Store::load_from_head(const Limits& limits, RecoveryReport* report) {
  const auto head_path = Store::head_path(root_);
  const auto prev_path = Store::head_prev_path(root_);

  const bool head_exists = platform::is_regular_file_no_reparse(head_path);
  const auto present = list_records(records_dir(root_));

  if (!head_exists) {
    if (!present.empty()) {
      return make_error(ErrorCode::Corruption,
                        "the store holds " + std::to_string(present.size()) +
                            " committed records but no published manifest: the commit point is "
                            "missing, so no state can be adopted whole",
                        "head_missing");
    }
    RecoveryReport created;
    created.outcome = RecoveryOutcome::Created;
    return adopt_image(image_, &created);
  }

  // `failure` carries the original typed error so that the distinction between
  // malformed, wrong-version, oversized, corrupted and conflicting state
  // survives to the caller. Collapsing every verification failure into one code
  // would throw away exactly the information an operator needs.
  Error failure = make_error(ErrorCode::Corruption, "the published manifest did not verify",
                             "head");
  bool failed = false;
  {
    auto head_bytes = platform::read_file_bounded(head_path, kMaxHeadBytes);
    if (!head_bytes.ok()) {
      failure = head_bytes.error();
      failed = true;
    } else {
      auto head = decode_head(head_bytes.value());
      if (!head.ok()) {
        failure = head.error();
        failed = true;
      } else if (!(head.value().identity == image_.store_identity)) {
        failure = make_error(ErrorCode::Conflict,
                              "the published manifest belongs to a different store identity "
                              "than the store marker",
                              "identity");
        failed = true;
      } else {
        auto record_bytes = platform::read_file_bounded(
            Store::record_path(root_, head.value().generation), limits.max_read_bytes);
        if (!record_bytes.ok()) {
          failure = record_bytes.error();
          failed = true;
        } else if (static_cast<std::uint64_t>(record_bytes.value().size()) !=
                   head.value().record_length) {
          failure = make_error(ErrorCode::Corruption,
                                "published record length does not match the published manifest",
                                "length_mismatch");
          failed = true;
        } else {
          RecordHeader record_header;
          auto payload = unframe_record(record_bytes.value(), &record_header);
          if (!payload.ok()) {
            failure = payload.error();
            failed = true;
          } else if (!(record_header.payload_digest == head.value().record_digest)) {
            failure = make_error(ErrorCode::Corruption,
                                  "published record content does not match the published "
                                  "manifest digest",
                                  "payload_digest");
            failed = true;
          } else if (!(record_header.store_identity == head.value().identity)) {
            failure = make_error(ErrorCode::Conflict,
                                  "the published record belongs to a different store identity",
                                  "identity");
            failed = true;
          } else if (!(record_header.generation == head.value().generation) ||
                     !(record_header.attempt == head.value().attempt)) {
            failure = make_error(ErrorCode::Corruption,
                                  "published record generation or attempt does not match the "
                                  "manifest",
                                  "generation");
            failed = true;
          } else {
            auto image = StoreImage::decode(payload.value(), limits);
            if (!image.ok()) {
              failure = image.error();
              failed = true;
            } else if (!(image.value().store_identity == image_.store_identity)) {
              failure = make_error(ErrorCode::Conflict,
                                   "the published record belongs to a different store identity",
                                   "identity");
              failed = true;
            } else if (!(image.value().generation == head.value().generation)) {
              failure = make_error(ErrorCode::Corruption,
                                    "published record generation does not match the manifest",
                                    "generation");
              failed = true;
            } else if (!(record_header.previous_record_digest ==
                         image.value().previous_record_digest)) {
              // The header's chain link is duplicated inside the payload. Both
              // must agree, otherwise an edit to the header would go unnoticed.
              failure = make_error(ErrorCode::Corruption,
                                   "published record header chain link does not match the "
                                   "payload's",
                                   "chain");
              failed = true;
            } else if (auto chain = verify_chain_link(root_, image.value(), limits);
                       !chain.ok()) {
              failure = chain.error();
              failed = true;
            } else {
              // Every check passed: this is the whole authoritative state.
              if (report != nullptr) {
                report->outcome = RecoveryOutcome::OpenedClean;
                report->generation = image.value().generation;
                report->attempt = image.value().attempt;
                report->record_digest = record_header.payload_digest;
              }
              return adopt_image(std::move(image.value()), nullptr);
            }
          }
        }
      }
    }
  }

  // The published manifest did not verify. The only honest recovery is to adopt
  // the previous whole commit -- never to merge, repair or partially read the
  // failed one.
  // Every failure below reports the *original* typed error, annotated with the
  // fact that no fallback was available. Reporting a single generic corruption
  // code here would erase the difference between a truncated file, a foreign
  // version, an oversized declaration and a genuine conflict -- which is the
  // difference an operator needs in order to act.
  const auto with_note = [&failure, failed](std::string_view prefix) {
    Error annotated = failure;
    annotated.message = std::string(prefix) + ": " + failure.message;
    if (failed && annotated.code == ErrorCode::Ok) {
      annotated.code = ErrorCode::Corruption;
    }
    return annotated;
  };
  if (!platform::is_regular_file_no_reparse(prev_path)) {
    return with_note("the published manifest failed verification and no previous commit is "
                     "available");
  }
  auto prev_bytes = platform::read_file_bounded(prev_path, kMaxHeadBytes);
  if (!prev_bytes.ok()) {
    return with_note(
        "the published manifest failed verification and the previous manifest could not be read");
  }
  auto prev_head = decode_head(prev_bytes.value());
  if (!prev_head.ok()) {
    return with_note(
        "the published manifest failed verification and the previous manifest is unreadable");
  }
  if (!(prev_head.value().identity == image_.store_identity)) {
    Error conflict = make_error(ErrorCode::Conflict,
                                "the previous commit belongs to a different store identity",
                                "identity");
    conflict.message += ": " + failure.message;
    return conflict;
  }
  auto prev_record = platform::read_file_bounded(
      Store::record_path(root_, prev_head.value().generation), limits.max_read_bytes);
  if (!prev_record.ok()) {
    return with_note(
        "the published manifest failed verification and the previous record could not be read");
  }
  RecordHeader prev_header;
  auto prev_payload = unframe_record(prev_record.value(), &prev_header);
  if (!prev_payload.ok()) {
    return with_note(
        "the published manifest failed verification and the previous record did not verify");
  }
  if (!(prev_header.payload_digest == prev_head.value().record_digest)) {
    Error mismatch = make_error(ErrorCode::Corruption,
                                "the previous record content does not match its manifest",
                                "payload_digest");
    mismatch.message += ": " + failure.message;
    return mismatch;
  }
  auto prev_image = StoreImage::decode(prev_payload.value(), limits);
  if (!prev_image.ok()) {
    Error undecodable = prev_image.error();
    undecodable.message += ": " + failure.message;
    return undecodable;
  }
  StoreImage recovered = std::move(prev_image.value());
  recovered.recovered_from_previous_commit = true;
  // A stable token, not free text: the note names the *kind* of failure the
  // fallback was caused by, and nothing about it may vary run to run.
  recovered.recovery_note = std::string("head_failed_") + to_string(failure.code);
  if (report != nullptr) {
    report->outcome = RecoveryOutcome::RecoveredFromPreviousCommit;
    report->generation = recovered.generation;
    report->attempt = recovered.attempt;
    report->record_digest = prev_header.payload_digest;
    report->note = recovered.recovery_note + ": " + failure.message;
  }
  auto adopted = adopt_image(std::move(recovered), nullptr);
  if (!adopted.ok()) {
    return adopted.error();
  }
  if (report != nullptr) {
    report->generation = image_.generation;
    report->attempt = image_.attempt;
  }
  return Status::success();
}

Status Store::cleanup_staging(RecoveryReport* report) {
  const auto staging = Store::staging_dir(root_);
  std::error_code ec;
  if (!std::filesystem::exists(staging, ec)) {
    return Status::success();
  }
  bool removed_any = false;
  for (const auto& entry : std::filesystem::directory_iterator(staging, ec)) {
    if (ec) {
      return make_error(ErrorCode::IoFailure,
                        "enumerating the staging directory failed: " + ec.message(), "os");
    }
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    auto status = platform::remove_file_if_present(entry.path());
    if (!status.ok()) {
      return status;
    }
    removed_any = true;
  }
  if (report != nullptr && removed_any) {
    report->staging_residue_removed = true;
  }
  return Status::success();
}

Status Store::retire_records(const Limits& limits, RecoveryReport* report) {
  const auto records = list_records(Store::records_dir(root_));
  if (records.size() <= limits.max_records_retained) {
    return Status::success();
  }

  std::vector<Generation> referenced;
  if (image_.generation.value() != 0) {
    referenced.push_back(image_.generation);
  }
  {
    auto prev_bytes = platform::read_file_bounded(Store::head_prev_path(root_), kMaxHeadBytes);
    if (prev_bytes.ok()) {
      auto prev_head = decode_head(prev_bytes.value());
      if (prev_head.ok()) {
        referenced.push_back(prev_head.value().generation);
      }
    }
  }

  std::uint64_t retired = 0;
  for (const auto& entry : records) {
    if (records.size() - retired <= limits.max_records_retained) {
      break;
    }
    if (std::find(referenced.begin(), referenced.end(), entry.first) != referenced.end()) {
      continue;
    }
    // Only a record this store recognises as its own is ever removed. A file
    // that merely happens to have a record-shaped name is left alone: deleting
    // state we cannot identify would be worse than holding extra bytes.
    auto bytes = platform::read_file_bounded(entry.second, limits.max_read_bytes);
    if (!bytes.ok()) {
      continue;
    }
    RecordHeader header;
    auto payload = unframe_record(bytes.value(), &header);
    if (!payload.ok() || !(header.store_identity == image_.store_identity)) {
      continue;
    }
    auto status = platform::remove_file_if_present(entry.second);
    if (!status.ok()) {
      return status;
    }
    ++retired;
  }
  if (report != nullptr) {
    report->records_retired = retired;
  }
  return Status::success();
}

Result<Store> Store::open(const std::filesystem::path& root, const StoreOpenOptions& options) {
  if (root.empty()) {
    return make_error(ErrorCode::InvalidArgument, "store root must be a non-empty path", "path");
  }
  std::error_code ec;
  const std::filesystem::path absolute = std::filesystem::absolute(root, ec).lexically_normal();
  if (ec) {
    return make_error(ErrorCode::InvalidArgument,
                      "store root could not be resolved to an absolute path: " + ec.message(),
                      "path");
  }

  const bool exists = std::filesystem::exists(absolute, ec);
  if (ec) {
    return make_error(ErrorCode::IoFailure,
                      "inspecting the store root failed: " + ec.message(), "os");
  }
  if (!exists && !options.create_if_missing) {
    return make_error(ErrorCode::NotFound,
                      "store root does not exist and create_if_missing is not set: '" +
                          absolute.string() + "'",
                      "path");
  }
  if (!exists && !options.writer) {
    return make_error(ErrorCode::NotFound,
                      "store root does not exist: '" + absolute.string() + "'", "path");
  }

  Store store;
  store.root_ = absolute;
  store.limits_ = options.limits;
  store.writer_ = options.writer;
  store.mutex_ = std::make_unique<std::mutex>();

  if (options.writer) {
    auto status = platform::ensure_directory(absolute);
    if (!status.ok()) {
      return status.error();
    }
    status = platform::ensure_directory(Store::records_dir(absolute));
    if (!status.ok()) {
      return status.error();
    }
    status = platform::ensure_directory(Store::staging_dir(absolute));
    if (!status.ok()) {
      return status.error();
    }

    auto lock = platform::ExclusiveFileLock::acquire(Store::lock_path(absolute));
    if (!lock.ok()) {
      return lock.error();
    }
    store.lock_ = std::make_unique<LockHolder>();
    store.lock_->lock = std::move(lock.value());
  } else if (!std::filesystem::is_directory(absolute, ec)) {
    return make_error(ErrorCode::InvalidArgument,
                      "store root exists and is not a directory: '" + absolute.string() + "'",
                      "path");
  }

  // Marker.
  const auto marker = Store::marker_path(absolute);
  if (platform::is_regular_file_no_reparse(marker)) {
    auto bytes = platform::read_file_bounded(marker, kMaxMarkerBytes);
    if (!bytes.ok()) {
      return bytes.error();
    }
    auto decoded = decode_marker(bytes.value());
    if (!decoded.ok()) {
      return decoded.error();
    }
    store.image_.store_identity = decoded.value().identity;
    store.image_.recorded_at = decoded.value().created_at;
    // A store always has a clock domain, even before its first commit: state
    // validation requires one, and a store that cannot be inspected read-only
    // until someone writes to it would be a defect in its own right.
    store.image_.clock_domain = ClockDomain{"facility-default"};
  } else if (options.create_if_missing) {
    if (!options.writer) {
      return make_error(ErrorCode::PermissionDenied,
                        "creating a store requires writer access", "writer");
    }
    store.image_.store_identity = Sha256::hash(IncarnationId::generate().to_string());
    store.image_.recorded_at = Tick{0};
    store.image_.clock_domain = ClockDomain{"facility-default"};
    auto status = store.write_marker(options.limits);
    if (!status.ok()) {
      return status.error();
    }
  } else {
    return make_error(ErrorCode::NotFound,
                      "directory is not a capacity reconciliation store (no marker): '" +
                          absolute.string() + "'",
                      "marker");
  }

  auto status = store.load_from_head(options.limits, &store.recovery_);
  if (!status.ok()) {
    return status.error();
  }
  if (store.image_.store_identity.is_zero()) {
    return make_error(ErrorCode::Corruption, "store identity could not be established",
                      "identity");
  }
  if (store.image_.clock_domain.empty()) {
    store.image_.clock_domain = ClockDomain{"facility-default"};
  }
  store.image_.format_version = kStoreFormatVersion;

  if (options.writer) {
    status = store.cleanup_staging(&store.recovery_);
    if (!status.ok()) {
      return status.error();
    }
    if (options.retire_unreferenced_records) {
      status = store.retire_records(options.limits, &store.recovery_);
      if (!status.ok()) {
        return status.error();
      }
    }
  }
  store.recovery_.outcome = store.recovery_.outcome;
  return store;
}

Result<RecoveryReport> Store::commit(StoreImage successor, Generation expected,
                                     const IncarnationId& expected_incarnation) {
  if (!writer_) {
    return make_error(ErrorCode::PermissionDenied,
                      "this store handle was opened read-only; committing is refused", "writer");
  }
  if (mutex_ == nullptr) {
    return make_error(ErrorCode::InvariantViolation, "store handle was not fully initialised",
                      "mutex");
  }
  const std::lock_guard<std::mutex> guard(*mutex_);
  if (!(expected == image_.generation)) {
    return generation_mismatch(expected.value(), image_.generation.value());
  }
  if (!(expected_incarnation == image_.incarnation)) {
    return make_error(ErrorCode::StaleAuthority,
                      "the caller holds an incarnation that is no longer current; authority "
                      "granted before this store was opened is fenced",
                      "incarnation");
  }
  if (successor.epoch < image_.epoch) {
    return make_error(ErrorCode::StaleAuthority, "epoch cannot move backwards", "epoch");
  }

  auto next_attempt = image_.attempt.next();
  if (!next_attempt.ok()) {
    return next_attempt.error();
  }
  auto next_generation = image_.generation.next();
  if (!next_generation.ok()) {
    return next_generation.error();
  }

  // Fields owned by the store are taken from the store, never from the caller.
  successor.format_version = kStoreFormatVersion;
  successor.store_identity = image_.store_identity;
  successor.generation = next_generation.value();
  successor.attempt = next_attempt.value();
  successor.incarnation = image_.incarnation;
  successor.previous_record_digest = Digest{};
  if (image_.generation.value() != 0) {
    auto head_bytes = platform::read_file_bounded(Store::head_path(root_), kMaxHeadBytes);
    if (head_bytes.ok()) {
      auto head = decode_head(head_bytes.value());
      if (head.ok()) {
        successor.previous_record_digest = head.value().record_digest;
      }
    }
  }
  if (successor.clock_domain.empty()) {
    successor.clock_domain = image_.clock_domain;
  }

  auto status = successor.validate(limits_);
  if (!status.ok()) {
    return status.error();
  }

  const std::string payload = successor.canonical_bytes();
  if (payload.size() > kMaxRecordBytes) {
    return make_error(ErrorCode::LimitExceeded,
                      "the committed state is " + std::to_string(payload.size()) +
                          " bytes, above the record limit of " + std::to_string(kMaxRecordBytes),
                      "payload_length");
  }

  RecordHeader header;
  header.format_version = kRecordFormatVersion;
  header.previous_record_digest = successor.previous_record_digest;
  header.store_identity = successor.store_identity;
  header.generation = successor.generation;
  header.attempt = successor.attempt;
  // frame_record recomputes this over the same bytes; computing it here too
  // means the header the *caller* holds carries the digest that the published
  // manifest and the read-back verification both compare against.
  header.payload_digest = Sha256::hash(payload);

  auto framed = frame_record(header, payload);
  if (!framed.ok()) {
    return framed.error();
  }

  const auto staging = Store::staging_dir(root_);
  const auto staged_record = staging / record_filename(successor.generation);
  const auto staged_head = staging / "HEAD.next";

  status = platform::remove_file_if_present(staged_record);
  if (!status.ok()) {
    return status.error();
  }
  status = platform::remove_file_if_present(staged_head);
  if (!status.ok()) {
    return status.error();
  }

  // Step 4: stage and flush.
  status = platform::write_file_durable(staged_record, framed.value(), false);
  if (!status.ok()) {
    return status.error();
  }

  // Step 5: verify by reading the staged bytes back from the medium.
  {
    auto read_back = platform::read_file_bounded(staged_record, limits_.max_read_bytes);
    if (!read_back.ok()) {
      (void)platform::remove_file_if_present(staged_record);
      return make_error(ErrorCode::IoFailure,
                        "staged record could not be read back for verification: " +
                            read_back.error().message,
                        "verify");
    }
    if (read_back.value().size() != framed.value().size()) {
      (void)platform::remove_file_if_present(staged_record);
      return make_error(ErrorCode::IoFailure,
                        "staged record length changed between write and read back", "verify");
    }
    RecordHeader verified_header;
    auto verified_payload = unframe_record(read_back.value(), &verified_header);
    if (!verified_payload.ok()) {
      (void)platform::remove_file_if_present(staged_record);
      return make_error(ErrorCode::IoFailure,
                        "staged record failed verification: " + verified_payload.error().message,
                        "verify");
    }
    if (!(verified_header.payload_digest == header.payload_digest)) {
      (void)platform::remove_file_if_present(staged_record);
      return make_error(ErrorCode::IoFailure,
                        "staged record digest changed between write and read back", "verify");
    }
    auto verified_image = StoreImage::decode(verified_payload.value(), limits_);
    if (!verified_image.ok()) {
      (void)platform::remove_file_if_present(staged_record);
      return make_error(ErrorCode::IoFailure,
                        "staged record content did not decode: " +
                            verified_image.error().message,
                        "verify");
    }
    if (!(verified_image.value().generation == successor.generation)) {
      (void)platform::remove_file_if_present(staged_record);
      return make_error(ErrorCode::IoFailure,
                        "staged record generation does not match the reserved generation",
                        "verify");
    }
  }

  const auto final_record = Store::record_path(root_, successor.generation);
  status = platform::atomic_replace(staged_record, final_record);
  if (!status.ok()) {
    (void)platform::remove_file_if_present(staged_record);
    return status.error();
  }

  StoredHead head;
  head.format_version = kStoreFormatVersion;
  head.identity = successor.store_identity;
  head.generation = successor.generation;
  head.attempt = successor.attempt;
  head.incarnation = successor.incarnation;
  head.epoch = successor.epoch;
  head.recorded_at = successor.recorded_at;
  head.clock_domain = successor.clock_domain;
  head.record_digest = header.payload_digest;
  head.record_length = static_cast<std::uint64_t>(framed.value().size());

  auto head_bytes = encode_head(head);
  if (!head_bytes.ok()) {
    return head_bytes.error();
  }
  if (head_bytes.value().size() > kMaxHeadBytes) {
    return make_error(ErrorCode::LimitExceeded, "published manifest exceeds the size limit",
                      "head_size");
  }

  // Preserve the outgoing manifest before it is replaced, so that a failure
  // after this point can still recover the previous whole state.
  const auto head_path = Store::head_path(root_);
  if (platform::is_regular_file_no_reparse(head_path)) {
    auto current = platform::read_file_bounded(head_path, kMaxHeadBytes);
    if (current.ok()) {
      const auto staged_prev = staging / "HEAD.prev.next";
      status = platform::remove_file_if_present(staged_prev);
      if (!status.ok()) {
        return status.error();
      }
      status = platform::write_file_durable(staged_prev, current.value(), false);
      if (!status.ok()) {
        return status.error();
      }
      status = platform::atomic_replace(staged_prev, Store::head_prev_path(root_));
      if (!status.ok()) {
        return status.error();
      }
    }
  }

  status = platform::write_file_durable(staged_head, head_bytes.value(), false);
  if (!status.ok()) {
    return status.error();
  }

  // Step 6: the commit point.
  status = platform::atomic_replace(staged_head, head_path);
  if (!status.ok()) {
    return status.error();
  }

  image_ = std::move(successor);

  // Step 7: cleanup.
  status = cleanup_staging(nullptr);
  if (!status.ok()) {
    return status.error();
  }
  RecoveryReport report;
  report.outcome = RecoveryOutcome::OpenedClean;
  report.generation = image_.generation;
  report.attempt = image_.attempt;
  report.record_digest = header.payload_digest;
  status = retire_records(limits_, &report);
  if (!status.ok()) {
    return status.error();
  }
  recovery_ = report;
  return report;
}

Result<RecoveryReport> Store::advance_epoch(Generation expected,
                                            const IncarnationId& expected_incarnation,
                                            Epoch new_epoch) {
  if (!(new_epoch > image_.epoch)) {
    return make_error(ErrorCode::InvalidArgument,
                      "epoch must advance: an unchanged or lower epoch does not fence anything",
                      "epoch");
  }
  StoreImage next = image_;
  next.epoch = new_epoch;
  return commit(std::move(next), expected, expected_incarnation);
}

Store::VerificationReport Store::verify(const std::filesystem::path& root, const Limits& limits) {
  VerificationReport report;
  std::error_code ec;
  const std::filesystem::path absolute = std::filesystem::absolute(root, ec).lexically_normal();
  if (ec) {
    report.problems.push_back("store root could not be resolved: " + ec.message());
    return report;
  }
  if (!std::filesystem::is_directory(absolute, ec)) {
    report.problems.push_back("store root is not a directory: " + absolute.string());
    return report;
  }

  auto marker_bytes =
      platform::read_file_bounded(Store::marker_path(absolute), kMaxMarkerBytes);
  if (!marker_bytes.ok()) {
    report.problems.push_back("marker: " + marker_bytes.error().to_string());
    return report;
  }
  auto marker = decode_marker(marker_bytes.value());
  if (!marker.ok()) {
    report.problems.push_back("marker: " + marker.error().to_string());
    return report;
  }

  if (!std::filesystem::is_directory(Store::records_dir(absolute), ec)) {
    report.problems.push_back("records directory is missing");
    return report;
  }
  for (const auto& entry : list_records(Store::records_dir(absolute))) {
    ++report.records_present;
    auto bytes = platform::read_file_bounded(entry.second, limits.max_read_bytes);
    if (!bytes.ok()) {
      report.problems.push_back("record " + std::to_string(entry.first.value()) +
                                ": " + bytes.error().to_string());
      continue;
    }
    RecordHeader header;
    auto payload = unframe_record(bytes.value(), &header);
    if (!payload.ok()) {
      report.problems.push_back("record " + std::to_string(entry.first.value()) +
                                ": " + payload.error().to_string());
      continue;
    }
    if (!(header.generation == entry.first)) {
      report.problems.push_back("record " + std::to_string(entry.first.value()) +
                                ": filename generation does not match the header");
      continue;
    }
    if (!(header.store_identity == marker.value().identity)) {
      report.problems.push_back("record " + std::to_string(entry.first.value()) +
                                ": belongs to a different store identity");
      continue;
    }
    auto image = StoreImage::decode(payload.value(), limits);
    if (!image.ok()) {
      report.problems.push_back("record " + std::to_string(entry.first.value()) +
                                ": " + image.error().to_string());
      continue;
    }
    if (image.value().history_sequence == 0 && !image.value().history.empty()) {
      report.problems.push_back("record " + std::to_string(entry.first.value()) +
                                ": history sequence is inconsistent");
    }
  }

  auto published = read_published(absolute, limits);
  if (!published.ok()) {
    // A store with no published commit yet is empty, not broken: it has been
    // created and nothing has been committed. Any other read failure is real.
    if (published.error().code == ErrorCode::NotFound && report.records_present == 0) {
      report.ok = report.problems.empty();
      return report;
    }
    report.problems.push_back("published state: " + published.error().to_string());
    return report;
  }
  report.generation = published.value().first.generation;
  report.record_digest = published.value().first.record_digest;
  report.records_referenced = 1;

  // Also verify the previous commit, which is what a torn publish would fall
  // back to. A store whose fallback is unreadable is not a healthy store.
  auto prev_bytes = platform::read_file_bounded(Store::head_prev_path(absolute), kMaxHeadBytes);
  if (prev_bytes.ok()) {
    auto prev_head = decode_head(prev_bytes.value());
    if (!prev_head.ok()) {
      report.problems.push_back("previous manifest: " + prev_head.error().to_string());
    } else {
      ++report.records_referenced;
      auto prev_record = platform::read_file_bounded(
          Store::record_path(absolute, prev_head.value().generation), limits.max_read_bytes);
      if (!prev_record.ok()) {
        report.problems.push_back("previous record: " + prev_record.error().to_string());
      } else {
        RecordHeader prev_header;
        auto prev_payload = unframe_record(prev_record.value(), &prev_header);
        if (!prev_payload.ok()) {
          report.problems.push_back("previous record: " + prev_payload.error().to_string());
        } else if (!(prev_header.payload_digest == prev_head.value().record_digest)) {
          report.problems.push_back(
              "previous record: content does not match the previous manifest digest");
        }
      }
    }
  }

  // Residue in the staging area means a previous commit did not complete its
  // cleanup, which is worth reporting rather than hiding.
  if (std::filesystem::is_directory(Store::staging_dir(absolute), ec)) {
    for (const auto& entry : std::filesystem::directory_iterator(Store::staging_dir(absolute), ec)) {
      if (!entry.is_regular_file(ec)) {
        continue;
      }
      report.problems.push_back("staging residue: " + entry.path().filename().string());
    }
  }

  report.ok = report.problems.empty();
  return report;
}

}  // namespace summon::capacity_reconciliation
