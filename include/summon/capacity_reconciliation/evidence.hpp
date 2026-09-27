// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Immutable, generation-bound evidence.
//
// This runtime is a *consumer*. It never writes back to Facility Capacity,
// Rack Capacity, Space Capacity, Power Capacity, Cooling Capacity, the
// reservation register, the asset/rack/location registries or the observation
// stream. It receives frozen statements from them and reconciles those
// statements.
//
// An EvidenceItem is therefore immutable and self-describing:
//
//   * it names a producing source family and a source instance;
//   * it carries the generation, revision, epoch and incarnation it was
//     produced under, so stale authority can be detected rather than merged;
//   * it carries the tick it was observed at, in a named clock domain, and the
//     tick after which it must not be treated as current;
//   * it carries the exact unit it is expressed in;
//   * it carries a content digest, so swapping two items with the same
//     identity but different content is detectable.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "summon/capacity_reconciliation/canonical.hpp"
#include "summon/capacity_reconciliation/hash.hpp"
#include "summon/capacity_reconciliation/identity.hpp"
#include "summon/capacity_reconciliation/scope.hpp"
#include "summon/capacity_reconciliation/status.hpp"
#include "summon/capacity_reconciliation/units.hpp"
#include "summon/capacity_reconciliation/value.hpp"

namespace summon::capacity_reconciliation {

/// Which upstream runtime family produced a statement. The list is exactly the
/// set of consumers named in the systems boundary plus the operator.
enum class EvidenceSource : std::uint8_t {
  Unknown = 0,
  FacilityCapacity,     ///< Facility Capacity runtime.
  RackCapacity,         ///< Rack Capacity runtime.
  SpaceCapacity,        ///< Space Capacity runtime.
  PowerCapacity,        ///< Power Capacity runtime.
  CoolingCapacity,      ///< Cooling Capacity runtime.
  ReservationRegister,  ///< Reservation authority.
  AssetRegistry,        ///< Physical asset inventory.
  RackRegistry,         ///< Rack inventory.
  LocationRegistry,     ///< Physical location registry.
  ObservationStream,    ///< Measurement / observatory feed.
  OperatorDeclaration,  ///< Human operator declaration.
};

[[nodiscard]] const char* to_string(EvidenceSource source) noexcept;
[[nodiscard]] std::optional<EvidenceSource> evidence_source_from_string(
    std::string_view token) noexcept;

/// Which capacity view a statement belongs to. The order of the enumerators is
/// the canonical chain order: planned -> reserved -> installed -> observed ->
/// usable -> allocatable.
enum class CapacityView : std::uint8_t {
  Planned = 0,      ///< What the design intends to exist.
  Reserved = 1,     ///< What has been committed to a consumer.
  Installed = 2,    ///< What physically exists (nameplate).
  Observed = 3,     ///< What is actually delivering right now.
  Usable = 4,       ///< Installed minus environmental derate.
  Allocatable = 5,  ///< Declared headroom available to new commitments.
};

inline constexpr std::size_t kCapacityViewCount = 6;

[[nodiscard]] const char* to_string(CapacityView view) noexcept;
[[nodiscard]] std::optional<CapacityView> capacity_view_from_string(
    std::string_view token) noexcept;
[[nodiscard]] constexpr std::size_t view_index(CapacityView view) noexcept {
  return static_cast<std::size_t>(view);
}

/// Why a statement is not usable as authoritative evidence.
enum class EvidenceStatus : std::uint8_t {
  /// Complete, in-window, accepted.
  Accepted = 0,
  /// The declaring source marked the statement as partial.
  Partial = 1,
  /// Past its validity window at the evaluation instant.
  Expired = 2,
  /// The source explicitly marked the statement superseded.
  Superseded = 3,
  /// The statement names a dimension, view or unit outside the boundary.
  Unsupported = 4,
  /// The producer was asked and returned nothing usable.
  Unavailable = 5,
};

[[nodiscard]] const char* to_string(EvidenceStatus status) noexcept;
[[nodiscard]] std::optional<EvidenceStatus> evidence_status_from_string(
    std::string_view token) noexcept;

/// Everything needed to decide whether a statement may drive a decision.
struct EvidenceStamp {
  /// Store generation the statement was produced against.
  Generation generation;
  /// Producer's own revision counter for the described object.
  Revision revision;
  /// Control-plane epoch the statement was produced under.
  Epoch epoch;
  /// Incarnation of the producing runtime process.
  IncarnationId incarnation;
  /// Instant the statement describes, in `clock_domain`.
  Tick observed_at;
  /// Instant after which the statement must not be treated as current. A value
  /// of `observed_at` means "valid only at that instant".
  Tick valid_until;
  /// Named clock domain of both ticks.
  ClockDomain clock_domain;
};

/// A frozen statement about one view of one dimension at one scope.
class EvidenceItem {
 public:
  EvidenceItem() = default;

