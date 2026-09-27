// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The reconciliation kernel.
//
// Read this file as the answer to the core question:
//
//   "When planned, reserved, installed, observed and available facility
//    capacity disagree, what can be reconciled deterministically, what remains
//    unexplained, and which evidence is authoritative enough to drive the next
//    decision?"
//
// The three mechanisms are:
//
//   1. Selection. Within one (scope, dimension key, view) the candidate
//      statements are reduced to one value by an explicit, digested precedence
//      policy. Statements whose producer revision is behind another statement
//      from the same source instance are dropped as superseded, not averaged.
//      Statements the policy cannot order become Unknown(Conflicted) and are
//      recorded as a conflict set -- never silently resolved by picking one.
//
//   2. Arithmetic. Every residual is computed with checked integer arithmetic
//      on exact units. Unknown propagates as Unknown; it is never coerced to
//      zero. Overflow makes the cell ArithmeticOverflow rather than wrapping.
//
//   3. Attribution. Upstream authorities may explain a residual with signed
//      attributions. The kernel subtracts exactly the attributions that name
//      the same scope, dimension key and residual, and *keeps whatever is
//      left*. There is no balancing step anywhere in this file.

#include "reconcile.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "summon/capacity_reconciliation/limits.hpp"
#include "summon/capacity_reconciliation/quantity.hpp"

namespace summon::capacity_reconciliation {
namespace {

/// Derive a node identity from the node's own content and its position in the
/// graph.
///
/// Identities inside a run must be *derived*, never random. A random identity
/// embedded in the canonical content would make two computations over identical
/// inputs produce different run digests, which destroys exactly the property
/// the digest exists to provide. Deriving the identity from the content also
/// means a consumer can refer to the same node across runs.
[[nodiscard]] NodeId derive_node_id(const ExplanationNode& node, std::uint32_t index) {
  CanonicalWriter writer;
  writer.token(to_string(node.role));
  writer.bytes(node.label);
  writer.boolean(node.value.is_known());
  if (node.value.is_known()) {
    writer.i64(node.value.value());
  } else {
    writer.token(to_string(node.reason));
  }
  writer.boolean(node.evidence_id.has_value());
  if (node.evidence_id.has_value()) {
    writer.bytes(node.evidence_id->to_string());
  }
  writer.boolean(node.attribution_id.has_value());
  if (node.attribution_id.has_value()) {
    writer.bytes(node.attribution_id->to_string());
  }
  writer.u64(static_cast<std::uint64_t>(node.operands.size()));
  for (std::uint32_t operand : node.operands) {
    writer.u32(operand);
  }
  writer.u32(index);
  const Digest digest = writer.digest("capacity-reconciliation/node-id/v1");
  std::array<std::uint8_t, 16> bytes{};
  std::copy_n(digest.bytes().begin(), bytes.size(), bytes.begin());
  if (std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t b) { return b == 0; })) {
    bytes[0] = 1;
  }
  return NodeId{bytes};
}

/// Derive a conflict identity from the conflict's own content and its position
/// among the conflicts of one resolved view. See derive_node_id.
[[nodiscard]] ConflictId derive_conflict_id(const EvidenceConflict& conflict,
                                            std::uint32_t index) {
  CanonicalWriter writer;
  writer.bytes(conflict.scope.to_string());
  writer.token(conflict.dimension.to_string());
  writer.token(to_string(conflict.view));
  writer.u64(static_cast<std::uint64_t>(conflict.participants.size()));
  for (const EvidenceId& participant : conflict.participants) {
    writer.bytes(participant.to_string());
  }
  for (const Digest& digest : conflict.participant_digests) {
    writer.bytes(digest.to_hex());
  }
  writer.u64(static_cast<std::uint64_t>(conflict.values.size()));
  for (Amount value : conflict.values) {
    writer.i64(value);
  }
  for (EvidenceSource source : conflict.asserting_sources) {
    writer.token(to_string(source));
  }
  writer.token(conflict.unresolvable_reason);
  writer.boolean(conflict.selected_value.has_value());
  if (conflict.selected_value.has_value()) {
    writer.i64(*conflict.selected_value);
  }
  writer.u32(index);
  const Digest digest = writer.digest("capacity-reconciliation/conflict-id/v1");
  std::array<std::uint8_t, 16> bytes{};
  std::copy_n(digest.bytes().begin(), bytes.size(), bytes.begin());
  if (std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t b) { return b == 0; })) {
    bytes[0] = 1;
  }
  return ConflictId{bytes};
}

/// A resolved value for one view at one scope, before rollup.
struct ResolvedView {
  bool present = false;
  Quantity value;  ///< known, or unknown with the reason that blocked it
  std::optional<EvidenceId> selected;
  EvidenceSource selected_source = EvidenceSource::Unknown;
  std::uint32_t candidate_count = 0;
  bool partial = false;
  bool stale = false;
  bool generation_regression = false;
  /// Evidence items consumed, in canonical order (provenance for the graph).
  std::vector<const EvidenceItem*> consumed;
  std::vector<EvidenceConflict> conflicts;
};

/// Effective value of one candidate statement, after status, freshness and
/// generation rules have been applied.
struct EffectiveCandidate {
  const EvidenceItem* item = nullptr;
  Quantity value;
  bool partial = false;
  bool stale = false;
  bool generation_regression = false;
};

bool scope_matches(const ScopeSelection& selection, const ScopeIdentity& evidence_scope) {
  if (selection.mode == ScopeSelectionMode::Exact) {
    return selection.scope == evidence_scope;
  }
  return selection.scope.is_ancestor_of(evidence_scope);
}

Result<EffectiveCandidate> effective_value(const EvidenceItem& item,
                                           const ReconciliationPolicy& policy, Tick instant,
                                           Generation run_generation,
                                           Generation min_generation) {
  EffectiveCandidate out;
  out.item = &item;

  if (item.status() == EvidenceStatus::Superseded) {
    return make_error(ErrorCode::Unknown, "statement is marked superseded");
  }

  switch (item.status()) {
    case EvidenceStatus::Unsupported:
      out.value = Quantity::unknown(UnknownReason::Unsupported);
      return out;
    case EvidenceStatus::Unavailable:
      out.value = Quantity::unknown(UnknownReason::SourceUnavailable);
      return out;
    case EvidenceStatus::Expired:
      out.value = Quantity::unknown(UnknownReason::Expired);
      out.stale = true;
      return out;
    case EvidenceStatus::Partial:
      out.partial = true;
      break;
    case EvidenceStatus::Accepted:
    case EvidenceStatus::Superseded:
      break;
  }

  // Generation fences are checked before freshness: a statement produced under
  // an older generation is refused on authority grounds even if its tick window
  // looks current.
  if (min_generation.value() != 0 && item.stamp().generation < min_generation) {
    out.value = Quantity::unknown(UnknownReason::GenerationTooOld);
    out.generation_regression = true;
    return out;
  }
  if (policy.max_generation_lag != 0) {
    if (item.stamp().generation > run_generation) {
      out.value = Quantity::unknown(UnknownReason::GenerationTooOld);
      out.generation_regression = true;
      return out;
    }
    const std::uint64_t lag = run_generation.value() - item.stamp().generation.value();
    if (lag > policy.max_generation_lag) {
      out.value = Quantity::unknown(UnknownReason::GenerationTooOld);
      out.generation_regression = true;
      return out;
    }
  }

  if (!item.is_current_at(instant)) {
    if (policy.stale_evidence == StaleEvidencePolicy::Reject) {
      out.value = Quantity::unknown(UnknownReason::Expired);
      out.stale = true;
      return out;
    }
    out.stale = true;
  }

  out.value = item.value();
  return out;
}

