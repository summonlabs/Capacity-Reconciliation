// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Reconciliation policy.
//
// Everything that could otherwise be an implicit judgement is an explicit,
// versioned, digested policy value: which source family outranks which, how old
// evidence may be, which residuals are tolerable, and what to do when a view is
// missing. Two runs with the same evidence and the same policy digest produce
// the same result; changing the policy changes the digest and therefore the
// run's identity.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "summon/capacity_reconciliation/canonical.hpp"
#include "summon/capacity_reconciliation/evidence.hpp"
#include "summon/capacity_reconciliation/hash.hpp"
#include "summon/capacity_reconciliation/status.hpp"
#include "summon/capacity_reconciliation/value.hpp"

namespace summon::capacity_reconciliation {

/// How to treat a view that has no usable value at the evaluation instant.
enum class MissingViewPolicy : std::uint8_t {
  /// The cell is INCOMPLETE and any residual touching the missing view is
  /// Unknown. This is the default and the only safe choice.
  Incomplete = 0,
  /// The missing view is treated as an explicit unknown and classification
  /// proceeds with the views that are present, recording which were dropped.
  ProceedWithPresent = 1,
};

[[nodiscard]] const char* to_string(MissingViewPolicy policy) noexcept;
[[nodiscard]] std::optional<MissingViewPolicy> missing_view_policy_from_string(
    std::string_view token) noexcept;

/// How to treat evidence past its validity window.
enum class StaleEvidencePolicy : std::uint8_t {
  /// Expired evidence is unusable: it becomes Unknown(Expired).
  Reject = 0,
  /// Expired evidence is used but the cell is marked stale. Only legal when
  /// the caller explicitly opts in; the classification still reports
  /// STALE_EVIDENCE so no consumer can mistake it for current.
  UseButMarkStale = 1,
};

[[nodiscard]] const char* to_string(StaleEvidencePolicy policy) noexcept;
[[nodiscard]] std::optional<StaleEvidencePolicy> stale_evidence_policy_from_string(
    std::string_view token) noexcept;

/// How to roll up descendant scopes into a selected scope.
enum class RollupPolicy : std::uint8_t {
  /// Every descendant that has any evidence for the key must contribute, and
  /// every contributing quantity must be known, or the rollup is
  /// Unknown(PartialRollup).
  Strict = 0,
  /// Known contributors are summed and unknown contributors are recorded in
  /// the explanation graph; the rollup is still Unknown(PartialRollup) so the
  /// distinction survives, but the partial sum is reported.
  PartialSumReported = 1,
};

[[nodiscard]] const char* to_string(RollupPolicy policy) noexcept;
[[nodiscard]] std::optional<RollupPolicy> rollup_policy_from_string(
    std::string_view token) noexcept;

/// Tolerances applied before a difference is called a discrepancy. Expressed in
/// the exact unit of the key, never as a percentage of a float.
struct ToleranceBand {
  /// Absolute allowance in the key's unit. A difference whose magnitude is
  /// less than or equal to this is treated as agreement.
  Amount absolute = 0;
  /// Additional allowance expressed in parts per million of the reference
  /// value, evaluated with integer arithmetic only. Zero disables it.
  std::uint32_t parts_per_million = 0;

  friend bool operator==(const ToleranceBand& a, const ToleranceBand& b) noexcept {
    return a.absolute == b.absolute && a.parts_per_million == b.parts_per_million;
  }
};

/// Which view supplies the magnitude a discrepancy is judged against, so that
/// a "20% deviation" style rule is expressed without floating point.
struct DimensionPolicy {
  DimensionKey dimension;
  ToleranceBand tolerance;
  /// Views that must be present for a cell in this dimension to be classified
  /// as complete. Defaults to installed + observed.
  std::vector<CapacityView> required_views;

  friend bool operator==(const DimensionPolicy& a, const DimensionPolicy& b) noexcept;
};

/// The complete, digested policy for a reconciliation run.
class ReconciliationPolicy {
 public:
  /// A default-constructed policy *is* the standard policy. It is never empty:
  /// an empty precedence list would rank every source family equally and turn
  /// ordinary multi-source agreement into an unresolvable conflict, which no
  /// caller asked for and which would silently change every classification.
  ReconciliationPolicy();

