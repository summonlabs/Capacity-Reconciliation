// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/capacity_reconciliation/report.hpp"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "summon/capacity_reconciliation/version.hpp"

namespace summon::capacity_reconciliation {
namespace {

/// Every machine-readable document this runtime emits identifies the schema it
/// conforms to. A consumer must never have to guess which runtime and which
/// schema version produced a document it was handed.
constexpr std::string_view kJsonSchema = "capacity-reconciliation/v1";

void append_escaped(std::string* out, std::string_view text) {
  static const char* kHex = "0123456789abcdef";
  out->push_back('"');
  for (char raw : text) {
    const auto c = static_cast<unsigned char>(raw);
    switch (c) {
      case '"':
        out->append("\\\"");
        continue;
      case '\\':
        out->append("\\\\");
        continue;
      case '\n':
        out->append("\\n");
        continue;
      case '\r':
        out->append("\\r");
        continue;
      case '\t':
        out->append("\\t");
        continue;
      case '\b':
        out->append("\\b");
        continue;
      case '\f':
        out->append("\\f");
        continue;
      default:
        break;
    }
    if (c < 0x20 || c >= 0x7F) {
      out->append("\\u00");
      out->push_back(kHex[(c >> 4) & 0xFu]);
      out->push_back(kHex[c & 0xFu]);
      continue;
    }
    out->push_back(static_cast<char>(c));
  }
  out->push_back('"');
}

}  // namespace

std::string json_escape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 2);
  append_escaped(&out, text);
  return out;
}

void JsonWriter::separate() {
  if (!first_.empty()) {
    if (!first_.back()) {
      out_.push_back(',');
    }
    first_.back() = false;
  }
}

void JsonWriter::value_prefix() {
  // A value that directly follows a key needs no separator of its own: the key
  // already emitted it. Any other value is an array element and does.
  if (after_key_) {
    after_key_ = false;
    return;
  }
  separate();
}

void JsonWriter::indent() {
  out_.push_back('\n');
  for (std::size_t i = 0; i < depth_; ++i) {
    out_.append("  ");
  }
}

void JsonWriter::begin_object() {
  value_prefix();
  out_.push_back('{');
  first_.push_back(true);
  ++depth_;
}

void JsonWriter::end_object() {
  --depth_;
  if (!first_.empty() && !first_.back()) {
    indent();
  }
  out_.push_back('}');
  if (!first_.empty()) {
    first_.pop_back();
  }
  after_key_ = false;
}

void JsonWriter::begin_array() {
  value_prefix();
  out_.push_back('[');
  first_.push_back(true);
  ++depth_;
}

void JsonWriter::end_array() {
  --depth_;
  if (!first_.empty() && !first_.back()) {
    indent();
  }
  out_.push_back(']');
  if (!first_.empty()) {
    first_.pop_back();
  }
  after_key_ = false;
}

void JsonWriter::key(std::string_view name) {
  separate();
  indent();
  append_escaped(&out_, name);
  out_.push_back(':');
  out_.push_back(' ');
  after_key_ = true;
}

void JsonWriter::string_value(std::string_view value) {
  value_prefix();
  append_escaped(&out_, value);
}

void JsonWriter::string_value(const char* value) {
  value_prefix();
  append_escaped(&out_, value == nullptr ? std::string_view("") : std::string_view(value));
}

void JsonWriter::number_value(std::int64_t value) {
  value_prefix();
  out_.append(std::to_string(value));
}

void JsonWriter::unumber_value(std::uint64_t value) {
  value_prefix();
  out_.append(std::to_string(value));
}

void JsonWriter::bool_value(bool value) {
  value_prefix();
  out_.append(value ? "true" : "false");
}

void JsonWriter::null_value() {
  value_prefix();
  out_.append("null");
}

void JsonWriter::raw_value(std::string_view json) {
  value_prefix();
  out_.append(json);
}

void JsonWriter::member(std::string_view name, std::string_view value) {
  (void)after_key_;
  key(name);
  string_value(value);
}

void JsonWriter::member(std::string_view name, const char* value) {
  (void)after_key_;
  key(name);
  string_value(value);
}

void JsonWriter::member(std::string_view name, std::int64_t value) {
  key(name);
  number_value(value);
}

void JsonWriter::member(std::string_view name, std::uint64_t value) {
  key(name);
  unumber_value(value);
}

void JsonWriter::member(std::string_view name, bool value) {
  key(name);
  bool_value(value);
}

void JsonWriter::member_null(std::string_view name) {
  key(name);
  null_value();
}