/// Reduce the candidate statements for one (scope, dimension key, view).
ResolvedView resolve_view(const std::vector<const EvidenceItem*>& candidates,
                          const ReconciliationPolicy& policy, Tick instant,
                          Generation run_generation, Generation min_generation) {
  ResolvedView resolved;
  resolved.candidate_count = static_cast<std::uint32_t>(candidates.size());
  if (candidates.empty()) {
    return resolved;
  }
  resolved.present = true;

  // Step 1: drop statements whose producer revision is behind another statement
  // from the same source instance. A newer revision from the same producer
  // replaces the older one; it is not a disagreement.
  std::vector<EffectiveCandidate> effective;
  effective.reserve(candidates.size());
  for (const EvidenceItem* item : candidates) {
    auto value = effective_value(*item, policy, instant, run_generation, min_generation);
    if (!value.ok()) {
      continue;  // explicitly superseded: dropped, not contributed
    }
    effective.push_back(std::move(value.value()));
  }
  if (effective.empty()) {
    resolved.value = Quantity::unknown(UnknownReason::Superseded);
    return resolved;
  }

  std::vector<EffectiveCandidate> kept;
  kept.reserve(effective.size());
  for (std::size_t i = 0; i < effective.size(); ++i) {
    bool superseded = false;
    for (std::size_t j = 0; j < effective.size(); ++j) {
      if (i == j) {
        continue;
      }
      if (effective[i].item->source() == effective[j].item->source() &&
          effective[i].item->source_instance() == effective[j].item->source_instance() &&
          effective[i].item->stamp().revision < effective[j].item->stamp().revision) {
        superseded = true;
        break;
      }
    }
    if (!superseded) {
      kept.push_back(effective[i]);
    }
  }
  effective = std::move(kept);

  for (const EffectiveCandidate& candidate : effective) {
    resolved.consumed.push_back(candidate.item);
    resolved.partial = resolved.partial || candidate.partial;
    resolved.stale = resolved.stale || candidate.stale;
    resolved.generation_regression =
        resolved.generation_regression || candidate.generation_regression;
  }

  // Step 2: find the most authoritative family present and keep only its
  // statements. A statement from a family the policy ranks lower never
  // determines the value; if it disagrees it is recorded, not adopted.
  std::size_t best_precedence = policy.precedence_of(effective.front().item->source());
  for (const EffectiveCandidate& candidate : effective) {
    best_precedence =
        std::min(best_precedence, policy.precedence_of(candidate.item->source()));
  }

  std::vector<const EffectiveCandidate*> best;
  std::vector<const EffectiveCandidate*> outranked;
  for (const EffectiveCandidate& candidate : effective) {
    if (policy.precedence_of(candidate.item->source()) == best_precedence) {
      best.push_back(&candidate);
    } else {
      outranked.push_back(&candidate);
    }
  }

  // Step 3: an equally authoritative statement that could not establish a value
  // blocks the view. A lower-ranked family's silence does not.
  for (const EffectiveCandidate* candidate : best) {
    if (candidate->value.is_unknown()) {
      resolved.value = Quantity::unknown(candidate->value.reason());
      resolved.selected = candidate->item->id();
      resolved.selected_source = candidate->item->source();
      return resolved;
    }
  }

  // Step 4: do the equally authoritative statements agree?
  const Amount agreed = best.front()->value.value();
  bool agrees = true;
  for (const EffectiveCandidate* candidate : best) {
    if (candidate->value.value() != agreed) {
      agrees = false;
      break;
    }
  }

  if (agrees) {
    resolved.value = Quantity::known(agreed);
  } else {
    resolved.value = Quantity::unknown(UnknownReason::Conflicted);
  }
  resolved.selected = best.front()->item->id();
  resolved.selected_source = best.front()->item->source();

  // Step 5: record the conflict set. A disagreement the policy *can* order is
  // recorded as resolved; a disagreement it cannot order is recorded as
  // unresolvable and is what makes the cell conflicted.
  const bool unresolved = !agrees;
  if (unresolved || !outranked.empty()) {
    std::vector<const EffectiveCandidate*> participants;
    participants.insert(participants.end(), best.begin(), best.end());
    participants.insert(participants.end(), outranked.begin(), outranked.end());

    EvidenceConflict conflict;
    conflict.scope = participants.front()->item->scope();
    conflict.dimension = participants.front()->item->dimension();
    conflict.view = participants.front()->item->view();

    std::vector<std::pair<Amount, EvidenceSource>> asserted;
    for (const EffectiveCandidate* candidate : participants) {
      conflict.participants.push_back(candidate->item->id());
      conflict.participant_digests.push_back(candidate->item->content_digest());
      if (candidate->value.is_unknown()) {
        continue;
      }
      const Amount value = candidate->value.value();
      bool seen = false;
      for (const auto& entry : asserted) {
        if (entry.first == value) {
          seen = true;
          break;
        }
      }
      if (!seen) {
        asserted.emplace_back(value, candidate->item->source());
      }
    }
    std::sort(asserted.begin(), asserted.end(),
              [](const std::pair<Amount, EvidenceSource>& a,
                 const std::pair<Amount, EvidenceSource>& b) { return a.first < b.first; });
    for (const auto& entry : asserted) {
      conflict.values.push_back(entry.first);
      conflict.asserting_sources.push_back(entry.second);
    }

    if (unresolved) {
      // Two statements from the *same producing instance* that disagree at the
      // same revision are a producer defect. Statements from different
      // instances of the same family, or from different families the policy
      // ranks equally, are a genuine tie the policy cannot break.
      bool same_instance = true;
      for (const EffectiveCandidate* candidate : best) {
        if (candidate->item->source() != best.front()->item->source() ||
            candidate->item->source_instance() != best.front()->item->source_instance()) {
          same_instance = false;
          break;
        }
      }
      conflict.unresolvable_reason =
          same_instance ? "intra_source_disagreement" : "equal_precedence";
    } else {
      conflict.unresolvable_reason = "outranked_by_precedence";
      conflict.selected_value = agreed;
    }
    conflict.id =
        derive_conflict_id(conflict, static_cast<std::uint32_t>(resolved.conflicts.size()));
    resolved.conflicts.push_back(std::move(conflict));
  }

  return resolved;
}

