// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/capacity_reconciliation/classify.hpp"

#include <array>

namespace summon::capacity_reconciliation {
namespace {

struct ClassToken {
  DiscrepancyClass value;
  const char* token;
};

// The order of this table is the validation precedence. It is a contract: a
// cell's primary classification is the first entry that applies. The test suite
// asserts this ordering explicitly.
constexpr std::array<ClassToken, 19> kClassTokens{{
    {DiscrepancyClass::ArithmeticOverflow, "arithmetic_overflow"},
    {DiscrepancyClass::EvidenceConflict, "evidence_conflict"},
    {DiscrepancyClass::NoEvidence, "no_evidence"},
    {DiscrepancyClass::GenerationRegression, "generation_regression"},
    {DiscrepancyClass::Stale, "stale"},
    {DiscrepancyClass::UnitSeparated, "unit_separated"},
    {DiscrepancyClass::Incomplete, "incomplete"},
    {DiscrepancyClass::ObservedShortfall, "observed_shortfall"},
    {DiscrepancyClass::ObservationExceedsInstalled, "observation_exceeds_installed"},
    {DiscrepancyClass::PlannedGap, "planned_gap"},
    {DiscrepancyClass::UnplannedInstall, "unplanned_install"},
    {DiscrepancyClass::UsableDerated, "usable_derated"},
    {DiscrepancyClass::UsableExceedsObserved, "usable_exceeds_observed"},
    {DiscrepancyClass::AllocatableOverstated, "allocatable_overstated"},
    {DiscrepancyClass::AllocatableUnderstated, "allocatable_understated"},
    {DiscrepancyClass::ReservationExceedsInstalled, "reservation_exceeds_installed"},
    {DiscrepancyClass::ReservationExceedsAllocatable, "reservation_exceeds_allocatable"},
    {DiscrepancyClass::UnexplainedResidual, "unexplained_residual"},
    {DiscrepancyClass::Agrees, "agrees"},
}};

struct TargetToken {
  ReasonTarget target;
  const char* token;
};

constexpr std::array<TargetToken, 9> kTargetTokens{{
    {ReasonTarget::None, "none"},
    {ReasonTarget::Cell, "cell"},
    {ReasonTarget::PlannedGap, "planned_gap"},
    {ReasonTarget::ObservedGap, "observed_gap"},
    {ReasonTarget::DerateGap, "derate_gap"},
    {ReasonTarget::HeadroomGap, "headroom_gap"},
    {ReasonTarget::ReservationPressure, "reservation_pressure"},
    {ReasonTarget::ReservationOverhang, "reservation_overhang"},
    {ReasonTarget::AllocatableSkew, "allocatable_skew"},
}};

struct StateToken {
  ResolutionState state;
  const char* token;
};

constexpr std::array<StateToken, 9> kStateTokens{{
    {ResolutionState::Open, "open"},
    {ResolutionState::Explained, "explained"},
    {ResolutionState::PartiallyExplained, "partially_explained"},
    {ResolutionState::Conflicted, "conflicted"},
    {ResolutionState::Stale, "stale"},
    {ResolutionState::Incomplete, "incomplete"},
    {ResolutionState::Superseded, "superseded"},
    {ResolutionState::Closed, "closed"},
    {ResolutionState::Reopened, "reopened"},
}};

}  // namespace

const char* to_string(DiscrepancyClass value) noexcept {
  for (const ClassToken& entry : kClassTokens) {
    if (entry.value == value) {
      return entry.token;
    }
  }
  return "unrecognised";
}

std::optional<DiscrepancyClass> discrepancy_class_from_string(std::string_view token) noexcept {
  for (const ClassToken& entry : kClassTokens) {
    if (token == entry.token) {
      return entry.value;
    }
  }
  return std::nullopt;
}

const char* to_string(ReasonTarget target) noexcept {
  for (const TargetToken& entry : kTargetTokens) {
    if (entry.target == target) {
      return entry.token;
    }
  }
  return "unrecognised";
}

const char* to_string(ResolutionState state) noexcept {
  for (const StateToken& entry : kStateTokens) {
    if (entry.state == state) {
      return entry.token;
    }
  }
  return "unrecognised";
}

std::optional<ResolutionState> resolution_state_from_string(std::string_view token) noexcept {
  for (const StateToken& entry : kStateTokens) {
    if (token == entry.token) {
      return entry.state;
    }
  }
  return std::nullopt;
}

std::string Finding::to_string() const {
  std::string out = summon::capacity_reconciliation::to_string(classification);
  out += " [target=";
  out += summon::capacity_reconciliation::to_string(target);
  out += " magnitude=";
  out += magnitude.has_value() ? std::to_string(*magnitude) : "unknown";
  out += " tolerance=";
  out += std::to_string(tolerance);
  out += exceeds_tolerance ? " exceeds_tolerance=true]" : " exceeds_tolerance=false]";
  if (!detail.empty()) {
    out += " ";
    out += detail;
  }
  return out;
}

bool finding_less(const Finding& a, const Finding& b) noexcept {
  if (a.classification != b.classification) {
    return static_cast<std::uint8_t>(a.classification) <
           static_cast<std::uint8_t>(b.classification);
  }
  if (a.target != b.target) {
    return static_cast<std::uint8_t>(a.target) < static_cast<std::uint8_t>(b.target);
  }
  const Amount a_magnitude = a.magnitude.value_or(0);
  const Amount b_magnitude = b.magnitude.value_or(0);
  if (a_magnitude != b_magnitude) {
    return a_magnitude < b_magnitude;
  }
  return a.detail < b.detail;
}

}  // namespace summon::capacity_reconciliation
