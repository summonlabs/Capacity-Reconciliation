// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/capacity_reconciliation/attribution.hpp"

#include <algorithm>
#include <array>

#include "summon/capacity_reconciliation/limits.hpp"

namespace summon::capacity_reconciliation {
namespace {

struct ResidualToken {
  ResidualKind kind;
  const char* token;
};

constexpr std::array<ResidualToken, 7> kResidualTokens{{
    {ResidualKind::None, "none"},
    {ResidualKind::PlannedGap, "planned_gap"},
    {ResidualKind::ObservedGap, "observed_gap"},
    {ResidualKind::DerateGap, "derate_gap"},
    {ResidualKind::HeadroomGap, "headroom_gap"},
    {ResidualKind::ReservationPressure, "reservation_pressure"},
    {ResidualKind::ReservationOverhang, "reservation_overhang"},
}};

struct ReasonToken {
  AttributionReason reason;
  const char* token;
};

constexpr std::array<ReasonToken, 14> kReasonTokens{{
    {AttributionReason::Other, "other"},
    {AttributionReason::EnvironmentalConstraint, "environmental_constraint"},
    {AttributionReason::DeclaredDerate, "declared_derate"},
    {AttributionReason::Maintenance, "maintenance"},
    {AttributionReason::Decommissioned, "decommissioned"},
    {AttributionReason::EquipmentFailure, "equipment_failure"},
    {AttributionReason::ReservationHold, "reservation_hold"},
    {AttributionReason::Uncommissioned, "uncommissioned"},
    {AttributionReason::PowerCapping, "power_capping"},
    {AttributionReason::ThermalLimit, "thermal_limit"},
    {AttributionReason::NetworkConstraint, "network_constraint"},
    {AttributionReason::StructuralLimit, "structural_limit"},
    {AttributionReason::WeightLimit, "weight_limit"},
    {AttributionReason::OperatorHold, "operator_hold"},
}};

constexpr std::string_view kAttributionDomain = "capacity-reconciliation/attribution/v1";
constexpr std::string_view kAttributionSetDomain = "capacity-reconciliation/attribution-set/v1";

enum Tag : std::uint8_t {
  kTagId = 1,
  kTagSource = 2,
  kTagSourceInstance = 3,
  kTagScope = 4,
  kTagDimension = 5,
  kTagTarget = 6,
  kTagReason = 7,
  kTagReasonToken = 8,
  kTagAmount = 9,
  kTagGeneration = 10,
  kTagRevision = 11,
  kTagEpoch = 12,
  kTagIncarnation = 13,
  kTagObservedAt = 14,
  kTagValidUntil = 15,
  kTagClockDomain = 16,
  kTagContentDigest = 17,
  kTagEnd = 0x7F,
};

}  // namespace

const char* to_string(ResidualKind kind) noexcept {
  for (const ResidualToken& entry : kResidualTokens) {
    if (entry.kind == kind) {
      return entry.token;
    }
  }
  return "unrecognised";
}

std::optional<ResidualKind> residual_kind_from_string(std::string_view token) noexcept {
  for (const ResidualToken& entry : kResidualTokens) {
    if (token == entry.token) {
      return entry.kind;
    }
  }
  return std::nullopt;
}

const char* to_string(AttributionReason reason) noexcept {
  for (const ReasonToken& entry : kReasonTokens) {
    if (entry.reason == reason) {
      return entry.token;
    }
  }
  return "unrecognised";
}

std::optional<AttributionReason> attribution_reason_from_string(std::string_view token) noexcept {
  for (const ReasonToken& entry : kReasonTokens) {
    if (token == entry.token) {
      return entry.reason;
    }
  }
  return std::nullopt;
}

Result<AttributionItem> AttributionItem::create(AttributionId id, EvidenceSource source,
                                                std::string source_instance, ScopeIdentity scope,
                                                DimensionKey dimension, ResidualKind target,
                                                AttributionReason reason, std::string reason_token,
                                                Amount amount, EvidenceStamp stamp) {
  if (!id.valid()) {
    return make_error(ErrorCode::InvalidArgument, "attribution requires a generated identity",
                      "attribution_id");
  }
  if (source == EvidenceSource::Unknown) {
    return make_error(ErrorCode::InvalidArgument,
                      "attribution requires a concrete producing source family", "evidence_source");
  }
  auto instance = make_token(source_instance, kMaxSourceNameBytes, "source instance");
  if (!instance.ok()) {
    return instance.error();
  }
  if (scope.empty()) {
    return make_error(ErrorCode::InvalidArgument, "attribution requires a scope", "scope");
  }
  if (target == ResidualKind::None) {
    return make_error(ErrorCode::InvalidArgument,
                      "attribution must name the residual it explains: an attribution that "
                      "targets nothing would silently adjust the wrong total",
                      "residual_kind");
  }
  if (stamp.clock_domain.empty()) {
    return make_error(ErrorCode::InvalidArgument, "attribution requires a declared clock domain",
                      "clock_domain");
  }
  if (stamp.valid_until < stamp.observed_at) {
    return make_error(ErrorCode::InvalidArgument,
                      "attribution validity window is inverted", "validity_window");
  }
  auto token = make_token(reason_token, kMaxReasonTokenBytes, "attribution reason token");
  if (!token.ok()) {
    return token.error();
  }

  AttributionItem item;
  item.id_ = id;
  item.source_ = source;
  item.source_instance_ = instance.value();
  item.scope_ = std::move(scope);
  item.dimension_ = dimension;
  item.target_ = target;
  item.reason_ = reason;
  item.reason_token_ = token.value();
  item.amount_ = amount;
  item.stamp_ = std::move(stamp);
  item.content_digest_ = digest_with_domain(kAttributionDomain, item.canonical_bytes());
  return item;
}

std::string AttributionItem::canonical_bytes() const {
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
  writer.field(kTagTarget);
  writer.token(to_string(target_));
  writer.field(kTagReason);
  writer.token(to_string(reason_));
  writer.field(kTagReasonToken);
  writer.token(reason_token_);
  writer.field(kTagAmount);
  writer.i64(amount_);
  writer.field(kTagGeneration);
  writer.u64(stamp_.generation.value());
  writer.field(kTagRevision);
  writer.u64(stamp_.revision.value());
  writer.field(kTagEpoch);
  writer.u64(stamp_.epoch.value());
  writer.field(kTagIncarnation);
  writer.bytes(stamp_.incarnation.to_string());
  writer.field(kTagObservedAt);
  writer.u64(stamp_.observed_at.value());
  writer.field(kTagValidUntil);
  writer.u64(stamp_.valid_until.value());
  writer.field(kTagClockDomain);
  writer.token(stamp_.clock_domain.name());
  writer.field(kTagContentDigest);
  writer.bytes(content_digest_.to_hex());
  writer.field(kTagEnd);
  return std::move(writer).take();
}

Result<AttributionItem> AttributionItem::decode(std::string_view bytes) {
  CanonicalReader reader(bytes);

  auto status = reader.field(kTagId, "attribution id");
  if (!status.ok()) {
    return status.error();
  }
  auto id_text = reader.bytes(64, "attribution id");
  if (!id_text.ok()) {
    return id_text.error();
  }
  auto id = AttributionId::parse(id_text.value());
  if (!id.has_value()) {
    return make_error(ErrorCode::Malformed, "stored attribution identity is malformed",
                      "attribution_id");
  }

  status = reader.field(kTagSource, "attribution source");
  if (!status.ok()) {
    return status.error();
  }
  auto source_text = reader.token(kMaxReasonTokenBytes, "attribution source");
  if (!source_text.ok()) {
    return source_text.error();
  }
  auto source = evidence_source_from_string(source_text.value());
  if (!source.has_value()) {
    return make_error(ErrorCode::Malformed,
                      "stored attribution source is not recognised: '" + source_text.value() + "'",
                      "evidence_source");
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
  auto scope_text = reader.bytes(kMaxScopeDepth * (kMaxScopeSegmentBytes + 1u), "scope");
  if (!scope_text.ok()) {
    return scope_text.error();
  }
  auto scope = ScopeIdentity::parse(scope_text.value());
  if (!scope.ok()) {
    return scope.error();
  }
  if (scope.value().to_string() != scope_text.value()) {
    return make_error(ErrorCode::Malformed, "stored attribution scope is not canonical", "scope");
  }

  status = reader.field(kTagDimension, "dimension");
  if (!status.ok()) {
    return status.error();
  }
  auto dimension_text = reader.bytes(64, "dimension");
  if (!dimension_text.ok()) {
    return dimension_text.error();
  }
  auto dimension = DimensionKey::parse(dimension_text.value());
  if (!dimension.has_value()) {
    return make_error(ErrorCode::Malformed, "stored attribution dimension is not recognised",
                      "dimension");
  }

  status = reader.field(kTagTarget, "target");
  if (!status.ok()) {
    return status.error();
  }
  auto target_text = reader.token(32, "target");
  if (!target_text.ok()) {
    return target_text.error();
  }
  auto target = residual_kind_from_string(target_text.value());
  if (!target.has_value()) {
    return make_error(ErrorCode::Malformed, "stored residual target is not recognised", "target");
  }

  status = reader.field(kTagReason, "reason");
  if (!status.ok()) {
    return status.error();
  }
  auto reason_text = reader.token(48, "reason");
  if (!reason_text.ok()) {
    return reason_text.error();
  }
  auto reason = attribution_reason_from_string(reason_text.value());
  if (!reason.has_value()) {
    return make_error(ErrorCode::Malformed, "stored attribution reason is not recognised",
                      "reason");
  }

  status = reader.field(kTagReasonToken, "reason token");
  if (!status.ok()) {
    return status.error();
  }
  auto reason_token = reader.token(kMaxReasonTokenBytes, "reason token");
  if (!reason_token.ok()) {
    return reason_token.error();
  }

  status = reader.field(kTagAmount, "amount");
  if (!status.ok()) {
    return status.error();
  }
  auto amount = reader.i64();
  if (!amount.ok()) {
    return amount.error();
  }

  EvidenceStamp stamp;
  status = reader.field(kTagGeneration, "stamp generation");
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
  status = reader.exhausted("attribution item");
  if (!status.ok()) {
    return status.error();
  }

  auto item = AttributionItem::create(*id, *source, instance.value(), scope.value(), *dimension,
                                      *target, *reason, reason_token.value(), amount.value(),
                                      stamp);
  if (!item.ok()) {
    return item.error();
  }
  if (!(item.value().content_digest_ == *declared)) {
    return make_error(ErrorCode::Corruption,
                      "attribution content digest does not match the stored bytes",
                      "content_digest");
  }
  return item;
}

bool AttributionItem::is_current_at(Tick instant) const noexcept {
  return !(stamp_.valid_until < instant);
}

Result<AttributionSet> AttributionSet::build(std::vector<AttributionItem> items,
                                             std::size_t max_items) {
  if (items.size() > max_items) {
    return make_error(ErrorCode::LimitExceeded,
                      "attribution set of " + std::to_string(items.size()) +
                          " items exceeds the configured limit of " + std::to_string(max_items),
                      "attribution_items");
  }
  std::sort(items.begin(), items.end());

  for (std::size_t i = 1; i < items.size(); ++i) {
    if (items[i].id() == items[i - 1].id()) {
      return make_error(ErrorCode::AlreadyExists,
                        "duplicate attribution identity " + items[i].id().to_string(),
                        "attribution_id");
    }
  }

  CanonicalWriter writer;
  for (const AttributionItem& item : items) {
    writer.bytes(item.content_digest().to_hex());
  }

  AttributionSet set;
  set.items_ = std::move(items);
  set.digest_ = writer.digest(kAttributionSetDomain);
  return set;
}

}  // namespace summon::capacity_reconciliation