/// Per-scope view resolution, one entry per capacity view.
struct ScopeResolution {
  ScopeIdentity scope;
  std::array<ResolvedView, kCapacityViewCount> views;
};

/// Combine per-scope resolutions for one cell.
struct CellResolution {
  std::array<Quantity, kCapacityViewCount> views{};
  std::array<std::optional<EvidenceId>, kCapacityViewCount> selected{};
  std::array<EvidenceSource, kCapacityViewCount> selected_source{};
  std::array<std::uint32_t, kCapacityViewCount> candidate_count{};
  std::array<bool, kCapacityViewCount> partial{};
  bool stale = false;
  bool generation_regression = false;
  std::uint32_t rollup_contributors = 0;
  std::uint32_t rollup_missing = 0;
  std::vector<EvidenceConflict> conflicts;
};

void combine_views(const std::vector<ScopeResolution>& resolutions,
                   const ReconciliationPolicy& policy, CellResolution* out) {
  (void)policy;
  const bool rolled = resolutions.size() > 1;
  if (rolled) {
    out->rollup_contributors = static_cast<std::uint32_t>(resolutions.size());
  }
  for (std::size_t index = 0; index < kCapacityViewCount; ++index) {
    for (const ScopeResolution& resolution : resolutions) {
      const ResolvedView& view = resolution.views[index];
      out->stale = out->stale || view.stale;
      out->generation_regression = out->generation_regression || view.generation_regression;
      out->candidate_count[index] += view.candidate_count;
      out->partial[index] = out->partial[index] || view.partial;
      for (const EvidenceConflict& conflict : view.conflicts) {
        out->conflicts.push_back(conflict);
      }
    }

    if (!rolled) {
      const ResolvedView& view = resolutions.front().views[index];
      if (!view.present) {
        out->views[index] = Quantity::unknown(UnknownReason::NotReported);
        continue;
      }
      out->views[index] = view.value;
      out->selected[index] = view.selected;
      out->selected_source[index] = view.selected_source;
      continue;
    }

    // A rollup sums the contributions of *every* contributing scope. A scope
    // that has statements about the cell but nothing for this view is a missing
    // contributor, and a missing contributor makes the total unknown. Emitting
    // the partial sum here would be the classic silent under-report.
    bool any_missing = false;
    UnknownReason value_reason = UnknownReason::None;
    Quantity total = Quantity::known(0);
    for (const ScopeResolution& resolution : resolutions) {
      const ResolvedView& view = resolution.views[index];
      if (!view.present) {
        any_missing = true;
        ++out->rollup_missing;
        continue;
      }
      if (view.value.is_unknown()) {
        any_missing = true;
        ++out->rollup_missing;
        value_reason = combine_unknown_reasons(value_reason, view.value.reason());
        continue;
      }
      auto sum = add_quantities(total, view.value, UnknownReason::PartialRollup);
      if (!sum.ok()) {
        any_missing = true;
        ++out->rollup_missing;
        value_reason = combine_unknown_reasons(value_reason, UnknownReason::PartialRollup);
        break;
      }
      total = sum.value();
    }

    if (any_missing) {
      // The partial sum is deliberately *not* promoted to the value. It is
      // retained in the explanation graph instead, where the sum node carries
      // it for an operator to read.
      out->views[index] = Quantity::unknown(
          value_reason == UnknownReason::None ? UnknownReason::PartialRollup : value_reason);
    } else {
      out->views[index] = total;
    }
  }
}

struct CellKey {
  ScopeIdentity scope;
  DimensionKey dimension;

  friend bool operator<(const CellKey& a, const CellKey& b) noexcept {
    if (!(a.scope == b.scope)) {
      return a.scope < b.scope;
    }
    return a.dimension < b.dimension;
  }
  friend bool operator==(const CellKey& a, const CellKey& b) noexcept {
    return a.scope == b.scope && a.dimension == b.dimension;
  }
};

/// Key of one (contributing scope, dimension key, view) group of statements.
struct GroupKey {
  std::size_t scope_index = 0;
  DimensionKey dimension;
  CapacityView view = CapacityView::Planned;

  friend bool operator<(const GroupKey& a, const GroupKey& b) noexcept {
    if (a.scope_index != b.scope_index) {
      return a.scope_index < b.scope_index;
    }
    if (a.dimension != b.dimension) {
      return a.dimension < b.dimension;
    }
    return view_index(a.view) < view_index(b.view);
  }
};

/// In which direction a residual is an anomaly. A residual whose expected value
/// is zero is anomalous either way; a residual that encodes a one-sided
/// constraint (reservations must not exceed nameplate, declared headroom must
/// not exceed usable) is anomalous only in the violating direction. The
/// direction governs whether the residual is reported as *unexplained*, so that
/// a normal, healthy cell does not drown in noise.
enum class AnomalyDirection : std::uint8_t { Either = 0, Positive = 1, Negative = 2 };

/// One cell's residual definition: what is subtracted from what, which
/// attribution target explains it, and which classifications it can raise.
struct ResidualSpec {
  ReasonTarget target;
  CapacityView minuend;
  CapacityView subtrahend;
  Quantity ReconciliationCell::*result_slot;
  DiscrepancyClass positive_class;
  DiscrepancyClass negative_class;
  const char* positive_detail;
  const char* negative_detail;
  CapacityView tolerance_reference;
  AnomalyDirection anomaly;
};

const std::array<ResidualSpec, 5>& residual_specs() {
  static const std::array<ResidualSpec, 5> kSpecs{{
      {ReasonTarget::PlannedGap, CapacityView::Planned, CapacityView::Installed,
       &ReconciliationCell::planned_gap, DiscrepancyClass::PlannedGap,
       DiscrepancyClass::UnplannedInstall, "planned capacity exceeds installed capacity",
       "installed capacity exceeds planned capacity", CapacityView::Planned,
       AnomalyDirection::Either},
      {ReasonTarget::ObservedGap, CapacityView::Installed, CapacityView::Observed,
       &ReconciliationCell::observed_gap, DiscrepancyClass::ObservedShortfall,
       DiscrepancyClass::ObservationExceedsInstalled,
       "installed capacity is not observed as deliverable",
       "observed capacity exceeds installed capacity", CapacityView::Installed,
       AnomalyDirection::Either},
      {ReasonTarget::DerateGap, CapacityView::Observed, CapacityView::Usable,
       &ReconciliationCell::derate_gap, DiscrepancyClass::UsableDerated,
       DiscrepancyClass::UsableExceedsObserved,
       "usable capacity is derated below what is observed to be deliverable",
       "usable capacity claims more than is observed to be deliverable",
       CapacityView::Observed, AnomalyDirection::Either},
      // Headroom held back is normal when reservations exist; only declared
      // allocatable *exceeding* usable is a violation.
      {ReasonTarget::HeadroomGap, CapacityView::Usable, CapacityView::Allocatable,
       &ReconciliationCell::headroom_gap, DiscrepancyClass::Agrees,
       DiscrepancyClass::AllocatableOverstated, "",
       "declared allocatable capacity exceeds usable capacity", CapacityView::Usable,
       AnomalyDirection::Negative},
      // Reservations below nameplate are normal; only exceeding it is a
      // violation.
      {ReasonTarget::ReservationPressure, CapacityView::Reserved, CapacityView::Installed,
       &ReconciliationCell::reservation_pressure,
       DiscrepancyClass::ReservationExceedsInstalled, DiscrepancyClass::Agrees,
       "reservations exceed installed capacity", "", CapacityView::Reserved,
       AnomalyDirection::Positive},
  }};
  return kSpecs;
}

