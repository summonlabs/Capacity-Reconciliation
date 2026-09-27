// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Explanation graph.
//
// For every cell the runtime records *how* a residual was arrived at, not just
// what it is. The graph is a small ordered DAG:
//
//   * input nodes reference the exact evidence or attribution items consumed,
//     by identity and content digest;
//   * derived nodes are the checked arithmetic steps, each naming its operands,
//     its operator and its exact result;
//   * for each residual there is exactly one *unexplained* node holding
//     whatever no attribution accounted for.
//
// The unexplained node is the point of the exercise: a residual is never
// adjusted, clamped or redistributed to make a total balance. If 40 kW of a
// 100 kW gap has an attribution and 60 kW does not, the graph says 40 explained
// and 60 unexplained, forever, and the cell is PartiallyExplained.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "summon/capacity_reconciliation/canonical.hpp"
#include "summon/capacity_reconciliation/classify.hpp"
#include "summon/capacity_reconciliation/evidence.hpp"
#include "summon/capacity_reconciliation/hash.hpp"
#include "summon/capacity_reconciliation/identity.hpp"
#include "summon/capacity_reconciliation/quantity.hpp"
#include "summon/capacity_reconciliation/status.hpp"
#include "summon/capacity_reconciliation/value.hpp"

namespace summon::capacity_reconciliation {

/// The role a graph node plays.
enum class NodeRole : std::uint8_t {
  /// An evidence item consumed as an input.
  EvidenceInput = 0,
  /// An attribution item consumed as an input.
  AttributionInput = 1,
  /// A rolled-up quantity over descendant scopes.
  Rollup = 2,
  /// A subtraction: left - right (or a negation when there is one operand).
  Difference = 3,
  /// An addition of operands.
  Sum = 4,
  /// The residual produced by subtracting attributions from a raw residual.
  ExplainedResidual = 5,
  /// The part of a residual that no attribution accounted for.
  UnexplainedResidual = 6,
  /// A view whose value could not be established.
  UnknownInput = 7,
};

[[nodiscard]] const char* to_string(NodeRole role) noexcept;
[[nodiscard]] std::optional<NodeRole> node_role_from_string(std::string_view token) noexcept;

/// One node of an explanation graph. Nodes are immutable value objects.
struct ExplanationNode {
  NodeId id;
  NodeRole role = NodeRole::EvidenceInput;
  /// For input nodes: the identity of the consumed item.
  std::optional<EvidenceId> evidence_id;
  std::optional<AttributionId> attribution_id;
  /// For derived nodes: indices of operand nodes within the same graph.
  std::vector<std::uint32_t> operands;
  /// The exact value this node carries, when known.
  Quantity value;
  /// For unknown nodes: why.
  UnknownReason reason = UnknownReason::NotReported;
  /// Short stable label, e.g. "installed", "observed", "attributed:chiller-2".
  std::string label;

  friend bool operator==(const ExplanationNode& a, const ExplanationNode& b) noexcept {
    return a.id == b.id && a.role == b.role && a.operands == b.operands &&
           a.value == b.value && a.label == b.label && a.reason == b.reason &&
           a.evidence_id == b.evidence_id && a.attribution_id == b.attribution_id;
  }
};

/// The complete explanation of one residual.
struct ResidualExplanation {
  ReasonTarget target = ReasonTarget::None;
  /// The raw residual, before attributions.
  Quantity raw;
  /// Sum of the attributions that were applied, when known.
  Quantity attributed;
  /// Raw minus attributed. Always present: if nothing was attributed this
  /// equals raw, and if raw is unknown the unexplained part is unknown for the
  /// same reason. It is never forced to zero.
  Quantity unexplained;
  /// Index into the graph of the node holding `unexplained`.
  std::uint32_t unexplained_node = 0;
  /// True when an unexplained part in this direction is a real anomaly for this
  /// constraint. Reservations below nameplate and headroom held back are normal
  /// operating states, so their residuals are still computed and still recorded
  /// -- but they are not unexplained discrepancies, and counting them as such
  /// would make every healthy cell look partially explained.
  bool anomalous = false;

  friend bool operator==(const ResidualExplanation& a,
                         const ResidualExplanation& b) noexcept {
    return a.target == b.target && a.raw == b.raw &&
           a.attributed == b.attributed && a.unexplained == b.unexplained &&
           a.unexplained_node == b.unexplained_node && a.anomalous == b.anomalous;
  }
};

/// An explanation graph for one cell.
class ExplanationGraph {
 public:
  ExplanationGraph() = default;

  /// Append a node, returning its index. Bounded by `max_nodes`.
  [[nodiscard]] Result<std::uint32_t> add(ExplanationNode node, std::size_t max_nodes);

  [[nodiscard]] const std::vector<ExplanationNode>& nodes() const noexcept { return nodes_; }
  [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }
  [[nodiscard]] bool empty() const noexcept { return nodes_.empty(); }

  [[nodiscard]] const ExplanationNode* at(std::uint32_t index) const noexcept;

  [[nodiscard]] const std::vector<ResidualExplanation>& residuals() const noexcept {
    return residuals_;
  }
  [[nodiscard]] Status add_residual(ResidualExplanation residual, std::size_t max_residuals);

  /// Find the residual explanation for a target, if present.
  [[nodiscard]] const ResidualExplanation* find_residual(ReasonTarget target) const noexcept;

  /// True when at least one residual has a known, non-zero unexplained part in
  /// the direction that is anomalous for that constraint.
  [[nodiscard]] bool has_unexplained() const noexcept;

  /// Canonical encoding of the graph; used for the run digest.
  void encode(CanonicalWriter& writer) const;

 private:
  std::vector<ExplanationNode> nodes_;
  std::vector<ResidualExplanation> residuals_;
};

/// Decode an explanation graph from a canonical stream. Forward operand
/// references, unrecognised roles and out-of-range counts are refused, so a
/// decoded graph is always topologically ordered and index-safe.
[[nodiscard]] Result<ExplanationGraph> decode_explanation(CanonicalReader& reader,
                                                          std::size_t max_nodes,
                                                          std::size_t max_residuals);

}  // namespace summon::capacity_reconciliation