std::string quantity_json(const Quantity& quantity) {
  if (quantity.is_known()) {
    return std::to_string(quantity.value());
  }
  std::string out = "{\"unknown\": \"";  // built by JsonWriter in callers; kept minimal here
  out += to_string(quantity.reason());
  out += "\"}";
  return out;
}

namespace {

void emit_quantity(JsonWriter& writer, const Quantity& quantity) {
  writer.begin_object();
  writer.member("known", quantity.is_known());
  if (quantity.is_known()) {
    writer.member("value", quantity.value());
  } else {
    writer.member("reason", to_string(quantity.reason()));
  }
  writer.end_object();
}

std::string cell_primary_token(const ReconciliationCell& cell) {
  return to_string(cell.primary_class());
}

}  // namespace

std::string render_explanation_json(const ExplanationGraph& graph) {
  JsonWriter writer;
  writer.begin_object();
  writer.member("schema", kJsonSchema);
  writer.key("nodes");
  writer.begin_array();
  for (const ExplanationNode& node : graph.nodes()) {
    writer.begin_object();
    writer.member("id", node.id.to_string());
    writer.member("role", to_string(node.role));
    if (node.evidence_id.has_value()) {
      writer.member("evidence", node.evidence_id->to_string());
    }
    if (node.attribution_id.has_value()) {
      writer.member("attribution", node.attribution_id->to_string());
    }
    writer.key("operands");
    writer.begin_array();
    for (std::uint32_t operand : node.operands) {
      writer.unumber_value(operand);
    }
    writer.end_array();
    writer.key("value");
    emit_quantity(writer, node.value);
    writer.member("label", node.label);
    writer.end_object();
  }
  writer.end_array();
  writer.key("residuals");
  writer.begin_array();
  for (const ResidualExplanation& explanation : graph.residuals()) {
    writer.begin_object();
    writer.member("target", to_string(explanation.target));
    writer.key("raw");
    emit_quantity(writer, explanation.raw);
    writer.key("attributed");
    emit_quantity(writer, explanation.attributed);
    writer.key("unexplained");
    emit_quantity(writer, explanation.unexplained);
    writer.member("unexplained_node", static_cast<std::uint64_t>(explanation.unexplained_node));
    writer.end_object();
  }
  writer.end_array();
  writer.end_object();
  return std::move(writer).take();
}

std::string render_conflict_json(const EvidenceConflict& conflict) {
  JsonWriter writer;
  writer.begin_object();
  writer.member("schema", kJsonSchema);
  writer.member("id", conflict.id.to_string());
  writer.member("scope", conflict.scope.to_string());
  writer.member("dimension", conflict.dimension.to_string());
  writer.member("view", to_string(conflict.view));
  writer.member("reason", conflict.unresolvable_reason);
  writer.member("resolved", conflict.selected_value.has_value());
  if (conflict.selected_value.has_value()) {
    writer.member("selected_value", *conflict.selected_value);
  }
  writer.key("participants");
  writer.begin_array();
  for (std::size_t i = 0; i < conflict.participants.size(); ++i) {
    writer.begin_object();
    writer.member("evidence", conflict.participants[i].to_string());
    if (i < conflict.participant_digests.size()) {
      writer.member("digest", conflict.participant_digests[i].to_hex());
    }
    writer.end_object();
  }
  writer.end_array();
  writer.key("asserted");
  writer.begin_array();
  for (std::size_t i = 0; i < conflict.values.size(); ++i) {
    writer.begin_object();
    writer.member("value", conflict.values[i]);
    if (i < conflict.asserting_sources.size()) {
      writer.member("source", to_string(conflict.asserting_sources[i]));
    }
    writer.end_object();
  }
  writer.end_array();
  writer.end_object();
  return std::move(writer).take();
}

