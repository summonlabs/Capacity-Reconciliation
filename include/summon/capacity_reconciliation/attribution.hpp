// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Attribution items.
//
// Reconciliation must be able to explain *why* installed capacity is not
// observable, or why a reservation exceeds installed capacity, without
// inventing a number. Upstream authorities therefore attach signed
// attributions: "40 kW of installed power is unavailable because chiller 2 is
// down", "-12 U because row 4 rack 12 was decommissioned".
//
// Attributions are explanations, not adjustments. The runtime subtracts them
// from a residual only when the attribution names the same scope, dimension key
// and residual it claims to explain, and it *preserves whatever is left over*
// as an unexplained residual. Balancing totals to zero is never attempted.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "summon/capacity_reconciliation/canonical.hpp"
#include "summon/capacity_reconciliation/evidence.hpp"
#include "summon/capacity_reconciliation/hash.hpp"
#include "summon/capacity_reconciliation/identity.hpp"
#include "summon/capacity_reconciliation/scope.hpp"
#include "summon/capacity_reconciliation/status.hpp"
#include "summon/capacity_reconciliation/units.hpp"
#include "summon/capacity_reconciliation/value.hpp"

namespace summon::capacity_reconciliation {

/// Which residual an attribution claims to explain.
enum class ResidualKind : std::uint8_t {
  None = 0,
  /// `planned - installed`: intent not yet realized.
  PlannedGap = 1,
  /// `installed - observed`: nameplate not delivering.
  ObservedGap = 2,
  /// `observed - usable`: negative derate, i.e. usable is understated.
  DerateGap = 3,
  /// `usable - allocatable_declared`: declared headroom below usable.
  HeadroomGap = 4,
  /// `reserved - installed`: reservation pressure beyond nameplate.
  ReservationPressure = 5,
  /// `reserved - allocatable_declared`: reservations beyond declared headroom.
  ReservationOverhang = 6,
};

[[nodiscard]] const char* to_string(ResidualKind kind) noexcept;
[[nodiscard]] std::optional<ResidualKind> residual_kind_from_string(
    std::string_view token) noexcept;

/// Stable, bounded reason token attached to an attribution. Typed for the
/// common operational causes; `Other` carries a free token that is still
/// character-class restricted.
enum class AttributionReason : std::uint8_t {
  Other = 0,
  EnvironmentalConstraint,
  DeclaredDerate,
  Maintenance,
  Decommissioned,
  EquipmentFailure,
  ReservationHold,
  Uncommissioned,
  PowerCapping,
  ThermalLimit,
  NetworkConstraint,
  StructuralLimit,
  WeightLimit,
  OperatorHold,
};

[[nodiscard]] const char* to_string(AttributionReason reason) noexcept;
[[nodiscard]] std::optional<AttributionReason> attribution_reason_from_string(
    std::string_view token) noexcept;

/// A frozen explanation statement from an upstream authority.
class AttributionItem {
 public:
  AttributionItem() = default;

  [[nodiscard]] static Result<AttributionItem> create(
      AttributionId id, EvidenceSource source, std::string source_instance, ScopeIdentity scope,
      DimensionKey dimension, ResidualKind target, AttributionReason reason,
      std::string reason_token, Amount amount, EvidenceStamp stamp);

  [[nodiscard]] const AttributionId& id() const noexcept { return id_; }
  [[nodiscard]] EvidenceSource source() const noexcept { return source_; }
  [[nodiscard]] const std::string& source_instance() const noexcept { return source_instance_; }
  [[nodiscard]] const ScopeIdentity& scope() const noexcept { return scope_; }
  [[nodiscard]] const DimensionKey& dimension() const noexcept { return dimension_; }
  [[nodiscard]] ResidualKind target() const noexcept { return target_; }
  [[nodiscard]] AttributionReason reason() const noexcept { return reason_; }
  [[nodiscard]] const std::string& reason_token() const noexcept { return reason_token_; }
  [[nodiscard]] Amount amount() const noexcept { return amount_; }
  [[nodiscard]] const EvidenceStamp& stamp() const noexcept { return stamp_; }

  [[nodiscard]] const Digest& content_digest() const noexcept { return content_digest_; }
  [[nodiscard]] std::string canonical_bytes() const;
  [[nodiscard]] static Result<AttributionItem> decode(std::string_view bytes);

  [[nodiscard]] bool is_current_at(Tick instant) const noexcept;

  /// Canonical ordering key: scope, dimension, target, source, instance,
  /// revision descending, id.
  friend bool operator<(const AttributionItem& a, const AttributionItem& b) noexcept {
    if (a.scope_ != b.scope_) {
      return a.scope_ < b.scope_;
    }
    if (a.dimension_ != b.dimension_) {
      return a.dimension_ < b.dimension_;
    }
    if (a.target_ != b.target_) {
      return static_cast<std::uint8_t>(a.target_) < static_cast<std::uint8_t>(b.target_);
    }
    if (a.source_ != b.source_) {
      return static_cast<std::uint8_t>(a.source_) < static_cast<std::uint8_t>(b.source_);
    }
    if (a.source_instance_ != b.source_instance_) {
      return a.source_instance_ < b.source_instance_;
    }
    if (a.stamp_.revision != b.stamp_.revision) {
      return b.stamp_.revision < a.stamp_.revision;
    }
    return a.id_ < b.id_;
  }

 private:
  AttributionId id_;
  EvidenceSource source_ = EvidenceSource::Unknown;
  std::string source_instance_;
  ScopeIdentity scope_;
  DimensionKey dimension_;
  ResidualKind target_ = ResidualKind::None;
  AttributionReason reason_ = AttributionReason::Other;
  std::string reason_token_;
  Amount amount_ = 0;
  EvidenceStamp stamp_;
  Digest content_digest_;
};

/// Immutable set of attributions, in canonical order.
class AttributionSet {
 public:
  AttributionSet() = default;

  [[nodiscard]] static Result<AttributionSet> build(std::vector<AttributionItem> items,
                                                    std::size_t max_items);

  [[nodiscard]] const std::vector<AttributionItem>& items() const noexcept { return items_; }
  [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
  [[nodiscard]] bool empty() const noexcept { return items_.empty(); }
  [[nodiscard]] const Digest& digest() const noexcept { return digest_; }

 private:
  std::vector<AttributionItem> items_;
  Digest digest_;
};

}  // namespace summon::capacity_reconciliation
