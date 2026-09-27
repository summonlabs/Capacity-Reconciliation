// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/capacity_reconciliation/explain.hpp"

#include <array>

namespace summon::capacity_reconciliation {
namespace {

struct RoleToken {
  NodeRole role;
  const char* token;
};

constexpr std::array<RoleToken, 8> kRoleTokens{{
    {NodeRole::EvidenceInput, "evidence_input"},
    {NodeRole::AttributionInput, "attribution_input"},
    {NodeRole::Rollup, "rollup"},
    {NodeRole::Difference, "difference"},
    {NodeRole::Sum, "sum"},
    {NodeRole::ExplainedResidual, "explained_residual"},
    {NodeRole::UnexplainedResidual, "unexplained_residual"},
    {NodeRole::UnknownInput, "unknown_input"},
}};

enum Tag : std::uint8_t {
  kTagNodeCount = 1,
  kTagNode = 2,
  kTagNodeId = 3,
  kTagRole = 4,
  kTagEvidenceId = 5,
  kTagAttributionId = 6,
  kTagOperandCount = 7,
  kTagOperand = 8,
  kTagKnown = 9,
  kTagValue = 10,
  kTagReason = 11,
  kTagLabel = 12,
  kTagResidualCount = 13,
  kTagResidual = 14,
  kTagResidualTarget = 15,
  kTagRawPresent = 16,
  kTagRaw = 17,
  kTagAttributedPresent = 18,
  kTagAttributed = 19,
  kTagUnexplainedPresent = 20,
  kTagUnexplained = 21,
  kTagUnexplainedNode = 22,
  kTagAnomalous = 23,
  kTagEnd = 0x7F,
};

void write_optional_quantity(CanonicalWriter& writer, std::uint8_t presence_tag,
                             std::uint8_t value_tag, const Quantity& quantity) {
  writer.field(presence_tag);
  writer.boolean(quantity.is_known());
  if (quantity.is_known()) {
    writer.field(value_tag);
    writer.i64(quantity.value());
  }
}

Result<Quantity> read_optional_quantity(CanonicalReader& reader, std::uint8_t presence_tag,
                                        std::uint8_t value_tag, const char* what) {
  auto status = reader.field(presence_tag, what);
  if (!status.ok()) {
    return status.error();
  }
  auto known = reader.boolean();
  if (!known.ok()) {
    return known.error();
  }
  if (!known.value()) {
    return Quantity::unknown(UnknownReason::NotReported);
  }
  status = reader.field(value_tag, what);
  if (!status.ok()) {
    return status.error();
  }
  auto value = reader.i64();
  if (!value.ok()) {
    return value.error();
  }
  return Quantity::known(value.value());
}

}  // namespace

const char* to_string(NodeRole role) noexcept {
  for (const RoleToken& entry : kRoleTokens) {
    if (entry.role == role) {
      return entry.token;
    }
  }
  return "unrecognised";
}

std::optional<NodeRole> node_role_from_string(std::string_view token) noexcept {
  for (const RoleToken& entry : kRoleTokens) {
    if (token == entry.token) {
      return entry.role;
    }
  }
  return std::nullopt;
}

Result<std::uint32_t> ExplanationGraph::add(ExplanationNode node, std::size_t max_nodes) {
  if (nodes_.size() >= max_nodes) {
    return make_error(ErrorCode::LimitExceeded,
                      "explanation graph exceeds the configured limit of " +
                          std::to_string(max_nodes) + " nodes",
                      "explanation_nodes");
  }
  if (!node.id.valid()) {
    return make_error(ErrorCode::InvariantViolation,
                      "explanation node was constructed without an identity", "node_id");
  }
  for (std::uint32_t operand : node.operands) {
    if (static_cast<std::size_t>(operand) >= nodes_.size()) {
      return make_error(ErrorCode::InvariantViolation,
                        "explanation node refers to operand " + std::to_string(operand) +
                            " which is not yet present in the graph",
                        "operand_index");
    }
  }
  if (nodes_.size() >= static_cast<std::size_t>(amount_max())) {
    return make_error(ErrorCode::LimitExceeded, "explanation graph index space exhausted",
                      "explanation_nodes");
  }
  const auto index = static_cast<std::uint32_t>(nodes_.size());
  nodes_.push_back(std::move(node));
  return index;
}

const ExplanationNode* ExplanationGraph::at(std::uint32_t index) const noexcept {
  if (static_cast<std::size_t>(index) >= nodes_.size()) {
    return nullptr;
  }
  return &nodes_[index];
}

Status ExplanationGraph::add_residual(ResidualExplanation explanation, std::size_t max_residuals) {
  if (residuals_.size() >= max_residuals) {
    return make_error(ErrorCode::LimitExceeded,
                      "explanation graph exceeds the configured limit of " +
                          std::to_string(max_residuals) + " residual explanations",
                      "explanation_residuals");
  }
  if (explanation.unexplained_node != 0 &&
      static_cast<std::size_t>(explanation.unexplained_node) >= nodes_.size()) {
    return make_error(ErrorCode::InvariantViolation,
                      "residual explanation refers to an absent unexplained node",
                      "unexplained_node");
  }
  for (ResidualExplanation& existing : residuals_) {
    if (existing.target == explanation.target) {
      existing = std::move(explanation);
      return Status::success();
    }
  }
  residuals_.push_back(std::move(explanation));
  return Status::success();
}

const ResidualExplanation* ExplanationGraph::find_residual(ReasonTarget target) const noexcept {
  for (const ResidualExplanation& explanation : residuals_) {
    if (explanation.target == target) {
      return &explanation;
    }
  }
  return nullptr;
}

bool ExplanationGraph::has_unexplained() const noexcept {
  for (const ResidualExplanation& explanation : residuals_) {
    if (explanation.anomalous && explanation.unexplained.is_known() &&
        explanation.unexplained.value() != 0) {
      return true;
    }
  }
  return false;
}

void ExplanationGraph::encode(CanonicalWriter& writer) const {
  writer.field(kTagNodeCount);
  writer.u64(static_cast<std::uint64_t>(nodes_.size()));
  for (const ExplanationNode& node : nodes_) {
    writer.field(kTagNode);
    writer.field(kTagNodeId);
    writer.bytes(node.id.to_string());
    writer.field(kTagRole);
    writer.token(to_string(node.role));
    writer.field(kTagEvidenceId);
    writer.boolean(node.evidence_id.has_value());
    if (node.evidence_id.has_value()) {
      writer.bytes(node.evidence_id->to_string());
    }
    writer.field(kTagAttributionId);
    writer.boolean(node.attribution_id.has_value());
    if (node.attribution_id.has_value()) {
      writer.bytes(node.attribution_id->to_string());
    }
    writer.field(kTagOperandCount);
    writer.u64(static_cast<std::uint64_t>(node.operands.size()));
    for (std::uint32_t operand : node.operands) {
      writer.field(kTagOperand);
      writer.u32(operand);
    }
    writer.field(kTagKnown);
    writer.boolean(node.value.is_known());
    if (node.value.is_known()) {
      writer.field(kTagValue);
      writer.i64(node.value.value());
    } else {
      writer.field(kTagReason);
      writer.token(to_string(node.reason));
    }
    writer.field(kTagLabel);
    writer.token(node.label.empty() ? std::string_view("node") : std::string_view(node.label));
  }
  writer.field(kTagResidualCount);
  writer.u64(static_cast<std::uint64_t>(residuals_.size()));
  for (const ResidualExplanation& explanation : residuals_) {
    writer.field(kTagResidual);
    writer.field(kTagResidualTarget);
    writer.token(to_string(explanation.target));
    write_optional_quantity(writer, kTagRawPresent, kTagRaw, explanation.raw);
    write_optional_quantity(writer, kTagAttributedPresent, kTagAttributed, explanation.attributed);
    write_optional_quantity(writer, kTagUnexplainedPresent, kTagUnexplained,
                            explanation.unexplained);
    writer.field(kTagUnexplainedNode);
    writer.u32(explanation.unexplained_node);
    writer.field(kTagAnomalous);
    writer.boolean(explanation.anomalous);
  }
  writer.field(kTagEnd);
}

Result<ExplanationGraph> decode_explanation(CanonicalReader& reader, std::size_t max_nodes,
                                            std::size_t max_residuals) {
  ExplanationGraph graph;

  auto status = reader.field(kTagNodeCount, "explanation node count");
  if (!status.ok()) {
    return status.error();
  }
  auto node_count = reader.u64();
  if (!node_count.ok()) {
    return node_count.error();
  }
  if (node_count.value() > static_cast<std::uint64_t>(max_nodes)) {
    return make_error(ErrorCode::LimitExceeded,
                      "stored explanation declares " + std::to_string(node_count.value()) +
                          " nodes, above the limit of " + std::to_string(max_nodes),
                      "explanation_nodes");
  }

  for (std::uint64_t i = 0; i < node_count.value(); ++i) {
    status = reader.field(kTagNode, "explanation node");
    if (!status.ok()) {
      return status.error();
    }
    status = reader.field(kTagNodeId, "node id");
    if (!status.ok()) {
      return status.error();
    }
    auto id_text = reader.bytes(64, "node id");
    if (!id_text.ok()) {
      return id_text.error();
    }
    auto id = NodeId::parse(id_text.value());
    if (!id.has_value()) {
      return make_error(ErrorCode::Malformed, "stored node identity is malformed", "node_id");
    }

    ExplanationNode node;
    node.id = *id;

    status = reader.field(kTagRole, "node role");
    if (!status.ok()) {
      return status.error();
    }
    auto role_text = reader.token(32, "node role");
    if (!role_text.ok()) {
      return role_text.error();
    }
    auto role = node_role_from_string(role_text.value());
    if (!role.has_value()) {
      return make_error(ErrorCode::Malformed, "stored node role is not recognised", "node_role");
    }
    node.role = *role;

    status = reader.field(kTagEvidenceId, "evidence id presence");
    if (!status.ok()) {
      return status.error();
    }
    auto has_evidence = reader.boolean();
    if (!has_evidence.ok()) {
      return has_evidence.error();
    }
    if (has_evidence.value()) {
      auto text = reader.bytes(64, "evidence id");
      if (!text.ok()) {
        return text.error();
      }
      auto parsed = EvidenceId::parse(text.value());
      if (!parsed.has_value()) {
        return make_error(ErrorCode::Malformed, "stored evidence identity is malformed",
                          "evidence_id");
      }
      node.evidence_id = *parsed;
    }

    status = reader.field(kTagAttributionId, "attribution id presence");
    if (!status.ok()) {
      return status.error();
    }
    auto has_attribution = reader.boolean();
    if (!has_attribution.ok()) {
      return has_attribution.error();
    }
    if (has_attribution.value()) {
      auto text = reader.bytes(64, "attribution id");
      if (!text.ok()) {
        return text.error();
      }
      auto parsed = AttributionId::parse(text.value());
      if (!parsed.has_value()) {
        return make_error(ErrorCode::Malformed, "stored attribution identity is malformed",
                          "attribution_id");
      }
      node.attribution_id = *parsed;
    }

    status = reader.field(kTagOperandCount, "operand count");
    if (!status.ok()) {
      return status.error();
    }
    auto operand_count = reader.u64();
    if (!operand_count.ok()) {
      return operand_count.error();
    }
    if (operand_count.value() > static_cast<std::uint64_t>(max_nodes)) {
      return make_error(ErrorCode::LimitExceeded, "stored operand count is out of range",
                        "operand_count");
    }
    for (std::uint64_t op = 0; op < operand_count.value(); ++op) {
      status = reader.field(kTagOperand, "operand");
      if (!status.ok()) {
        return status.error();
      }
      auto operand = reader.u32();
      if (!operand.ok()) {
        return operand.error();
      }
      if (static_cast<std::size_t>(operand.value()) >= i) {
        return make_error(ErrorCode::Malformed,
                          "stored explanation refers to a forward operand; the graph must be "
                          "topologically ordered",
                          "operand_index");
      }
      node.operands.push_back(operand.value());
    }

    status = reader.field(kTagKnown, "node value presence");
    if (!status.ok()) {
      return status.error();
    }
    auto known = reader.boolean();
    if (!known.ok()) {
      return known.error();
    }
    if (known.value()) {
      status = reader.field(kTagValue, "node value");
      if (!status.ok()) {
        return status.error();
      }
      auto value = reader.i64();
      if (!value.ok()) {
        return value.error();
      }
      node.value = Quantity::known(value.value());
    } else {
      status = reader.field(kTagReason, "node unknown reason");
      if (!status.ok()) {
        return status.error();
      }
      auto reason_text = reader.token(48, "node reason");
      if (!reason_text.ok()) {
        return reason_text.error();
      }
      auto reason = unknown_reason_from_string(reason_text.value());
      if (!reason.has_value()) {
        return make_error(ErrorCode::Malformed, "stored node reason is not recognised",
                          "unknown_reason");
      }
      node.reason = *reason;
      node.value = Quantity::unknown(*reason);
    }

    status = reader.field(kTagLabel, "node label");
    if (!status.ok()) {
      return status.error();
    }
    auto label = reader.token(64, "node label");
    if (!label.ok()) {
      return label.error();
    }
    node.label = label.value();

    auto added = graph.add(std::move(node), max_nodes);
    if (!added.ok()) {
      return added.error();
    }
  }

  status = reader.field(kTagResidualCount, "residual count");
  if (!status.ok()) {
    return status.error();
  }
  auto residual_count = reader.u64();
  if (!residual_count.ok()) {
    return residual_count.error();
  }
  if (residual_count.value() > static_cast<std::uint64_t>(max_residuals)) {
    return make_error(ErrorCode::LimitExceeded, "stored residual count is out of range",
                      "residual_count");
  }

  for (std::uint64_t i = 0; i < residual_count.value(); ++i) {
    status = reader.field(kTagResidual, "residual explanation");
    if (!status.ok()) {
      return status.error();
    }
    status = reader.field(kTagResidualTarget, "residual target");
    if (!status.ok()) {
      return status.error();
    }
    auto target_text = reader.token(48, "residual target");
    if (!target_text.ok()) {
      return target_text.error();
    }
    ResidualExplanation explanation;
    bool matched = false;
    for (std::uint8_t candidate = 1; candidate <= 8 && !matched; ++candidate) {
      const auto target = static_cast<ReasonTarget>(candidate);
      if (target_text.value() == to_string(target)) {
        explanation.target = target;
        matched = true;
      }
    }
    if (!matched) {
      return make_error(ErrorCode::Malformed, "stored residual target is not recognised",
                        "reason_target");
    }

    auto raw = read_optional_quantity(reader, kTagRawPresent, kTagRaw, "residual raw");
    if (!raw.ok()) {
      return raw.error();
    }
    explanation.raw = raw.value();

    auto attributed =
        read_optional_quantity(reader, kTagAttributedPresent, kTagAttributed, "residual attributed");
    if (!attributed.ok()) {
      return attributed.error();
    }
    explanation.attributed = attributed.value();

    auto unexplained = read_optional_quantity(reader, kTagUnexplainedPresent, kTagUnexplained,
                                              "residual unexplained");
    if (!unexplained.ok()) {
      return unexplained.error();
    }
    explanation.unexplained = unexplained.value();

    status = reader.field(kTagUnexplainedNode, "unexplained node index");
    if (!status.ok()) {
      return status.error();
    }
    auto node = reader.u32();
    if (!node.ok()) {
      return node.error();
    }
    explanation.unexplained_node = node.value();

    status = reader.field(kTagAnomalous, "residual anomaly flag");
    if (!status.ok()) {
      return status.error();
    }
    auto anomalous = reader.boolean();
    if (!anomalous.ok()) {
      return anomalous.error();
    }
    explanation.anomalous = anomalous.value();

    auto added = graph.add_residual(std::move(explanation), max_residuals);
    if (!added.ok()) {
      return added.error();
    }
  }

  status = reader.field(kTagEnd, "explanation end marker");
  if (!status.ok()) {
    return status.error();
  }
  return graph;
}

}  // namespace summon::capacity_reconciliation
