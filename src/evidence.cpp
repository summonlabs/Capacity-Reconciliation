// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/capacity_reconciliation/evidence.hpp"

#include <algorithm>
#include <array>

#include "summon/capacity_reconciliation/limits.hpp"
#include "summon/capacity_reconciliation/scope.hpp"

namespace summon::capacity_reconciliation {
namespace {

struct SourceToken {
  EvidenceSource source;
  const char* token;
};

constexpr std::array<SourceToken, 12> kSourceTokens{{
    {EvidenceSource::Unknown, "unknown"},
    {EvidenceSource::FacilityCapacity, "facility_capacity"},
    {EvidenceSource::RackCapacity, "rack_capacity"},
    {EvidenceSource::SpaceCapacity, "space_capacity"},
    {EvidenceSource::PowerCapacity, "power_capacity"},
    {EvidenceSource::CoolingCapacity, "cooling_capacity"},
    {EvidenceSource::ReservationRegister, "reservation_register"},
    {EvidenceSource::AssetRegistry, "asset_registry"},
    {EvidenceSource::RackRegistry, "rack_registry"},
    {EvidenceSource::LocationRegistry, "location_registry"},
    {EvidenceSource::ObservationStream, "observation_stream"},
    {EvidenceSource::OperatorDeclaration, "operator_declaration"},
}};

constexpr std::array<CapacityView, kCapacityViewCount> kViews{{
    CapacityView::Planned, CapacityView::Reserved, CapacityView::Installed,
    CapacityView::Observed, CapacityView::Usable, CapacityView::Allocatable,
}};

constexpr const char* kViewTokens[kCapacityViewCount] = {
    "planned", "reserved", "installed", "observed", "usable", "allocatable",
};

struct StatusToken {
  EvidenceStatus status;
  const char* token;
};

constexpr std::array<StatusToken, 6> kStatusTokens{{
    {EvidenceStatus::Accepted, "accepted"},
    {EvidenceStatus::Partial, "partial"},
    {EvidenceStatus::Expired, "expired"},
    {EvidenceStatus::Superseded, "superseded"},
    {EvidenceStatus::Unsupported, "unsupported"},
    {EvidenceStatus::Unavailable, "unavailable"},
}};

constexpr std::string_view kEvidenceDomain = "capacity-reconciliation/evidence/v1";
constexpr std::string_view kEvidenceSetDomain = "capacity-reconciliation/evidence-set/v1";

// Canonical field tags. They are part of the persisted format: renumbering
// them would make old records undecodable, which is why the format version is
// bumped whenever a tag changes.
enum Tag : std::uint8_t {
  kTagId = 1,
  kTagSource = 2,
  kTagSourceInstance = 3,
  kTagScope = 4,
  kTagDimension = 5,
  kTagView = 6,
  kTagKnown = 7,
  kTagValue = 8,
  kTagUnknownReason = 9,
  kTagStatus = 10,
  kTagGeneration = 11,
  kTagRevision = 12,
  kTagEpoch = 13,
  kTagIncarnation = 14,
  kTagObservedAt = 15,
  kTagValidUntil = 16,
  kTagClockDomain = 17,
  kTagContentDigest = 18,
  kTagEnd = 0x7F,
};

Result<std::string> read_scope(CanonicalReader& reader) {
  auto raw = reader.bytes(kMaxScopeDepth * (kMaxScopeSegmentBytes + 1u), "scope");
  if (!raw.ok()) {
    return raw.error();
  }
  auto parsed = ScopeIdentity::parse(raw.value());
  if (!parsed.ok()) {
    return make_error(ErrorCode::Malformed,
                      "stored scope is not a valid scope path: " + parsed.error().message, "scope");
  }
  if (parsed.value().to_string() != raw.value()) {
    return make_error(ErrorCode::Malformed,
                      "stored scope is not in canonical form: '" + raw.value() + "'", "scope");
  }
  return raw.value();
}

Result<DimensionKey> read_dimension(CanonicalReader& reader) {
  auto raw = reader.bytes(64, "dimension");
  if (!raw.ok()) {
    return raw.error();
  }
  auto parsed = DimensionKey::parse(raw.value());
  if (!parsed.has_value()) {
    return make_error(ErrorCode::Malformed,
                      "stored dimension key is not recognised: '" + raw.value() + "'",
                      "dimension");
  }
  return *parsed;
}

Result<Quantity> read_quantity(CanonicalReader& reader) {
  auto status = reader.field(kTagKnown, "quantity known flag");
  if (!status.ok()) {
    return status.error();
  }
  auto known = reader.boolean();
  if (!known.ok()) {
    return known.error();
  }
  if (known.value()) {
    auto status_value = reader.field(kTagValue, "quantity value");
    if (!status_value.ok()) {
      return status_value.error();
    }
    auto value = reader.i64();
    if (!value.ok()) {
      return value.error();
    }
    return Quantity::known(value.value());
  }
  auto status_reason = reader.field(kTagUnknownReason, "quantity unknown reason");
  if (!status_reason.ok()) {
    return status_reason.error();
  }
  auto reason = reader.token(kMaxReasonTokenBytes, "unknown reason");
  if (!reason.ok()) {
    return reason.error();
  }
  auto parsed = unknown_reason_from_string(reason.value());
  if (!parsed.has_value()) {
    return make_error(ErrorCode::Malformed,
                      "stored unknown reason is not recognised: '" + reason.value() + "'",
                      "unknown_reason");
  }
  return Quantity::unknown(*parsed);
}

void write_quantity(CanonicalWriter& writer, const Quantity& quantity) {
  writer.field(kTagKnown);
  writer.boolean(quantity.is_known());
  if (quantity.is_known()) {
    writer.field(kTagValue);
    writer.i64(quantity.value());
  } else {
    writer.field(kTagUnknownReason);
    writer.token(to_string(quantity.reason()));
  }
}

void write_stamp(CanonicalWriter& writer, const EvidenceStamp& stamp) {
  writer.field(kTagGeneration);
  writer.u64(stamp.generation.value());
  writer.field(kTagRevision);
  writer.u64(stamp.revision.value());
  writer.field(kTagEpoch);
  writer.u64(stamp.epoch.value());
  writer.field(kTagIncarnation);
  writer.bytes(stamp.incarnation.to_string());
  writer.field(kTagObservedAt);
  writer.u64(stamp.observed_at.value());
  writer.field(kTagValidUntil);
  writer.u64(stamp.valid_until.value());
  writer.field(kTagClockDomain);
  writer.token(stamp.clock_domain.name());
}

Result<EvidenceStamp> read_stamp(CanonicalReader& reader) {
  EvidenceStamp stamp;
  auto status = reader.field(kTagGeneration, "stamp generation");
  if (!status.ok()) {
    return status.error();
  }
  auto generation = reader.u64();
  if (!generation.ok()) {
    return generation.error();
  }
  stamp.generation = Generation{generation.value()};

  status = reader.field(kTagRevision, "stamp revision");
  if (!status.ok()) {
    return status.error();
  }
  auto revision = reader.u64();
  if (!revision.ok()) {
    return revision.error();
  }
  stamp.revision = Revision{revision.value()};

  status = reader.field(kTagEpoch, "stamp epoch");
  if (!status.ok()) {
    return status.error();
  }
  auto epoch = reader.u64();
  if (!epoch.ok()) {
    return epoch.error();
  }
  stamp.epoch = Epoch{epoch.value()};

  status = reader.field(kTagIncarnation, "stamp incarnation");
  if (!status.ok()) {
    return status.error();
  }
  auto incarnation = reader.bytes(64, "incarnation");
  if (!incarnation.ok()) {
    return incarnation.error();
  }
  auto parsed_incarnation = IncarnationId::parse(incarnation.value());
  if (!parsed_incarnation.has_value()) {
    return make_error(ErrorCode::Malformed, "stored incarnation identity is malformed",
                      "incarnation");
  }
  stamp.incarnation = *parsed_incarnation;

  status = reader.field(kTagObservedAt, "stamp observed_at");
  if (!status.ok()) {
    return status.error();
  }
  auto observed = reader.u64();
  if (!observed.ok()) {
    return observed.error();
  }
  stamp.observed_at = Tick{observed.value()};

  status = reader.field(kTagValidUntil, "stamp valid_until");
  if (!status.ok()) {
    return status.error();
  }
  auto valid = reader.u64();
  if (!valid.ok()) {
    return valid.error();
  }
  stamp.valid_until = Tick{valid.value()};

  status = reader.field(kTagClockDomain, "stamp clock domain");
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
  stamp.clock_domain = parsed_domain.value();
  return stamp;
}

Result<EvidenceSource> source_from_token(std::string_view token) {
  auto parsed = evidence_source_from_string(token);
  if (!parsed.has_value()) {
    return make_error(ErrorCode::Malformed,
                      "stored evidence source is not recognised: '" + std::string(token) + "'",
                      "evidence_source");
  }
  return *parsed;
}

Result<CapacityView> view_from_token(std::string_view token) {
  auto parsed = capacity_view_from_string(token);
  if (!parsed.has_value()) {
    return make_error(ErrorCode::Malformed,
                      "stored capacity view is not recognised: '" + std::string(token) + "'",
                      "capacity_view");
  }
  return *parsed;
}

Result<EvidenceStatus> status_from_token(std::string_view token) {
  auto parsed = evidence_status_from_string(token);
  if (!parsed.has_value()) {
    return make_error(ErrorCode::Malformed,
                      "stored evidence status is not recognised: '" + std::string(token) + "'",
                      "evidence_status");
  }
  return *parsed;
}

}  // namespace

const char* to_string(EvidenceSource source) noexcept {
  for (const SourceToken& entry : kSourceTokens) {
    if (entry.source == source) {
      return entry.token;
    }
  }
  return "unrecognised";
}

std::optional<EvidenceSource> evidence_source_from_string(std::string_view token) noexcept {
  for (const SourceToken& entry : kSourceTokens) {
    if (token == entry.token) {
      return entry.source;
    }
  }
  return std::nullopt;
}

const char* to_string(CapacityView view) noexcept {
  const std::size_t index = view_index(view);
  if (index >= kCapacityViewCount) {
    return "unrecognised";
  }
  return kViewTokens[index];
}

std::optional<CapacityView> capacity_view_from_string(std::string_view token) noexcept {
  for (CapacityView view : kViews) {
    if (token == to_string(view)) {
      return view;
    }
  }
  return std::nullopt;
}

const char* to_string(EvidenceStatus status) noexcept {
  for (const StatusToken& entry : kStatusTokens) {
    if (entry.status == status) {
      return entry.token;
    }
  }
  return "unrecognised";
}

std::optional<EvidenceStatus> evidence_status_from_string(std::string_view token) noexcept {
  for (const StatusToken& entry : kStatusTokens) {
    if (token == entry.token) {
      return entry.status;
    }
  }
  return std::nullopt;
}

Result<EvidenceItem> EvidenceItem::create(EvidenceId id, EvidenceSource source,
                                          std::string source_instance, ScopeIdentity scope,
                                          DimensionKey dimension, CapacityView view,
                                          Quantity value, EvidenceStatus status,
                                          EvidenceStamp stamp) {
  if (!id.valid()) {
    return make_error(ErrorCode::InvalidArgument, "evidence requires a generated identity",
                      "evidence_id");
  }
  if (source == EvidenceSource::Unknown) {
    return make_error(ErrorCode::InvalidArgument,
                      "evidence requires a concrete producing source family", "evidence_source");
  }
  auto instance = make_token(source_instance, kMaxSourceNameBytes, "source instance");
  if (!instance.ok()) {
    return instance.error();
  }
  if (scope.empty()) {
    return make_error(ErrorCode::InvalidArgument, "evidence requires a scope", "scope");
  }
  if (dimension.dimension() == CapacityDimension::None) {
    return make_error(ErrorCode::InvalidArgument, "evidence requires a dimension", "dimension");
  }
  if (stamp.clock_domain.empty()) {
    return make_error(ErrorCode::InvalidArgument,
                      "evidence requires a declared clock domain: ticks from an unnamed domain "
                      "cannot be compared with anything",
                      "clock_domain");
  }
  if (stamp.valid_until < stamp.observed_at) {
    return make_error(ErrorCode::InvalidArgument,
                      "evidence validity window is inverted: valid_until precedes observed_at",
                      "validity_window");
  }

  EvidenceItem item;
  item.id_ = id;
  item.source_ = source;
  item.source_instance_ = instance.value();
  item.scope_ = std::move(scope);
  item.dimension_ = dimension;
  item.view_ = view;
  item.value_ = value;
  item.status_ = status;
  item.stamp_ = std::move(stamp);
  item.content_digest_ = digest_with_domain(kEvidenceDomain, item.canonical_bytes());
  return item;
}

std::string EvidenceItem::canonical_bytes() const {
  CanonicalWriter writer;
  writer.field(kTagId);
  writer.bytes(id_.to_string());
  writer.field(kTagSource);
  writer.token(to_string(source_));
  writer.field(kTagSourceInstance);
  writer.token(source_instance_);
  writer.field(kTagScope);
  writer.bytes(scope_.to_string());
  writer.field(kTagDimension);
  writer.token(dimension_.to_string());
  writer.field(kTagView);
  writer.token(to_string(view_));
  write_quantity(writer, value_);
  writer.field(kTagStatus);
  writer.token(to_string(status_));
  write_stamp(writer, stamp_);
  writer.field(kTagContentDigest);
  writer.bytes(content_digest_.to_hex());
  writer.field(kTagEnd);
  return std::move(writer).take();
}

Result<EvidenceItem> EvidenceItem::decode(std::string_view bytes) {
  CanonicalReader reader(bytes);
  auto status = reader.field(kTagId, "evidence id");
  if (!status.ok()) {
    return status.error();
  }
  auto id_text = reader.bytes(64, "evidence id");
  if (!id_text.ok()) {
    return id_text.error();
  }
  auto id = EvidenceId::parse(id_text.value());
  if (!id.has_value()) {
    return make_error(ErrorCode::Malformed, "stored evidence identity is malformed",
                      "evidence_id");
  }

  status = reader.field(kTagSource, "evidence source");
  if (!status.ok()) {
    return status.error();
  }
  auto source_text = reader.token(kMaxReasonTokenBytes, "evidence source");
  if (!source_text.ok()) {
    return source_text.error();
  }
  auto source = source_from_token(source_text.value());
  if (!source.ok()) {
    return source.error();
  }

  status = reader.field(kTagSourceInstance, "source instance");
  if (!status.ok()) {
    return status.error();
  }
  auto instance = reader.token(kMaxSourceNameBytes, "source instance");
  if (!instance.ok()) {
    return instance.error();
  }

  status = reader.field(kTagScope, "scope");
  if (!status.ok()) {
    return status.error();
  }
  auto scope_text = read_scope(reader);
  if (!scope_text.ok()) {
    return scope_text.error();
  }
  auto scope = ScopeIdentity::parse(scope_text.value());
  if (!scope.ok()) {
    return scope.error();
  }

  status = reader.field(kTagDimension, "dimension");
  if (!status.ok()) {
    return status.error();
  }
  auto dimension = read_dimension(reader);
  if (!dimension.ok()) {
    return dimension.error();
  }

  status = reader.field(kTagView, "view");
  if (!status.ok()) {
    return status.error();
  }
  auto view_text = reader.token(32, "view");
  if (!view_text.ok()) {
    return view_text.error();
  }
  auto view = view_from_token(view_text.value());
  if (!view.ok()) {
    return view.error();
  }

  auto value = read_quantity(reader);
  if (!value.ok()) {
    return value.error();
  }

  status = reader.field(kTagStatus, "evidence status");
  if (!status.ok()) {
    return status.error();
  }
  auto status_text = reader.token(32, "evidence status");
  if (!status_text.ok()) {
    return status_text.error();
  }
  auto evidence_status = status_from_token(status_text.value());
  if (!evidence_status.ok()) {
    return evidence_status.error();
  }

  auto stamp = read_stamp(reader);
  if (!stamp.ok()) {
    return stamp.error();
  }

  status = reader.field(kTagContentDigest, "content digest");
  if (!status.ok()) {
    return status.error();
  }
  auto digest_text = reader.bytes(64, "content digest");
  if (!digest_text.ok()) {
    return digest_text.error();
  }
  auto declared = Digest::from_hex(digest_text.value());
  if (!declared.has_value()) {
    return make_error(ErrorCode::Malformed, "stored content digest is malformed", "digest");
  }

  status = reader.field(kTagEnd, "end marker");
  if (!status.ok()) {
    return status.error();
  }
  status = reader.exhausted("evidence item");
  if (!status.ok()) {
    return status.error();
  }

  auto item = EvidenceItem::create(*id, source.value(), instance.value(), scope.value(),
                                   dimension.value(), view.value(), value.value(),
                                   evidence_status.value(), stamp.value());
  if (!item.ok()) {
    return item.error();
  }
  if (!(item.value().content_digest_ == *declared)) {
    return make_error(ErrorCode::Corruption,
                      "evidence content digest does not match the stored bytes: the record was "
                      "modified after it was written",
                      "content_digest");
  }
  return item;
}

bool EvidenceItem::is_current_at(Tick instant) const noexcept {
  if (status_ == EvidenceStatus::Expired || status_ == EvidenceStatus::Superseded ||
      status_ == EvidenceStatus::Unsupported || status_ == EvidenceStatus::Unavailable) {
    return false;
  }
  return !(stamp_.valid_until < instant);
}

bool evidence_less(const EvidenceItem& a, const EvidenceItem& b) noexcept {
  if (!(a.scope() == b.scope())) {
    return a.scope() < b.scope();
  }
  if (a.dimension() != b.dimension()) {
    return a.dimension() < b.dimension();
  }
  if (a.view() != b.view()) {
    return view_index(a.view()) < view_index(b.view());
  }
  if (a.source() != b.source()) {
    return static_cast<std::uint8_t>(a.source()) < static_cast<std::uint8_t>(b.source());
  }
  if (a.source_instance() != b.source_instance()) {
    return a.source_instance() < b.source_instance();
  }
  // Newest revision first, so that the authoritative item for a source instance
  // is the one a canonical scan encounters first.
  if (!(a.stamp().revision == b.stamp().revision)) {
    return b.stamp().revision < a.stamp().revision;
  }
  return a.id() < b.id();
}

Result<EvidenceSet> EvidenceSet::build(std::vector<EvidenceItem> items, std::size_t max_items) {
  if (items.size() > max_items) {
    return make_error(ErrorCode::LimitExceeded,
                      "evidence set of " + std::to_string(items.size()) +
                          " items exceeds the configured limit of " + std::to_string(max_items),
                      "evidence_items");
  }
  std::sort(items.begin(), items.end(), evidence_less);

  for (std::size_t i = 1; i < items.size(); ++i) {
    if (items[i].id() == items[i - 1].id()) {
      return make_error(ErrorCode::AlreadyExists,
                        "duplicate evidence identity " + items[i].id().to_string() +
                            ": identity is never reused",
                        "evidence_id");
    }
  }

  CanonicalWriter writer;
  for (const EvidenceItem& item : items) {
    writer.bytes(item.content_digest().to_hex());
  }

  EvidenceSet set;
  set.items_ = std::move(items);
  set.digest_ = writer.digest(kEvidenceSetDomain);
  return set;
}

Generation EvidenceSet::max_generation() const noexcept {
  Generation highest;
  for (const EvidenceItem& item : items_) {
    if (item.stamp().generation > highest) {
      highest = item.stamp().generation;
    }
  }
  return highest;
}

Revision EvidenceSet::max_revision(std::string_view source_instance) const noexcept {
  Revision highest;
  for (const EvidenceItem& item : items_) {
    if (item.source_instance() == source_instance && highest < item.stamp().revision) {
      highest = item.stamp().revision;
    }
  }
  return highest;
}

}  // namespace summon::capacity_reconciliation