  /// Full construction with validation. Rejects invalid ids, empty scope,
  /// inadmissible dimension/unit pairs, unknown reasons paired with known
  /// values, inverted validity windows and over-long tokens.
  [[nodiscard]] static Result<EvidenceItem> create(EvidenceId id, EvidenceSource source,
                                                   std::string source_instance,
                                                   ScopeIdentity scope, DimensionKey dimension,
                                                   CapacityView view, Quantity value,
                                                   EvidenceStatus status, EvidenceStamp stamp);

  [[nodiscard]] const EvidenceId& id() const noexcept { return id_; }
  [[nodiscard]] EvidenceSource source() const noexcept { return source_; }
  [[nodiscard]] const std::string& source_instance() const noexcept { return source_instance_; }
  [[nodiscard]] const ScopeIdentity& scope() const noexcept { return scope_; }
  [[nodiscard]] const DimensionKey& dimension() const noexcept { return dimension_; }
  [[nodiscard]] CapacityView view() const noexcept { return view_; }
  [[nodiscard]] const Quantity& value() const noexcept { return value_; }
  [[nodiscard]] EvidenceStatus status() const noexcept { return status_; }
  [[nodiscard]] const EvidenceStamp& stamp() const noexcept { return stamp_; }

  /// Content digest, computed over the canonical encoding of every field
  /// including the identity. Two items that differ in any field differ here.
  [[nodiscard]] const Digest& content_digest() const noexcept { return content_digest_; }

  /// The canonical encoding used for content_digest.
  [[nodiscard]] std::string canonical_bytes() const;

  /// Decode from canonical bytes, recomputing and verifying the content digest.
  [[nodiscard]] static Result<EvidenceItem> decode(std::string_view bytes);

  /// True when the statement may be consumed at `instant` under a policy that
  /// rejects expired evidence.
  [[nodiscard]] bool is_current_at(Tick instant) const noexcept;

  friend bool operator==(const EvidenceItem& a, const EvidenceItem& b) noexcept {
    return a.content_digest_ == b.content_digest_ && a.id_ == b.id_;
  }

 private:
  EvidenceId id_;
  EvidenceSource source_ = EvidenceSource::Unknown;
  std::string source_instance_;
  ScopeIdentity scope_;
  DimensionKey dimension_;
  CapacityView view_ = CapacityView::Planned;
  Quantity value_;
  EvidenceStatus status_ = EvidenceStatus::Accepted;
  EvidenceStamp stamp_;
  Digest content_digest_;
};

/// Canonical ordering key for evidence. Ordering is total and depends only on
/// the content of the items, never on the order they were supplied in.
///
/// Order: scope, dimension key, view, source family, source instance, revision
/// (descending: newest first), id.
[[nodiscard]] bool evidence_less(const EvidenceItem& a, const EvidenceItem& b) noexcept;

/// An immutable set of evidence, held behind a shared pointer so that a run can
/// keep reading a stable snapshot while new evidence arrives.
class EvidenceSet {
 public:
  EvidenceSet() = default;

  /// Build from a vector. Validates identity uniqueness (ALREADY_EXISTS on
  /// duplicates) and the item bound, then stores items in canonical order.
  [[nodiscard]] static Result<EvidenceSet> build(std::vector<EvidenceItem> items,
                                                 std::size_t max_items);

  [[nodiscard]] const std::vector<EvidenceItem>& items() const noexcept { return items_; }
  [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
  [[nodiscard]] bool empty() const noexcept { return items_.empty(); }

  /// Digest over the canonical ordering of all items. Independent of the order
  /// in which items were supplied to build().
  [[nodiscard]] const Digest& digest() const noexcept { return digest_; }

  /// Highest generation present, or Generation{} when empty.
  [[nodiscard]] Generation max_generation() const noexcept;

  /// Highest revision present for a given source instance, or Revision{}.
  [[nodiscard]] Revision max_revision(std::string_view source_instance) const noexcept;

 private:
  std::vector<EvidenceItem> items_;
  Digest digest_;
};

}  // namespace summon::capacity_reconciliation
