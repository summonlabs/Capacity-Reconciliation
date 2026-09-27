// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Discrepancy classification.
//
// A cell is one (scope, dimension key) pair. Classification is a deterministic
// function of the cell's view vector, its conflict set and the policy. Findings
// are produced in a fixed precedence order; the first finding is the cell's
// primary classification and every remaining finding is retained, because the
// interesting operational question is usually "what else is also true".
//
// Reason codes are stable tokens. Consumers branch on the token, never on
// message text.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "summon/capacity_reconciliation/quantity.hpp"
#include "summon/capacity_reconciliation/status.hpp"

namespace summon::capacity_reconciliation {

/// The primary classification of a cell.
///
/// The enumerator order *is* the validation precedence: a cell's primary
/// classification is the first applicable entry in this order. The test suite
/// asserts the order explicitly, so reordering is a deliberate, tested act.
enum class DiscrepancyClass : std::uint8_t {
  /// A residual could not be computed: checked arithmetic overflowed.
  ArithmeticOverflow = 0,
  /// Two or more authoritative sources disagree and policy cannot order them.
  /// A conflict is a *cause*; "no evidence established" is frequently only its
  /// consequence, so the conflict is reported first.
  EvidenceConflict = 1,
  /// Statements exist for the cell but nothing about it is established.
  NoEvidence = 2,
  /// Consumed evidence is too many generations behind the run.
  GenerationRegression = 3,
  /// Consumed evidence is past its validity window.
  Stale = 4,
  /// The same dimension appears under more than one unit in this scope; the
  /// units are kept separate and no cross-unit total is produced.
  UnitSeparated = 5,
  /// A view required by policy is absent or unknown.
  Incomplete = 6,
  /// Nameplate capacity is not being observed.
  ObservedShortfall = 7,
  /// Observation exceeds declared installed capacity.
  ObservationExceedsInstalled = 8,
  /// The facility is not what was planned.
  PlannedGap = 9,
  /// Something is installed that the plan does not account for.
  UnplannedInstall = 10,
  /// Usable capacity is derated below what is observed to be deliverable.
  UsableDerated = 11,
  /// Usable capacity claims more than is observed to be deliverable.
  UsableExceedsObserved = 12,
  /// Declared allocatable headroom exceeds what the arithmetic supports.
  AllocatableOverstated = 13,
  /// Declared allocatable headroom is below what the arithmetic supports.
  AllocatableUnderstated = 14,
  /// Reservations exceed nameplate installed capacity.
  ReservationExceedsInstalled = 15,
  /// Reservations exceed declared allocatable headroom.
  ReservationExceedsAllocatable = 16,
  /// A residual remains after every attribution was applied. Preserved, not
  /// balanced away.
  UnexplainedResidual = 17,
  /// Views are present and agree within tolerance.
  Agrees = 18,
};

[[nodiscard]] const char* to_string(DiscrepancyClass value) noexcept;
[[nodiscard]] std::optional<DiscrepancyClass> discrepancy_class_from_string(
    std::string_view token) noexcept;

/// The residual a finding concerns.
enum class ReasonTarget : std::uint8_t {
  None = 0,
  Cell = 1,
  PlannedGap = 2,
  ObservedGap = 3,
  DerateGap = 4,
  HeadroomGap = 5,
  ReservationPressure = 6,
  ReservationOverhang = 7,
  AllocatableSkew = 8,
};

[[nodiscard]] const char* to_string(ReasonTarget target) noexcept;

/// A single machine-readable finding.
///
/// `code` is the stable token a consumer should branch on. `detail` is a short
/// human rendering and is explicitly *not* a contract.
struct Finding {
  DiscrepancyClass classification = DiscrepancyClass::NoEvidence;
  ReasonTarget target = ReasonTarget::None;
  /// Observed magnitude of the discrepancy, when it is known.
  std::optional<std::int64_t> magnitude;
  /// Tolerance that was applied, in the key's unit.
  std::int64_t tolerance = 0;
  /// Whether the magnitude exceeded the tolerance.
  bool exceeds_tolerance = false;
  /// Short human rendering. Not a contract.
  std::string detail;

  [[nodiscard]] std::string to_string() const;
};

/// Stable ordering of findings: by classification ordinal, then target, then
/// magnitude. Deterministic so that a run digest does not depend on the order
/// findings happened to be discovered in.
[[nodiscard]] bool finding_less(const Finding& a, const Finding& b) noexcept;

/// Resolution state of a reconciled cell or run.
enum class ResolutionState : std::uint8_t {
  /// The run was computed and at least one cell is inconclusive.
  Open = 0,
  /// Every cell is explained: no unexplained residuals, no conflicts, no
  /// missing views.
  Explained = 1,
  /// Some cells are explained; others carry unexplained residuals.
  PartiallyExplained = 2,
  /// One or more cells have unresolved evidence conflicts.
  Conflicted = 3,
  /// One or more cells depend on stale evidence.
  Stale = 4,
  /// One or more cells are missing a required view.
  Incomplete = 5,
  /// A successor run covers this run's scope at a later generation.
  Superseded = 6,
  /// An operator closed the run without a successor.
  Closed = 7,
  /// The run was reopened for further investigation.
  Reopened = 8,
};

[[nodiscard]] const char* to_string(ResolutionState state) noexcept;
[[nodiscard]] std::optional<ResolutionState> resolution_state_from_string(
    std::string_view token) noexcept;

/// Whether a resolution state is terminal for authority purposes. Superseded,
/// Closed and Reopened runs are never treated as current.
[[nodiscard]] constexpr bool is_terminal(ResolutionState state) noexcept {
  return state == ResolutionState::Superseded || state == ResolutionState::Closed ||
         state == ResolutionState::Reopened;
}

}  // namespace summon::capacity_reconciliation