std::string render_cell_json(const ReconciliationCell& cell) {
  JsonWriter writer;
  writer.begin_object();
  writer.member("schema", kJsonSchema);
  writer.member("scope", cell.scope.to_string());
  writer.member("dimension", cell.dimension.to_string());
  writer.member("unit", to_string(cell.dimension.unit()));
  writer.member("primary_class", cell_primary_token(cell));

  writer.key("views");
  writer.begin_object();
  for (std::size_t i = 0; i < kCapacityViewCount; ++i) {
    const auto view = static_cast<CapacityView>(i);
    writer.key(to_string(view));
    writer.begin_object();
    writer.member("present", cell.views.selected[i].has_value());
    if (cell.views.selected[i].has_value()) {
      writer.member("selected", cell.views.selected[i]->to_string());
      writer.member("source", to_string(cell.views.selected_source[i]));
    }
    writer.member("candidates", static_cast<std::uint64_t>(cell.views.candidate_count[i]));
    writer.member("partial", cell.views.partial[i]);
    writer.key("value");
    emit_quantity(writer, cell.views.views[i]);
    writer.end_object();
  }
  writer.end_object();

  writer.key("residuals");
  writer.begin_object();
  writer.key("planned_gap");
  emit_quantity(writer, cell.planned_gap);
  writer.key("observed_gap");
  emit_quantity(writer, cell.observed_gap);
  writer.key("derate_gap");
  emit_quantity(writer, cell.derate_gap);
  writer.key("headroom_gap");
  emit_quantity(writer, cell.headroom_gap);
  writer.key("reservation_pressure");
  emit_quantity(writer, cell.reservation_pressure);
  writer.key("reservation_overhang");
  emit_quantity(writer, cell.reservation_overhang);
  writer.key("derived_allocatable");
  emit_quantity(writer, cell.derived_allocatable);
  writer.key("allocatable_skew");
  emit_quantity(writer, cell.allocatable_skew);
  writer.end_object();

  writer.key("findings");
  writer.begin_array();
  for (const Finding& finding : cell.findings) {
    writer.begin_object();
    writer.member("class", to_string(finding.classification));
    writer.member("target", to_string(finding.target));
    if (finding.magnitude.has_value()) {
      writer.member("magnitude", *finding.magnitude);
    } else {
      writer.member_null("magnitude");
    }
    writer.member("tolerance", finding.tolerance);
    writer.member("exceeds_tolerance", finding.exceeds_tolerance);
    writer.member("detail", finding.detail);
    writer.end_object();
  }
  writer.end_array();

  writer.key("conflicts");
  writer.begin_array();
  for (const EvidenceConflict& conflict : cell.conflicts) {
    writer.raw_value(render_conflict_json(conflict));
  }
  writer.end_array();

  writer.key("sibling_units");
  writer.begin_array();
  for (Unit unit : cell.sibling_units) {
    writer.string_value(to_string(unit));
  }
  writer.end_array();

  writer.member("evidence_seen", cell.evidence_seen);
  writer.member("attributions_applied", cell.attributions_applied);
  writer.member("rolled_up", cell.rolled_up);
  writer.member("rollup_contributors", static_cast<std::uint64_t>(cell.rollup_contributors));
  writer.member("rollup_missing", static_cast<std::uint64_t>(cell.rollup_missing));
  writer.key("explanation");
  writer.raw_value(render_explanation_json(cell.explanation));
  writer.end_object();
  return std::move(writer).take();
}

std::string render_run_json(const ReconciliationRun& run, bool include_cells) {
  JsonWriter writer;
  writer.begin_object();
  writer.member("schema", kJsonSchema);
  writer.member("run", run.id().to_string());
  writer.member("generation", run.generation().value());
  writer.member("evaluation_instant", run.evaluation_instant().value());
  writer.member("clock_domain", run.request().clock_domain.name());
  writer.member("resolution", to_string(run.resolution()));
  writer.member("run_digest", run.run_digest().to_hex());
  writer.member("evidence_digest", run.evidence_digest().to_hex());
  writer.member("attribution_digest", run.attribution_digest().to_hex());
  writer.member("policy_digest", run.policy_digest().to_hex());
  writer.member("attempt", run.commit_attempt().value());
  if (run.committed_generation().has_value()) {
    writer.member("committed_generation", run.committed_generation()->value());
  } else {
    writer.member_null("committed_generation");
  }
  if (run.request().parent_run.has_value()) {
    writer.member("parent_run", run.request().parent_run->to_string());
  } else {
    writer.member_null("parent_run");
  }
  if (run.superseded_by().has_value()) {
    writer.member("superseded_by", run.superseded_by()->to_string());
  } else {
    writer.member_null("superseded_by");
  }
  writer.member("from_recovered_state", run.request().from_recovered_state);
  writer.member("label",
                run.request().label.empty() ? std::string_view("run")
                                            : std::string_view(run.request().label));

  writer.key("scopes");
  writer.begin_array();
  for (const ScopeSelection& selection : run.scopes()) {
    writer.begin_object();
    writer.member("scope", selection.scope.to_string());
    writer.member("mode", to_string(selection.mode));
    writer.end_object();
  }
  writer.end_array();

  writer.key("summary");
  writer.begin_object();
  writer.member("cells", static_cast<std::uint64_t>(run.cells().size()));
  writer.member("conflicts", static_cast<std::uint64_t>(run.conflict_count()));
  writer.member("unexplained_cells", static_cast<std::uint64_t>(run.unexplained_cell_count()));
  writer.member("agrees", static_cast<std::uint64_t>(run.count_class(DiscrepancyClass::Agrees)));
  writer.end_object();

  if (include_cells) {
    writer.key("cells");
    writer.begin_array();
    for (const ReconciliationCell& cell : run.cells()) {
      writer.raw_value(render_cell_json(cell));
    }
    writer.end_array();
  }
  writer.end_object();
  return std::move(writer).take();
}

