// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/capacity_reconciliation/diff.hpp"

#include <algorithm>
#include <array>

namespace summon::capacity_reconciliation {
namespace {

struct TransitionToken {
  CellTransition transition;
  const char* token;
};

constexpr std::array<TransitionToken, 6> kTransitionTokens{{
    {CellTransition::Unchanged, "unchanged"},
    {CellTransition::ResidualChanged, "residual_changed"},
    {CellTransition::Reclassified, "reclassified"},
    {CellTransition::Appeared, "appeared"},
    {CellTransition::Disappeared, "disappeared"},
    {CellTransition::EvidenceChanged, "evidence_changed"},
}};

constexpr std::string_view kDiffDomain = "capacity-reconciliation/diff/v1";
constexpr std::string_view kCellDomain = "capacity-reconciliation/cell/v1";

constexpr std::uint8_t kCellSchema = 1;

enum Tag : std::uint8_t {
  kTagSchema = 1,
  kTagBefore = 2,
  kTagAfter = 3,
  kTagTruncated = 4,
  kTagEntryCount = 5,
  kTagEntry = 6,
  kTagScope = 7,
  kTagDimension = 8,
  kTagTransition = 9,
  kTagHasBeforeClass = 10,
  kTagBeforeClass = 11,
  kTagHasAfterClass = 12,
  kTagAfterClass = 13,
  kTagHasTarget = 14,
  kTagTarget = 15,
  kTagBeforeValue = 16,
  kTagAfterValue = 17,
  kTagDeltaValue = 18,
  kTagBeforeEvidence = 19,
  kTagAfterEvidence = 20,
  kTagEnd = 0x7F,
};

void write_quantity(CanonicalWriter& writer, std::uint8_t tag, const Quantity& quantity) {
  writer.field(tag);
  writer.boolean(quantity.is_known());
  if (quantity.is_known()) {
    writer.i64(quantity.value());
  } else {
    writer.token(to_string(quantity.reason()));
  }
}

}  // namespace

const char* to_string(CellTransition transition) noexcept {
  for (const TransitionToken& entry : kTransitionTokens) {
    if (entry.transition == transition) {
      return entry.token;
    }
  }
  return "unrecognised";
}

std::optional<CellTransition> cell_transition_from_string(std::string_view token) noexcept {
  for (const TransitionToken& entry : kTransitionTokens) {
    if (token == entry.token) {
      return entry.transition;
    }
  }
  return std::nullopt;
}

bool operator<(const DiffEntry& a, const DiffEntry& b) noexcept {
  if (!(a.scope == b.scope)) {
    return a.scope < b.scope;
  }
  if (a.dimension != b.dimension) {
    return a.dimension < b.dimension;
  }
  return static_cast<std::uint8_t>(a.target.value_or(ReasonTarget::None)) <
         static_cast<std::uint8_t>(b.target.value_or(ReasonTarget::None));
}

Digest cell_explanation_digest(const ReconciliationCell& cell) noexcept {
  CanonicalWriter writer;
  writer.u8(kCellSchema);
  writer.bytes(cell.scope.to_string());
  writer.token(cell.dimension.to_string());
  for (std::size_t i = 0; i < kCapacityViewCount; ++i) {
    writer.boolean(cell.views.selected[i].has_value());
    if (cell.views.selected[i].has_value()) {
      writer.bytes(cell.views.selected[i]->to_string());
    }
    writer.boolean(cell.views.partial[i]);
    const Quantity& value = cell.views.views[i];
    writer.boolean(value.is_known());
    if (value.is_known()) {
      writer.i64(value.value());
    } else {
      writer.token(to_string(value.reason()));
    }
  }
  const Quantity* residuals[] = {
      &cell.planned_gap,          &cell.observed_gap,          &cell.derate_gap,
      &cell.headroom_gap,         &cell.reservation_pressure,  &cell.reservation_overhang,
      &cell.derived_allocatable,  &cell.allocatable_skew,
  };
  for (const Quantity* quantity : residuals) {
    writer.boolean(quantity->is_known());
    if (quantity->is_known()) {
      writer.i64(quantity->value());
    } else {
      writer.token(to_string(quantity->reason()));
    }
  }
  for (const Finding& finding : cell.findings) {
    writer.token(to_string(finding.classification));
    writer.token(to_string(finding.target));
    writer.boolean(finding.magnitude.has_value());
    if (finding.magnitude.has_value()) {
      writer.i64(*finding.magnitude);
    }
    writer.i64(finding.tolerance);
  }
  for (const EvidenceConflict& conflict : cell.conflicts) {
    writer.bytes(conflict.id.to_string());
    writer.token(conflict.unresolvable_reason);
  }
  for (Unit unit : cell.sibling_units) {
    writer.token(to_string(unit));
  }
  writer.boolean(cell.rolled_up);
  writer.u32(cell.rollup_contributors);
  writer.u32(cell.rollup_missing);
  writer.boolean(cell.explanation.has_unexplained());
  return writer.digest(kCellDomain);
}

Result<RunDiff> RunDiff::create(ReconciliationRunId before, ReconciliationRunId after,
                                std::vector<DiffEntry> entries, bool truncated) {
  if (!before.valid() || !after.valid()) {
    return make_error(ErrorCode::InvalidArgument, "a diff requires two run identities", "run_id");
  }
  if (before == after) {
    return make_error(ErrorCode::InvalidArgument,
                      "a diff of a run against itself is refused: it would always be empty and "
                      "would hide a caller mistake",
                      "run_id");
  }
  RunDiff diff;
  diff.before_ = before;
  diff.after_ = after;
  diff.truncated_ = truncated;
  diff.entries_ = std::move(entries);
  std::stable_sort(diff.entries_.begin(), diff.entries_.end());
  diff.digest_ = digest_with_domain(kDiffDomain, diff.canonical_bytes());
  return diff;
}

std::size_t RunDiff::count(CellTransition transition) const noexcept {
  std::size_t total = 0;
  for (const DiffEntry& entry : entries_) {
    if (entry.transition == transition) {
      ++total;
    }
  }
  return total;
}

std::string RunDiff::canonical_bytes() const {
  CanonicalWriter writer;
  writer.field(kTagSchema);
  writer.u8(kCellSchema);
  writer.field(kTagBefore);
  writer.bytes(before_.to_string());
  writer.field(kTagAfter);
  writer.bytes(after_.to_string());
  writer.field(kTagTruncated);
  writer.boolean(truncated_);
  writer.field(kTagEntryCount);
  writer.u64(static_cast<std::uint64_t>(entries_.size()));
  for (const DiffEntry& entry : entries_) {
    writer.field(kTagEntry);
    writer.field(kTagScope);
    writer.bytes(entry.scope.to_string());
    writer.field(kTagDimension);
    writer.token(entry.dimension.to_string());
    writer.field(kTagTransition);
    writer.token(to_string(entry.transition));
    writer.field(kTagHasBeforeClass);
    writer.boolean(entry.before_class.has_value());
    if (entry.before_class.has_value()) {
      writer.field(kTagBeforeClass);
      writer.token(to_string(*entry.before_class));
    }
    writer.field(kTagHasAfterClass);
    writer.boolean(entry.after_class.has_value());
    if (entry.after_class.has_value()) {
      writer.field(kTagAfterClass);
      writer.token(to_string(*entry.after_class));
    }
    writer.field(kTagHasTarget);
    writer.boolean(entry.target.has_value());
    if (entry.target.has_value()) {
      writer.field(kTagTarget);
      writer.token(to_string(*entry.target));
    }
    write_quantity(writer, kTagBeforeValue, entry.before);
    write_quantity(writer, kTagAfterValue, entry.after);
    write_quantity(writer, kTagDeltaValue, entry.delta);
    writer.field(kTagBeforeEvidence);
    writer.bytes(entry.before_evidence.to_hex());
    writer.field(kTagAfterEvidence);
    writer.bytes(entry.after_evidence.to_hex());
  }
  writer.field(kTagEnd);
  return std::move(writer).take();
}

Result<RunDiff> diff_runs(const ReconciliationRun& before, const ReconciliationRun& after,
                          std::size_t max_entries) {
  if (before.id() == after.id()) {
    return make_error(ErrorCode::InvalidArgument, "a run cannot be diffed against itself",
                      "run_id");
  }

  std::vector<DiffEntry> entries;
  bool truncated = false;

  const std::vector<ReconciliationCell>& before_cells = before.cells();
  const std::vector<ReconciliationCell>& after_cells = after.cells();

  std::size_t i = 0;
  std::size_t j = 0;
  while (i < before_cells.size() || j < after_cells.size()) {
    if (entries.size() >= max_entries) {
      truncated = true;
      break;
    }
    const bool take_before =
        j >= after_cells.size() ||
        (i < before_cells.size() && cell_less(before_cells[i], after_cells[j]));
    const bool take_after =
        i >= before_cells.size() ||
        (j < after_cells.size() && cell_less(after_cells[j], before_cells[i]));

    if (take_before) {
      const ReconciliationCell& cell = before_cells[i];
      DiffEntry entry;
      entry.scope = cell.scope;
      entry.dimension = cell.dimension;
      entry.transition = CellTransition::Disappeared;
      entry.before_class = cell.primary_class();
      entry.after_class = std::nullopt;
      entry.target = cell.findings.empty() ? std::optional<ReasonTarget>{}
                                           : std::optional<ReasonTarget>{cell.findings.front().target};
      entry.before = cell.findings.empty() || !cell.findings.front().magnitude.has_value()
                         ? Quantity::unknown(UnknownReason::NotReported)
                         : Quantity::known(*cell.findings.front().magnitude);
      entry.after = Quantity::unknown(UnknownReason::NotReported);
      entry.delta = Quantity::unknown(UnknownReason::NotReported);
      entry.before_evidence = cell_explanation_digest(cell);
      entries.push_back(std::move(entry));
      ++i;
      continue;
    }
    if (take_after) {
      const ReconciliationCell& cell = after_cells[j];
      DiffEntry entry;
      entry.scope = cell.scope;
      entry.dimension = cell.dimension;
      entry.transition = CellTransition::Appeared;
      entry.before_class = std::nullopt;
      entry.after_class = cell.primary_class();
      entry.target = cell.findings.empty() ? std::optional<ReasonTarget>{}
                                           : std::optional<ReasonTarget>{cell.findings.front().target};
      entry.before = Quantity::unknown(UnknownReason::NotReported);
      entry.after = cell.findings.empty() || !cell.findings.front().magnitude.has_value()
                        ? Quantity::unknown(UnknownReason::NotReported)
                        : Quantity::known(*cell.findings.front().magnitude);
      entry.delta = Quantity::unknown(UnknownReason::NotReported);
      entry.after_evidence = cell_explanation_digest(cell);
      entries.push_back(std::move(entry));
      ++j;
      continue;
    }

    const ReconciliationCell& left = before_cells[i];
    const ReconciliationCell& right = after_cells[j];
    DiffEntry entry;
    entry.scope = left.scope;
    entry.dimension = left.dimension;
    entry.before_class = left.primary_class();
    entry.after_class = right.primary_class();
    entry.before_evidence = cell_explanation_digest(left);
    entry.after_evidence = cell_explanation_digest(right);

    const bool has_left = !left.findings.empty() && left.findings.front().magnitude.has_value();
    const bool has_right = !right.findings.empty() && right.findings.front().magnitude.has_value();
    entry.before = has_left ? Quantity::known(*left.findings.front().magnitude)
                            : Quantity::unknown(UnknownReason::NotReported);
    entry.after = has_right ? Quantity::known(*right.findings.front().magnitude)
                            : Quantity::unknown(UnknownReason::NotReported);
    if (!left.findings.empty()) {
      entry.target = left.findings.front().target;
    } else if (!right.findings.empty()) {
      entry.target = right.findings.front().target;
    }
    if (has_left && has_right) {
      auto delta = sub_quantities(entry.after, entry.before, UnknownReason::NotReported);
      entry.delta = delta.ok() ? delta.value() : Quantity::unknown(UnknownReason::NotReported);
    } else {
      entry.delta = Quantity::unknown(UnknownReason::NotReported);
    }

    if (left.primary_class() != right.primary_class()) {
      entry.transition = CellTransition::Reclassified;
    } else if (!(entry.before_evidence == entry.after_evidence)) {
      entry.transition = CellTransition::ResidualChanged;
    } else if (left.evidence_seen != right.evidence_seen) {
      entry.transition = CellTransition::EvidenceChanged;
    } else {
      entry.transition = CellTransition::Unchanged;
    }
    entries.push_back(std::move(entry));
    ++i;
    ++j;
  }

  return RunDiff::create(before.id(), after.id(), std::move(entries), truncated);
}

}  // namespace summon::capacity_reconciliation
