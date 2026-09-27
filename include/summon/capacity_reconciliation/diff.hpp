// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Run diff and revalidation.
//
// A diff answers "what changed between two runs" without re-deriving anything:
// every transition is computed from the two immutable runs. Revalidation is a
// separate, explicit operation: it produces a *new* run from current evidence
// and links it to its predecessor, so a stale answer is never mutated into a
// fresh one.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "summon/capacity_reconciliation/canonical.hpp"
#include "summon/capacity_reconciliation/classify.hpp"
#include "summon/capacity_reconciliation/hash.hpp"
#include "summon/capacity_reconciliation/run.hpp"
#include "summon/capacity_reconciliation/status.hpp"

namespace summon::capacity_reconciliation {

/// How one cell changed between two runs.
enum class CellTransition : std::uint8_t {
  /// Present in both runs with an identical classification, residuals and
  /// explanation digest.
  Unchanged = 0,
  /// Present in both runs, something other than the classification changed.
  ResidualChanged = 1,
  /// Present in both runs with a different primary classification.
  Reclassified = 2,
  /// Present only in the newer run.
  Appeared = 3,
  /// Present only in the older run.
  Disappeared = 4,
  /// Present in both; the newer run consumed newer or different evidence for
  /// the cell even though nothing observable changed.
  EvidenceChanged = 5,
};

[[nodiscard]] const char* to_string(CellTransition transition) noexcept;
[[nodiscard]] std::optional<CellTransition> cell_transition_from_string(
    std::string_view token) noexcept;

/// One entry of a run diff.
struct DiffEntry {
  ScopeIdentity scope;
  DimensionKey dimension;
  CellTransition transition = CellTransition::Unchanged;
  std::optional<DiscrepancyClass> before_class;
  std::optional<DiscrepancyClass> after_class;
  std::optional<ReasonTarget> target;
  Quantity before;
  Quantity after;
  Quantity delta;
  Digest before_evidence;
  Digest after_evidence;

  friend bool operator<(const DiffEntry& a, const DiffEntry& b) noexcept;
};

/// The result of differencing two runs.
class RunDiff {
 public:
  RunDiff() = default;

  [[nodiscard]] static Result<RunDiff> create(ReconciliationRunId before, ReconciliationRunId after,
                                              std::vector<DiffEntry> entries, bool truncated);

  [[nodiscard]] const ReconciliationRunId& before() const noexcept { return before_; }
  [[nodiscard]] const ReconciliationRunId& after() const noexcept { return after_; }
  [[nodiscard]] const std::vector<DiffEntry>& entries() const noexcept { return entries_; }
  [[nodiscard]] bool truncated() const noexcept { return truncated_; }

  [[nodiscard]] std::size_t count(CellTransition transition) const noexcept;
  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

  /// Canonical encoding and digest. Two diffs of the same pair of runs are
  /// byte-identical.
  [[nodiscard]] std::string canonical_bytes() const;
  [[nodiscard]] const Digest& digest() const noexcept { return digest_; }

 private:
  ReconciliationRunId before_;
  ReconciliationRunId after_;
  std::vector<DiffEntry> entries_;
  bool truncated_ = false;
  Digest digest_;
};

/// Compute the diff between two runs. Stable and symmetric in nothing: the
/// direction matters.
[[nodiscard]] Result<RunDiff> diff_runs(const ReconciliationRun& before,
                                        const ReconciliationRun& after, std::size_t max_entries);

/// Explanation digest of one cell, used to detect explanation changes that did
/// not change a classification.
[[nodiscard]] Digest cell_explanation_digest(const ReconciliationCell& cell) noexcept;

}  // namespace summon::capacity_reconciliation