std::string render_evidence_json(const EvidenceItem& item) {
  JsonWriter writer;
  writer.begin_object();
  writer.member("schema", kJsonSchema);
  writer.member("evidence", item.id().to_string());
  writer.member("source", to_string(item.source()));
  writer.member("source_instance", item.source_instance());
  writer.member("scope", item.scope().to_string());
  writer.member("dimension", item.dimension().to_string());
  writer.member("view", to_string(item.view()));
  writer.key("value");
  emit_quantity(writer, item.value());
  writer.member("status", to_string(item.status()));
  writer.member("generation", item.stamp().generation.value());
  writer.member("revision", item.stamp().revision.value());
  writer.member("epoch", item.stamp().epoch.value());
  writer.member("incarnation", item.stamp().incarnation.to_string());
  writer.member("observed_at", item.stamp().observed_at.value());
  writer.member("valid_until", item.stamp().valid_until.value());
  writer.member("clock_domain", item.stamp().clock_domain.name());
  writer.member("digest", item.content_digest().to_hex());
  writer.end_object();
  return std::move(writer).take();
}

std::string render_attribution_json(const AttributionItem& item) {
  JsonWriter writer;
  writer.begin_object();
  writer.member("schema", kJsonSchema);
  writer.member("attribution", item.id().to_string());
  writer.member("source", to_string(item.source()));
  writer.member("source_instance", item.source_instance());
  writer.member("scope", item.scope().to_string());
  writer.member("dimension", item.dimension().to_string());
  writer.member("target", to_string(item.target()));
  writer.member("reason", to_string(item.reason()));
  writer.member("reason_token", item.reason_token());
  writer.member("amount", item.amount());
  writer.member("generation", item.stamp().generation.value());
  writer.member("revision", item.stamp().revision.value());
  writer.member("epoch", item.stamp().epoch.value());
  writer.member("observed_at", item.stamp().observed_at.value());
  writer.member("valid_until", item.stamp().valid_until.value());
  writer.member("clock_domain", item.stamp().clock_domain.name());
  writer.member("digest", item.content_digest().to_hex());
  writer.end_object();
  return std::move(writer).take();
}

std::string render_history_json(const HistoryEntry& entry) {
  JsonWriter writer;
  writer.begin_object();
  writer.member("schema", kJsonSchema);
  writer.member("sequence", entry.sequence);
  writer.member("kind", to_string(entry.kind));
  writer.member("generation", entry.generation.value());
  writer.member("attempt", entry.attempt.value());
  if (entry.run.has_value()) {
    writer.member("run", entry.run->to_string());
  } else {
    writer.member_null("run");
  }
  if (entry.related_run.has_value()) {
    writer.member("related_run", entry.related_run->to_string());
  } else {
    writer.member_null("related_run");
  }
  writer.member("subject_digest", entry.subject_digest.to_hex());
  writer.member("detail", entry.detail);
  writer.member("recorded_at", entry.recorded_at.value());
  writer.end_object();
  return std::move(writer).take();
}

