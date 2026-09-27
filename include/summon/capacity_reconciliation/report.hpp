// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Machine-readable rendering.
//
// The statement this runtime makes to the outside world is the canonical JSON
// produced here. The text renderers are conveniences and are explicitly not a
// contract; consumers must branch on the stable tokens emitted by this header.
//
// Every document is written with the canonical writer, so two runs of the same
// value produce byte-identical output: object keys are emitted in a fixed,
// documented order rather than in map order.

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "summon/capacity_reconciliation/attribution.hpp"
#include "summon/capacity_reconciliation/diff.hpp"
#include "summon/capacity_reconciliation/evidence.hpp"
#include "summon/capacity_reconciliation/policy.hpp"
#include "summon/capacity_reconciliation/run.hpp"
#include "summon/capacity_reconciliation/store.hpp"

namespace summon::capacity_reconciliation {

/// Escape a string as a JSON string literal, including the surrounding quotes.
/// Non-ASCII bytes are escaped as \u00XX so that output is pure ASCII and
/// cannot be mis-decoded under a different encoding.
[[nodiscard]] std::string json_escape(std::string_view text);

/// A minimal canonical JSON writer. Values are appended; the caller controls
/// key order, which is what makes output byte-stable.
class JsonWriter {
 public:
  JsonWriter() = default;

  void begin_object();
  void end_object();
  void begin_array();
  void end_array();

  void key(std::string_view name);
  void string_value(std::string_view value);
  /// Exact-match overload for C strings. Without it a `const char*` argument
  /// binds to the `bool` overload through a pointer-to-bool conversion and
  /// silently emits `true` instead of the token.
  void string_value(const char* value);
  void number_value(std::int64_t value);
  void unumber_value(std::uint64_t value);
  void bool_value(bool value);
  void null_value();

  /// Convenience: key + value.
  void member(std::string_view name, std::string_view value);
  /// Exact-match overload for C strings; see string_value(const char*).
  void member(std::string_view name, const char* value);
  void member(std::string_view name, std::int64_t value);
  void member(std::string_view name, std::uint64_t value);
  void member(std::string_view name, bool value);
  void member_null(std::string_view name);

  /// Pre-rendered raw JSON as a value (used for nested writers).
  void raw_value(std::string_view json);

  [[nodiscard]] const std::string& str() const noexcept { return out_; }
  [[nodiscard]] std::string take() && { return std::move(out_); }

 private:
  void separate();
  /// Emits the separator a value position needs, if any. A value that directly
  /// follows a key needs none; an array element does.
  void value_prefix();
  void indent();

  std::string out_;
  std::vector<bool> first_;
  std::size_t depth_ = 0;
  bool after_key_ = false;
};

/// Stable token for an unknown reason, suitable for JSON.
[[nodiscard]] std::string quantity_json(const Quantity& quantity);

/// Render one cell.
[[nodiscard]] std::string render_cell_json(const ReconciliationCell& cell);

/// Render one run.
[[nodiscard]] std::string render_run_json(const ReconciliationRun& run, bool include_cells);

/// Render one conflict.
[[nodiscard]] std::string render_conflict_json(const EvidenceConflict& conflict);

/// Render one evidence item.
[[nodiscard]] std::string render_evidence_json(const EvidenceItem& item);

/// Render one attribution item.
[[nodiscard]] std::string render_attribution_json(const AttributionItem& item);

/// Render one history entry.
[[nodiscard]] std::string render_history_json(const HistoryEntry& entry);

/// Render a diff.
[[nodiscard]] std::string render_diff_json(const RunDiff& diff);

/// Render the explanation graph of a cell.
[[nodiscard]] std::string render_explanation_json(const ExplanationGraph& graph);

/// Render a recovery report.
[[nodiscard]] std::string render_recovery_json(const RecoveryReport& report);

/// Render store verification.
[[nodiscard]] std::string render_verification_json(const Store::VerificationReport& report);

/// Render the policy.
[[nodiscard]] std::string render_policy_json(const ReconciliationPolicy& policy);

/// Wrap a list of already rendered JSON values in an object with metadata.
[[nodiscard]] std::string render_envelope(std::string_view kind,
                                          std::vector<std::pair<std::string, std::string>> fields);

/// Human-readable one-line summary of a run. Not a contract.
[[nodiscard]] std::string render_run_text(const ReconciliationRun& run);

/// Human-readable one-line summary of a cell. Not a contract.
[[nodiscard]] std::string render_cell_text(const ReconciliationCell& cell);

}  // namespace summon::capacity_reconciliation