  /// The default policy. Source precedence, in decreasing authority:
  ///
  ///   ObservationStream, ReservationRegister, AssetRegistry, RackRegistry,
  ///   LocationRegistry, FacilityCapacity, RackCapacity, SpaceCapacity,
  ///   PowerCapacity, CoolingCapacity, OperatorDeclaration, Unknown
  ///
  /// Rationale: for the *observed* view nothing outranks a measurement; for
  /// the *reserved* view the reservation register is the authority; the
  /// registries outrank the modelling runtimes because they describe installed
  /// physical reality; operator declarations rank last so that a declaration
  /// can never silently overrule machine evidence -- it must instead be
  /// reconciled as a conflict.
  [[nodiscard]] static ReconciliationPolicy standard();

  /// Precedence index for a source family; lower is more authoritative. Equal
  /// indices mean the policy cannot order two sources and any disagreement
  /// between them is an unresolvable conflict.
  [[nodiscard]] std::size_t precedence_of(EvidenceSource source) const noexcept;

  /// Override the precedence order. Must be a permutation of all source
  /// families; anything else is INVALID_ARGUMENT.
  [[nodiscard]] Status set_precedence(std::vector<EvidenceSource> order);

  [[nodiscard]] const std::vector<EvidenceSource>& precedence() const noexcept {
    return precedence_;
  }

  /// Per-dimension overrides that have been set, in canonical order.
  [[nodiscard]] const std::vector<DimensionPolicy>& dimensions() const noexcept {
    return dimensions_;
  }

  /// Set the tolerance for a dimension, replacing any previous entry.
  void set_tolerance(const DimensionKey& dimension, ToleranceBand band);

  /// Look up the tolerance for a dimension. Unknown dimensions get the default.
  [[nodiscard]] ToleranceBand tolerance_for(const DimensionKey& dimension) const noexcept;

  /// Set the required views for a dimension.
  void set_required_views(const DimensionKey& dimension, std::vector<CapacityView> views);

  /// Required views for a dimension, falling back to the default set.
  [[nodiscard]] const std::vector<CapacityView>& required_views_for(
      const DimensionKey& dimension) const noexcept;

  /// Evidence whose generation is more than this many generations behind the
  /// run's generation is refused as Unknown(GenerationTooOld). Zero disables
  /// the check (age is then governed solely by the tick window).
  std::uint64_t max_generation_lag = 0;

  MissingViewPolicy missing_view = MissingViewPolicy::Incomplete;
  StaleEvidencePolicy stale_evidence = StaleEvidencePolicy::Reject;
  RollupPolicy rollup = RollupPolicy::Strict;

  /// When true, a cell whose only finding is an unresolved evidence conflict
  /// keeps every conflicting value in its conflict set. Turning this off is
  /// refused: an unresolvable conflict must always be recorded.
  bool record_conflict_sets = true;

  /// When true, an unexplained residual is reported as its own finding rather
  /// than being folded into the residual's residual. Turning this off is
  /// refused: forcing totals to balance is outside the boundary.
  bool report_unexplained_residuals = true;

  /// Canonical encoding and digest. The digest is the policy identity carried
  /// by every run.
  ///
  /// The digest is computed from the canonical bytes on every call rather than
  /// cached. Every setting above is publicly mutable, so a cached digest could
  /// silently disagree with the policy it claims to identify -- and a policy
  /// digest that lies is worse than no digest at all.
  [[nodiscard]] std::string canonical_bytes() const;
  [[nodiscard]] Digest digest() const;

  friend bool operator==(const ReconciliationPolicy& a,
                         const ReconciliationPolicy& b) noexcept {
    return a.canonical_bytes() == b.canonical_bytes();
  }

 private:
  std::vector<EvidenceSource> precedence_;
  std::vector<DimensionPolicy> dimensions_;
  std::vector<CapacityView> default_required_views_;
};

/// The effective absolute tolerance for a difference measured against
/// `reference`: `band.absolute + |reference| * band.parts_per_million /
/// 1'000'000`, evaluated with integer arithmetic only and overflow-checked.
///
/// A negative `absolute` is treated as zero so that a malformed policy cannot
/// widen a tolerance into an accidental acceptance.
[[nodiscard]] Result<Amount> policy_tolerance_bound(const ToleranceBand& band, Amount reference);

}  // namespace summon::capacity_reconciliation