std::string render_diff_json(const RunDiff& diff) {
  JsonWriter writer;
  writer.begin_object();
  writer.member("schema", kJsonSchema);
  writer.member("before", diff.before().to_string());
  writer.member("after", diff.after().to_string());
  writer.member("digest", diff.digest().to_hex());
  writer.member("truncated", diff.truncated());
  writer.member("entries", static_cast<std::uint64_t>(diff.entries().size()));
  writer.key("transitions");
  writer.begin_object();
  for (std::uint8_t i = 0; i <= 5; ++i) {
    const auto transition = static_cast<CellTransition>(i);
    writer.member(to_string(transition),
                  static_cast<std::uint64_t>(diff.count(transition)));
  }
  writer.end_object();
  writer.key("cells");
  writer.begin_array();
  for (const DiffEntry& entry : diff.entries()) {
    writer.begin_object();
    writer.member("scope", entry.scope.to_string());
    writer.member("dimension", entry.dimension.to_string());
    writer.member("transition", to_string(entry.transition));
    if (entry.before_class.has_value()) {
      writer.member("before_class", to_string(*entry.before_class));
    } else {
      writer.member_null("before_class");
    }
    if (entry.after_class.has_value()) {
      writer.member("after_class", to_string(*entry.after_class));
    } else {
      writer.member_null("after_class");
    }
    if (entry.target.has_value()) {
      writer.member("target", to_string(*entry.target));
    } else {
      writer.member_null("target");
    }
    writer.key("before");
    emit_quantity(writer, entry.before);
    writer.key("after");
    emit_quantity(writer, entry.after);
    writer.key("delta");
    emit_quantity(writer, entry.delta);
    writer.member("before_evidence", entry.before_evidence.to_hex());
    writer.member("after_evidence", entry.after_evidence.to_hex());
    writer.end_object();
  }
  writer.end_array();
  writer.end_object();
  return std::move(writer).take();
}

std::string render_recovery_json(const RecoveryReport& report) {
  JsonWriter writer;
  writer.begin_object();
  writer.member("schema", kJsonSchema);
  writer.member("outcome", to_string(report.outcome));
  writer.member("generation", report.generation.value());
  writer.member("attempt", report.attempt.value());
  writer.member("record_digest", report.record_digest.to_hex());
  writer.member("staging_residue_removed", report.staging_residue_removed);
  writer.member("records_retired", report.records_retired);
  writer.member("note", report.note);
  writer.end_object();
  return std::move(writer).take();
}

std::string render_verification_json(const Store::VerificationReport& report) {
  JsonWriter writer;
  writer.begin_object();
  writer.member("schema", kJsonSchema);
  writer.member("ok", report.ok);
  writer.member("generation", report.generation.value());
  writer.member("record_digest", report.record_digest.to_hex());
  writer.member("records_present", report.records_present);
  writer.member("records_referenced", report.records_referenced);
  writer.key("problems");
  writer.begin_array();
  for (const std::string& problem : report.problems) {
    writer.string_value(problem);
  }
  writer.end_array();
  writer.end_object();
  return std::move(writer).take();
}

std::string render_policy_json(const ReconciliationPolicy& policy) {
  JsonWriter writer;
  writer.begin_object();
  writer.member("schema", kJsonSchema);
  writer.member("digest", policy.digest().to_hex());
  writer.member("max_generation_lag", policy.max_generation_lag);
  writer.member("missing_view", to_string(policy.missing_view));
  writer.member("stale_evidence", to_string(policy.stale_evidence));
  writer.member("rollup", to_string(policy.rollup));
  writer.member("record_conflict_sets", policy.record_conflict_sets);
  writer.member("report_unexplained_residuals", policy.report_unexplained_residuals);
  writer.key("precedence");
  writer.begin_array();
  for (EvidenceSource source : policy.precedence()) {
    writer.string_value(to_string(source));
  }
  writer.end_array();
  writer.end_object();
  return std::move(writer).take();
}

std::string render_envelope(std::string_view kind,
                            std::vector<std::pair<std::string, std::string>> fields) {
  JsonWriter writer;
  writer.begin_object();
  writer.member("schema", kJsonSchema);
  writer.member("kind", kind);
  writer.member("runtime_version", version_string());
  for (auto& field : fields) {
    writer.key(field.first);
    writer.raw_value(field.second);
  }
  writer.end_object();
  return std::move(writer).take();
}

std::string render_run_text(const ReconciliationRun& run) {
  std::string out;
  out += run.id().to_string();
  out += " gen=";
  out += std::to_string(run.generation().value());
  out += " resolution=";
  out += to_string(run.resolution());
  out += " cells=";
  out += std::to_string(run.cells().size());
  out += " conflicts=";
  out += std::to_string(run.conflict_count());
  out += " unexplained_cells=";
  out += std::to_string(run.unexplained_cell_count());
  return out;
}

std::string render_cell_text(const ReconciliationCell& cell) {
  std::string out;
  out += cell.scope.to_string();
  out += " ";
  out += cell.dimension.to_string();
  out += " -> ";
  out += to_string(cell.primary_class());
  for (const Finding& finding : cell.findings) {
    out += " | ";
    out += finding.to_string();
  }
  return out;
}

}  // namespace summon::capacity_reconciliation