/// Anomaly direction for the two residuals that are not part of the uniform
/// table. Allocation skew is expected to be exactly zero, so either sign is an
/// anomaly; reservation overhang is a one-sided constraint like the others.
constexpr AnomalyDirection kAllocatableSkewAnomaly = AnomalyDirection::Either;
constexpr AnomalyDirection kReservationOverhangAnomaly = AnomalyDirection::Positive;

[[nodiscard]] bool is_anomalous(AnomalyDirection direction, Amount value) noexcept {
  switch (direction) {
    case AnomalyDirection::Either:
      return value != 0;
    case AnomalyDirection::Positive:
      return value > 0;
    case AnomalyDirection::Negative:
      return value < 0;
  }
  return false;
}

constexpr ReasonTarget kAllocatableSkewTarget = ReasonTarget::AllocatableSkew;

struct FindingContext {
  ToleranceBand tolerance;
  Amount tolerance_bound = 0;
};

void consider(DiscrepancyClass classification, ReasonTarget target, const Quantity& value,
              const FindingContext& context, const char* detail,
              std::vector<Finding>* findings) {
  if (value.is_unknown() || classification == DiscrepancyClass::Agrees) {
    return;
  }
  const Amount magnitude = value.value();
  const Amount absolute = magnitude < 0 ? -magnitude : magnitude;
  const bool exceeds = absolute > context.tolerance_bound;
  Finding finding;
  finding.classification = classification;
  finding.target = target;
  finding.magnitude = magnitude;
  finding.tolerance = context.tolerance_bound;
  finding.exceeds_tolerance = exceeds;
  if (exceeds && detail != nullptr) {
    finding.detail = detail;
  }
  findings->push_back(std::move(finding));
}

struct AttributionKey {
  std::string scope;
  std::string dimension;
  std::uint8_t target = 0;

  friend bool operator<(const AttributionKey& a, const AttributionKey& b) noexcept {
    if (a.scope != b.scope) {
      return a.scope < b.scope;
    }
    if (a.dimension != b.dimension) {
      return a.dimension < b.dimension;
    }
    return a.target < b.target;
  }
};

constexpr std::uint8_t residual_key(ResidualKind kind) noexcept {
  return static_cast<std::uint8_t>(kind);
}

ResidualKind residual_kind_for(ReasonTarget target) noexcept {
  switch (target) {
    case ReasonTarget::PlannedGap:
      return ResidualKind::PlannedGap;
    case ReasonTarget::ObservedGap:
      return ResidualKind::ObservedGap;
    case ReasonTarget::DerateGap:
      return ResidualKind::DerateGap;
    case ReasonTarget::HeadroomGap:
      return ResidualKind::HeadroomGap;
    case ReasonTarget::ReservationPressure:
      return ResidualKind::ReservationPressure;
    case ReasonTarget::ReservationOverhang:
      return ResidualKind::ReservationOverhang;
    case ReasonTarget::AllocatableSkew:
    case ReasonTarget::None:
    case ReasonTarget::Cell:
      return ResidualKind::None;
  }
  return ResidualKind::None;
}

/// Collect the attributions that explain one residual of one cell, in canonical
/// order, skipping those that are not current at the evaluation instant.
std::vector<const AttributionItem*> matching_attributions(
    const std::map<AttributionKey, std::vector<const AttributionItem*>>& index,
    const std::vector<ScopeIdentity>& scopes, const DimensionKey& dimension, ResidualKind kind,
    Tick instant) {
  std::vector<const AttributionItem*> matched;
  for (const ScopeIdentity& scope : scopes) {
    auto it = index.find(AttributionKey{scope.to_string(), dimension.to_string(),
                                        residual_key(kind)});
    if (it == index.end()) {
      continue;
    }
    for (const AttributionItem* item : it->second) {
      if (item->is_current_at(instant)) {
        matched.push_back(item);
      }
    }
  }
  std::sort(matched.begin(), matched.end(),
            [](const AttributionItem* a, const AttributionItem* b) { return *a < *b; });
  return matched;
}

}  // namespace

Result<ReconciliationRun> reconcile_kernel(const KernelInput& input, KernelStats* stats) {
  if (input.evidence == nullptr || input.attributions == nullptr || input.request == nullptr ||
      input.limits == nullptr) {
    return make_error(ErrorCode::InvalidArgument, "reconcile_kernel requires all inputs",
                      "argument");
  }
  // Canonicalise the scope selection order. The run digest must depend on
  // *which* scopes were selected, never on the order the caller happened to
  // list them in, so the order is normalised here rather than trusted.
  RunRequest sorted_request = *input.request;
  std::sort(sorted_request.scopes.begin(), sorted_request.scopes.end(),
            [](const ScopeSelection& a, const ScopeSelection& b) {
              return a.scope < b.scope;
            });
  const RunRequest& request = sorted_request;
  const Limits& limits = *input.limits;
  const ReconciliationPolicy& policy = request.policy;

  if (request.scopes.empty()) {
    return make_error(ErrorCode::InvalidArgument, "a run requires at least one scope", "scopes");
  }
  if (request.scopes.size() > limits.max_scope_selections) {
    return make_error(ErrorCode::LimitExceeded,
                      "run requests " + std::to_string(request.scopes.size()) +
                          " scope selections, above the limit of " +
                          std::to_string(limits.max_scope_selections),
                      "scope_selections");
  }
  if (request.clock_domain.empty()) {
    return make_error(ErrorCode::InvalidArgument, "a run requires a declared clock domain",
                      "clock_domain");
  }
  if (!policy.record_conflict_sets || !policy.report_unexplained_residuals) {
    return make_error(ErrorCode::Unsupported,
                      "the policy disables conflict sets or unexplained-residual reporting; "
                      "this runtime refuses to be configured to hide a discrepancy",
                      "policy");
  }
  for (std::size_t i = 0; i < request.scopes.size(); ++i) {
    for (std::size_t j = i + 1; j < request.scopes.size(); ++j) {
      if (request.scopes[i].scope == request.scopes[j].scope) {
        return make_error(ErrorCode::AlreadyExists,
                          "scope '" + request.scopes[i].scope.to_string() +
                              "' is selected more than once in the same run",
                          "scope_selection");
      }
    }
  }

  const std::vector<EvidenceItem>& evidence = input.evidence->items();
  const std::vector<AttributionItem>& attributions = input.attributions->items();

  // Evidence produced after the run's generation cannot be seen by the run, and
  // ticks from a different clock domain cannot be compared with the run's.
  for (const EvidenceItem& item : evidence) {
    if (item.stamp().generation > request.generation) {
      return make_error(ErrorCode::StaleGeneration,
                        "evidence " + item.id().to_string() + " was produced at generation " +
                            std::to_string(item.stamp().generation.value()) +
                            ", beyond the run generation " +
                            std::to_string(request.generation.value()) +
                            "; a run cannot consume evidence from the future",
                        "evidence_generation");
    }
    if (!(item.stamp().clock_domain == request.clock_domain)) {
      return make_error(ErrorCode::InvalidArgument,
                        "evidence " + item.id().to_string() + " is stamped in clock domain '" +
                            item.stamp().clock_domain.name() + "' but the run is evaluated in '" +
                            request.clock_domain.name() + "'",
                        "clock_domain");
    }
  }
  for (const AttributionItem& item : attributions) {
    if (item.stamp().generation > request.generation) {
      return make_error(ErrorCode::StaleGeneration,
                        "attribution " + item.id().to_string() +
                            " was produced beyond the run generation",
                        "attribution_generation");
    }
    if (!(item.stamp().clock_domain == request.clock_domain)) {
      return make_error(ErrorCode::InvalidArgument,
                        "attribution " + item.id().to_string() +
                            " is stamped in a different clock domain than the run",
                        "clock_domain");
    }
  }

  // ---- index evidence by scope, then resolve each selection -------------
  //
  // Grouping by scope once keeps the cost of a run proportional to the evidence
  // plus the selections rather than to their product, and it makes the
  // descendant walk of a rollup touch one entry per *scope* instead of one
  // entry per statement. Evidence is already in canonical order, so each
  // group's contents are too.
  std::map<ScopeIdentity, std::vector<const EvidenceItem*>> by_scope;
  for (const EvidenceItem& item : evidence) {
    by_scope[item.scope()].push_back(&item);
  }

  std::vector<std::vector<const EvidenceItem*>> per_selection(request.scopes.size());
  for (std::size_t s = 0; s < request.scopes.size(); ++s) {
    const ScopeSelection& selection = request.scopes[s];
    if (selection.mode == ScopeSelectionMode::Exact) {
      auto exact = by_scope.find(selection.scope);
      if (exact != by_scope.end()) {
        per_selection[s] = exact->second;
      }
      continue;
    }
    for (const auto& group : by_scope) {
      if (selection.scope.is_ancestor_of(group.first)) {
        per_selection[s].insert(per_selection[s].end(), group.second.begin(),
                                group.second.end());
      }
    }
    // The map is ordered by scope, so the concatenation of its groups must be
    // re-sorted to restore the canonical evidence order the kernel relies on.
    std::sort(per_selection[s].begin(), per_selection[s].end(),
              [](const EvidenceItem* a, const EvidenceItem* b) { return evidence_less(*a, *b); });
  }

  KernelStats local_stats;
  local_stats.evidence_considered = evidence.size();
  local_stats.attributions_considered = attributions.size();

  // ---- index attributions ----------------------------------------------
  std::map<AttributionKey, std::vector<const AttributionItem*>> attribution_index;
  for (const AttributionItem& item : attributions) {
    attribution_index[AttributionKey{item.scope().to_string(), item.dimension().to_string(),
                                     residual_key(item.target())}]
        .push_back(&item);
  }

  std::vector<ReconciliationCell> cells;
  std::vector<CellKey> emitted_keys;

  for (std::size_t s = 0; s < request.scopes.size(); ++s) {
    const ScopeSelection& selection = request.scopes[s];

    // Which scopes contribute to this selection?
    std::vector<ScopeIdentity> contributing_scopes;
    for (const EvidenceItem* item : per_selection[s]) {
      if (std::find(contributing_scopes.begin(), contributing_scopes.end(), item->scope()) ==
          contributing_scopes.end()) {
        contributing_scopes.push_back(item->scope());
      }
    }
    std::sort(contributing_scopes.begin(), contributing_scopes.end());
    if (contributing_scopes.empty()) {
      continue;  // a selection with no evidence produces no cell
    }

    std::map<GroupKey, std::vector<const EvidenceItem*>> groups;
    std::vector<DimensionKey> dimensions;
    for (const EvidenceItem* item : per_selection[s]) {
      std::size_t scope_index = 0;
      for (; scope_index < contributing_scopes.size(); ++scope_index) {
        if (contributing_scopes[scope_index] == item->scope()) {
          break;
        }
      }
      GroupKey key;
      key.scope_index = scope_index;
      key.dimension = item->dimension();
      key.view = item->view();
      groups[key].push_back(item);

      if (std::find(dimensions.begin(), dimensions.end(), item->dimension()) ==
          dimensions.end()) {
        dimensions.push_back(item->dimension());
      }
    }
    std::sort(dimensions.begin(), dimensions.end());

    if (cells.size() + dimensions.size() > limits.max_cells_per_run) {
      return make_error(ErrorCode::LimitExceeded,
                        "the run produces more than " +
                            std::to_string(limits.max_cells_per_run) + " cells",
                        "cells_per_run");
    }

    for (const DimensionKey& dimension : dimensions) {
      CellKey cell_key;
      cell_key.scope = selection.scope;
      cell_key.dimension = dimension;
      if (std::find(emitted_keys.begin(), emitted_keys.end(), cell_key) != emitted_keys.end()) {
        return make_error(ErrorCode::Conflict,
                          "scope selections overlap: cell " + cell_key.scope.to_string() + " " +
                              cell_key.dimension.to_string() +
                              " would be produced twice by different selections",
                          "scope_selection");
      }
      emitted_keys.push_back(cell_key);

      std::size_t total_evidence = 0;
      for (const EvidenceItem* item : per_selection[s]) {
        if (item->dimension() == dimension) {
          ++total_evidence;
        }
      }
      if (total_evidence > limits.max_evidence_per_cell) {
        return make_error(ErrorCode::LimitExceeded,
                          "cell " + cell_key.scope.to_string() + " " + dimension.to_string() +
                              " has " + std::to_string(total_evidence) +
                              " evidence items, above the per-cell limit of " +
                              std::to_string(limits.max_evidence_per_cell),
                          "evidence_per_cell");
      }

      // Resolve each contributing scope.
      std::vector<ScopeResolution> resolutions;
      for (std::size_t scope_index = 0; scope_index < contributing_scopes.size(); ++scope_index) {
        ScopeResolution resolution;
        resolution.scope = contributing_scopes[scope_index];
        bool any = false;
        for (std::size_t v = 0; v < kCapacityViewCount; ++v) {
          GroupKey key;
          key.scope_index = scope_index;
          key.dimension = dimension;
          key.view = static_cast<CapacityView>(v);
          auto it = groups.find(key);
          static const std::vector<const EvidenceItem*> kEmpty;
          const std::vector<const EvidenceItem*>& candidates =
              (it == groups.end()) ? kEmpty : it->second;
          resolution.views[v] =
              resolve_view(candidates, policy, request.evaluation_instant, request.generation,
                           request.min_evidence_generation);
          any = any || resolution.views[v].present;
        }
        if (any) {
          resolutions.push_back(std::move(resolution));
        }
      }
      if (resolutions.empty()) {
        continue;
      }

      ReconciliationCell cell;
      cell.scope = cell_key.scope;
      cell.dimension = dimension;

      CellResolution combined;
      combine_views(resolutions, policy, &combined);
      if (combined.conflicts.size() > limits.max_conflicts_per_cell) {
        return make_error(ErrorCode::LimitExceeded,
                          "cell " + cell_key.scope.to_string() + " " + dimension.to_string() +
                              " produced " + std::to_string(combined.conflicts.size()) +
                              " conflicts, above the per-cell limit of " +
                              std::to_string(limits.max_conflicts_per_cell),
                          "conflicts_per_cell");
      }
      cell.views.views = combined.views;
      cell.views.selected = combined.selected;
      cell.views.selected_source = combined.selected_source;
      cell.views.candidate_count = combined.candidate_count;
      cell.views.partial = combined.partial;
      cell.conflicts = std::move(combined.conflicts);
      cell.rolled_up = selection.mode == ScopeSelectionMode::SubtreeRollup;
      if (!cell.rolled_up) {
        cell.rollup_contributors = 0;
        cell.rollup_missing = 0;
      } else {
        cell.rollup_contributors = static_cast<std::uint32_t>(resolutions.size());
        cell.rollup_missing = combined.rollup_missing;
      }
      cell.evidence_seen = total_evidence;
      local_stats.cells += 1;

      // Unit separation: the same dimension expressed in another unit inside
      // this scope is recorded, never combined.
      for (const EvidenceItem* item : per_selection[s]) {
        if (same_dimension_different_unit(item->dimension(), dimension)) {
          if (std::find(cell.sibling_units.begin(), cell.sibling_units.end(),
                        item->dimension().unit()) == cell.sibling_units.end()) {
            cell.sibling_units.push_back(item->dimension().unit());
          }
        }
      }
      std::sort(cell.sibling_units.begin(), cell.sibling_units.end(), [](Unit a, Unit b) {
        return static_cast<std::uint8_t>(a) < static_cast<std::uint8_t>(b);
      });

      auto& graph = cell.explanation;

      auto derive_node = [&](NodeRole role, std::vector<std::uint32_t> operands,
                             const Quantity& value,
                             std::string label) -> Result<std::uint32_t> {
        ExplanationNode node;
        node.role = role;
        node.operands = std::move(operands);
        node.value = value;
        node.reason = value.is_known() ? UnknownReason::None : value.reason();
        node.label = std::move(label);
        node.id = derive_node_id(node, static_cast<std::uint32_t>(graph.size()));
        return graph.add(std::move(node), limits.max_explanation_nodes_per_cell);
      };

      // View nodes, one per consumed statement.
      std::array<std::uint32_t, kCapacityViewCount> view_value_nodes{};
      for (std::size_t v = 0; v < kCapacityViewCount; ++v) {
        const auto view = static_cast<CapacityView>(v);
        std::vector<std::uint32_t> node_indices;
        for (const ScopeResolution& resolution : resolutions) {
          for (const EvidenceItem* item : resolution.views[v].consumed) {
            ExplanationNode node;
            node.role = NodeRole::EvidenceInput;
            node.evidence_id = item->id();
            node.value = item->value();
            node.reason = item->value().is_known() ? UnknownReason::None : item->value().reason();
            node.label = std::string(to_string(item->source())) + "." + item->source_instance();
            node.id = derive_node_id(node, static_cast<std::uint32_t>(graph.size()));
            auto index = graph.add(std::move(node), limits.max_explanation_nodes_per_cell);
            if (!index.ok()) {
              return index.error();
            }
            node_indices.push_back(index.value());
          }
        }

        if (node_indices.empty()) {
          auto index = derive_node(NodeRole::UnknownInput, {},
                                   Quantity::unknown(UnknownReason::NotReported),
                                   std::string("view.") + to_string(view));
          if (!index.ok()) {
            return index.error();
          }
          view_value_nodes[v] = index.value();
        } else if (node_indices.size() == 1) {
          view_value_nodes[v] = node_indices.front();
        } else {
          auto index = derive_node(NodeRole::Sum, std::move(node_indices), cell.views.views[v],
                                   std::string("rollup.") + to_string(view));
          if (!index.ok()) {
            return index.error();
          }
          view_value_nodes[v] = index.value();
        }
      }

      bool overflow = false;

      // Applies the attributions for one residual to the graph, returning the
      // total explained.
      auto apply_explained = [&](ReasonTarget target, ResidualKind kind,
                                 std::vector<std::uint32_t>* attribution_nodes) -> Quantity {
        Quantity attributed = Quantity::known(0);
        const std::vector<const AttributionItem*> matched =
            matching_attributions(attribution_index, contributing_scopes, dimension, kind,
                                  request.evaluation_instant);
        for (const AttributionItem* item : matched) {
          auto sum = add_quantities(attributed, Quantity::known(item->amount()),
                                    UnknownReason::PartialRollup);
          if (!sum.ok()) {
            overflow = true;
            return Quantity::unknown(UnknownReason::PartialRollup);
          }
          attributed = sum.value();
          ++cell.attributions_applied;

          ExplanationNode node;
          node.role = NodeRole::AttributionInput;
          node.attribution_id = item->id();
          node.value = Quantity::known(item->amount());
          node.label = std::string("attributed:") + to_string(item->reason()) + ":" +
                       item->source_instance();
          node.id = derive_node_id(node, static_cast<std::uint32_t>(graph.size()));
          auto index = graph.add(std::move(node), limits.max_explanation_nodes_per_cell);
          if (!index.ok()) {
            overflow = true;
            return Quantity::unknown(UnknownReason::PartialRollup);
          }
          attribution_nodes->push_back(index.value());
        }
        (void)target;
        return attributed;
      };

      for (const ResidualSpec& spec : residual_specs()) {
        const Quantity& minuend = cell.views.views[view_index(spec.minuend)];
        const Quantity& subtrahend = cell.views.views[view_index(spec.subtrahend)];
        auto difference = sub_quantities(minuend, subtrahend, UnknownReason::NotReported);
        if (!difference.ok()) {
          overflow = true;
          cell.*(spec.result_slot) = Quantity::unknown(UnknownReason::NotReported);
          continue;
        }
        cell.*(spec.result_slot) = difference.value();

        auto difference_node = derive_node(
            NodeRole::Difference,
            {view_value_nodes[view_index(spec.minuend)], view_value_nodes[view_index(spec.subtrahend)]},
            difference.value(), std::string("residual.") + to_string(spec.target));
        if (!difference_node.ok()) {
          return difference_node.error();
        }

        std::vector<std::uint32_t> attribution_nodes;
        const ResidualKind kind = residual_kind_for(spec.target);
        Quantity attributed =
            (kind == ResidualKind::None)
                ? Quantity::known(0)
                : apply_explained(spec.target, kind, &attribution_nodes);

        auto explained = derive_node(NodeRole::ExplainedResidual, attribution_nodes, attributed,
                                     std::string("explained.") + to_string(spec.target));
        if (!explained.ok()) {
          return explained.error();
        }

        auto unexplained = sub_quantities(difference.value(), attributed,
                                          UnknownReason::NotReported);
        Quantity unexplained_value = unexplained.ok() ? unexplained.value()
                                                      : Quantity::unknown(UnknownReason::NotReported);
        if (!unexplained.ok()) {
          overflow = true;
        }
        auto unexplained_node =
            derive_node(NodeRole::UnexplainedResidual,
                        {difference_node.value(), explained.value()}, unexplained_value,
                        std::string("unexplained.") + to_string(spec.target));
        if (!unexplained_node.ok()) {
          return unexplained_node.error();
        }

        ResidualExplanation explanation;
        explanation.target = spec.target;
        explanation.raw = difference.value();
        explanation.attributed = attributed;
        explanation.unexplained = unexplained_value;
        explanation.unexplained_node = unexplained_node.value();
        explanation.anomalous = unexplained_value.is_known() &&
                                is_anomalous(spec.anomaly, unexplained_value.value());
        auto added = graph.add_residual(std::move(explanation),
                                        limits.max_explanation_residuals_per_cell);
        if (!added.ok()) {
          return added.error();
        }
      }

      // Derived allocatable and its skew against the declared value.
      {
        auto derived = sub_quantities(cell.views.views[view_index(CapacityView::Usable)],
                                      cell.views.views[view_index(CapacityView::Reserved)],
                                      UnknownReason::NotReported);
        if (!derived.ok()) {
          overflow = true;
          cell.derived_allocatable = Quantity::unknown(UnknownReason::NotReported);
          cell.allocatable_skew = Quantity::unknown(UnknownReason::NotReported);
        } else {
          cell.derived_allocatable = derived.value();
          auto skew = sub_quantities(cell.views.views[view_index(CapacityView::Allocatable)],
                                     derived.value(), UnknownReason::NotReported);
          if (!skew.ok()) {
            overflow = true;
            cell.allocatable_skew = Quantity::unknown(UnknownReason::NotReported);
          } else {
            cell.allocatable_skew = skew.value();
            auto derived_node = derive_node(
                NodeRole::Difference,
                {view_value_nodes[view_index(CapacityView::Usable)],
                 view_value_nodes[view_index(CapacityView::Reserved)]},
                derived.value(), "derived.allocatable");
            if (!derived_node.ok()) {
              return derived_node.error();
            }
            auto skew_node = derive_node(
                NodeRole::Difference,
                {view_value_nodes[view_index(CapacityView::Allocatable)], derived_node.value()},
                skew.value(), "residual.allocatable_skew");
            if (!skew_node.ok()) {
              return skew_node.error();
            }

            ResidualExplanation explanation;
            explanation.target = kAllocatableSkewTarget;
            explanation.raw = skew.value();
            explanation.attributed = Quantity::known(0);
            explanation.unexplained = skew.value();
            explanation.unexplained_node = skew_node.value();
            explanation.anomalous = skew.value().is_known() &&
                                    is_anomalous(kAllocatableSkewAnomaly, skew.value().value());
            auto added = graph.add_residual(std::move(explanation),
                                            limits.max_explanation_residuals_per_cell);
            if (!added.ok()) {
              return added.error();
            }
          }
        }

        // Reservation overhang: reservations versus declared allocatable.
        auto overhang = sub_quantities(cell.views.views[view_index(CapacityView::Reserved)],
                                       cell.views.views[view_index(CapacityView::Allocatable)],
                                       UnknownReason::NotReported);
        if (!overhang.ok()) {
          overflow = true;
          cell.reservation_overhang = Quantity::unknown(UnknownReason::NotReported);
        } else {
          cell.reservation_overhang = overhang.value();

          auto overhang_node = derive_node(
              NodeRole::Difference,
              {view_value_nodes[view_index(CapacityView::Reserved)],
               view_value_nodes[view_index(CapacityView::Allocatable)]},
              overhang.value(), "residual.reservation_overhang");
          if (!overhang_node.ok()) {
            return overhang_node.error();
          }

          std::vector<std::uint32_t> attribution_nodes;
          Quantity attributed =
              apply_explained(ReasonTarget::ReservationOverhang, ResidualKind::ReservationOverhang,
                              &attribution_nodes);

          auto explained = derive_node(NodeRole::ExplainedResidual, attribution_nodes, attributed,
                                       "explained.reservation_overhang");
          if (!explained.ok()) {
            return explained.error();
          }
          auto unexplained = sub_quantities(overhang.value(), attributed,
                                            UnknownReason::NotReported);
          Quantity unexplained_value = unexplained.ok()
                                           ? unexplained.value()
                                           : Quantity::unknown(UnknownReason::NotReported);
          if (!unexplained.ok()) {
            overflow = true;
          }
          auto unexplained_node = derive_node(
              NodeRole::UnexplainedResidual, {overhang_node.value(), explained.value()},
              unexplained_value, "unexplained.reservation_overhang");
          if (!unexplained_node.ok()) {
            return unexplained_node.error();
          }

          ResidualExplanation explanation;
          explanation.target = ReasonTarget::ReservationOverhang;
          explanation.raw = overhang.value();
          explanation.attributed = attributed;
          explanation.unexplained = unexplained_value;
          explanation.unexplained_node = unexplained_node.value();
          explanation.anomalous = unexplained_value.is_known() &&
                                  is_anomalous(kReservationOverhangAnomaly,
                                               unexplained_value.value());
          auto added = graph.add_residual(std::move(explanation),
                                          limits.max_explanation_residuals_per_cell);
          if (!added.ok()) {
            return added.error();
          }
        }
      }

      // ---- findings ------------------------------------------------------
      const std::vector<CapacityView>& required = policy.required_views_for(dimension);
      const ToleranceBand dimension_tolerance = policy.tolerance_for(dimension);

      auto push_cell_finding = [&cell](DiscrepancyClass classification, const char* detail) {
        Finding finding;
        finding.classification = classification;
        finding.target = ReasonTarget::Cell;
        if (detail != nullptr) {
          finding.detail = detail;
        }
        cell.findings.push_back(std::move(finding));
      };

      if (overflow) {
        push_cell_finding(DiscrepancyClass::ArithmeticOverflow,
                          "checked arithmetic overflowed while computing a residual");
      } else {
        bool any_known_view = false;
        for (std::size_t v = 0; v < kCapacityViewCount; ++v) {
          if (cell.views.views[v].is_known()) {
            any_known_view = true;
            break;
          }
        }
        if (!any_known_view) {
          push_cell_finding(DiscrepancyClass::NoEvidence,
                            "statements exist for this cell but none of them establishes a value");
        }

        for (const EvidenceConflict& conflict : cell.conflicts) {
          if (!conflict.selected_value.has_value()) {
            push_cell_finding(DiscrepancyClass::EvidenceConflict,
                              "authoritative statements disagree and the policy cannot order "
                              "them");
            break;
          }
        }

        if (combined.generation_regression) {
          push_cell_finding(DiscrepancyClass::GenerationRegression,
                            "consumed evidence is behind the run's accepted generation");
        }
        if (combined.stale) {
          push_cell_finding(DiscrepancyClass::Stale,
                            "consumed evidence is outside its validity window");
        }
        if (!cell.sibling_units.empty()) {
          push_cell_finding(DiscrepancyClass::UnitSeparated,
                            "the same dimension is present in more than one unit; no cross-unit "
                            "total is produced");
        }

        for (std::size_t v = 0; v < kCapacityViewCount; ++v) {
          const auto view = static_cast<CapacityView>(v);
          if (std::find(required.begin(), required.end(), view) == required.end()) {
            continue;
          }
          const bool missing =
              cell.views.partial[v] ||
              (!cell.views.selected[v].has_value() && cell.views.views[v].is_unknown());
          if (missing) {
            Finding finding;
            finding.classification = DiscrepancyClass::Incomplete;
            finding.target = ReasonTarget::Cell;
            finding.detail = std::string("required view '") + to_string(view) +
                             "' is not established: " + cell.views.views[v].to_string();
            cell.findings.push_back(std::move(finding));
          }
        }

        for (const ResidualSpec& spec : residual_specs()) {
          const Quantity& value = cell.*(spec.result_slot);
          if (value.is_unknown() || value.value() == 0) {
            continue;
          }
          const Quantity& reference = cell.views.views[view_index(spec.tolerance_reference)];
          auto bound =
              policy_tolerance_bound(dimension_tolerance, reference.is_known() ? reference.value()
                                                                              : 0);
          if (!bound.ok()) {
            return bound.error();
          }
          const FindingContext context{dimension_tolerance, bound.value()};
          if (value.value() > 0) {
            consider(spec.positive_class, spec.target, value, context, spec.positive_detail,
                     &cell.findings);
          } else {
            consider(spec.negative_class, spec.target, value, context, spec.negative_detail,
                     &cell.findings);
          }
        }

        // Reservation overhang: reservations against *declared* allocatable
        // capacity. This is a one-sided constraint -- reserving less than the
        // declared headroom is normal -- so only the exceeding direction is a
        // discrepancy.
        if (cell.reservation_overhang.is_known() && cell.reservation_overhang.value() > 0) {
          const Quantity& reference = cell.views.views[view_index(CapacityView::Reserved)];
          auto bound = policy_tolerance_bound(
              dimension_tolerance, reference.is_known() ? reference.value() : 0);
          if (!bound.ok()) {
            return bound.error();
          }
          const FindingContext context{dimension_tolerance, bound.value()};
          consider(DiscrepancyClass::ReservationExceedsAllocatable,
                   ReasonTarget::ReservationOverhang, cell.reservation_overhang, context,
                   "reservations exceed declared allocatable capacity", &cell.findings);
        }

        // Allocatable skew, whose subtrahend is a derived quantity.
        if (cell.allocatable_skew.is_known() && cell.allocatable_skew.value() != 0) {
          const Quantity& reference = cell.views.views[view_index(CapacityView::Allocatable)];
          auto bound = policy_tolerance_bound(
              dimension_tolerance, reference.is_known() ? reference.value() : 0);
          if (!bound.ok()) {
            return bound.error();
          }
          const FindingContext context{dimension_tolerance, bound.value()};
          if (cell.allocatable_skew.value() > 0) {
            consider(
                DiscrepancyClass::AllocatableOverstated, ReasonTarget::AllocatableSkew,
                cell.allocatable_skew, context,
                "declared allocatable capacity exceeds usable capacity minus reservations",
                &cell.findings);
          } else {
            consider(
                DiscrepancyClass::AllocatableUnderstated, ReasonTarget::AllocatableSkew,
                cell.allocatable_skew, context,
                "declared allocatable capacity is below usable capacity minus reservations",
                &cell.findings);
          }
        }

        // Unexplained residuals. These are reported, never balanced away -- but
        // only when the residual is anomalous in the direction that matters for
        // that constraint. Headroom held back and reservations below nameplate
        // are normal operating states, not unexplained discrepancies.
        for (const ResidualExplanation& explanation : cell.explanation.residuals()) {
          if (!explanation.unexplained.is_known() || explanation.unexplained.value() == 0) {
            continue;
          }
          if (!explanation.anomalous) {
            continue;
          }
          auto bound = policy_tolerance_bound(
              dimension_tolerance,
              explanation.raw.is_known() ? explanation.raw.value() : 0);
          if (!bound.ok()) {
            return bound.error();
          }
          const Amount absolute = explanation.unexplained.value() < 0
                                      ? -explanation.unexplained.value()
                                      : explanation.unexplained.value();
          if (absolute <= bound.value()) {
            continue;
          }
          Finding finding;
          finding.classification = DiscrepancyClass::UnexplainedResidual;
          finding.target = explanation.target;
          finding.magnitude = explanation.unexplained.value();
          finding.tolerance = bound.value();
          finding.exceeds_tolerance = true;
          finding.detail = "no attribution accounts for this part of the residual";
          cell.findings.push_back(std::move(finding));
        }
      }

      if (cell.findings.empty()) {
        push_cell_finding(DiscrepancyClass::Agrees,
                          "every present view agrees within the configured tolerance");
      }

      std::stable_sort(cell.findings.begin(), cell.findings.end(), finding_less);
      cells.push_back(std::move(cell));
    }
  }

  std::stable_sort(cells.begin(), cells.end(), cell_less);

  local_stats.conflicts = 0;
  local_stats.unexplained_cells = 0;
  for (const ReconciliationCell& cell : cells) {
    local_stats.conflicts += cell.conflicts.size();
    if (cell.explanation.has_unexplained()) {
      ++local_stats.unexplained_cells;
    }
  }

  auto run = ReconciliationRun::create(input.run_id, request, input.evidence->digest(),
                                       input.attributions->digest(), std::move(cells));
  if (!run.ok()) {
    return run.error();
  }
  auto validated = run.value().validate(input.limits->max_cells_per_run);
  if (!validated.ok()) {
    return validated.error();
  }
  if (stats != nullptr) {
    *stats = local_stats;
  }
  return run;
}

}  // namespace summon::capacity_reconciliation
