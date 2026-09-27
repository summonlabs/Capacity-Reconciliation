// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Reconciliation runs, cells, conflicts and history.
//
// A ReconciliationRun is an immutable record of one deterministic evaluation:
// "at generation G, over this evidence set, under this policy, at this instant,
// here is what disagrees and what remains unexplained". It is never mutated
// after computation. Superseding, closing and reopening produce *new* runs
// linked by lineage, so history is append-only and every prior answer stays
// reproducible.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "summon/capacity_reconciliation/attribution.hpp"
#include "summon/capacity_reconciliation/canonical.hpp"
#include "summon/capacity_reconciliation/classify.hpp"
#include "summon/capacity_reconciliation/evidence.hpp"
#include "summon/capacity_reconciliation/explain.hpp"
#include "summon/capacity_reconciliation/hash.hpp"
#include "summon/capacity_reconciliation/identity.hpp"
#include "summon/capacity_reconciliation/limits.hpp"
#include "summon/capacity_reconciliation/policy.hpp"
#include "summon/capacity_reconciliation/scope.hpp"
#include "summon/capacity_reconciliation/status.hpp"
#include "summon/capacity_reconciliation/value.hpp"

namespace summon::capacity_reconciliation {

/// One unresolved disagreement between two authoritative statements about the
/// same scope, dimension key and view.
struct EvidenceConflict {
  ConflictId id;
  ScopeIdentity scope;
  DimensionKey dimension;
  CapacityView view = CapacityView::Planned;
  /// Every statement that took part, in canonical order.
  std::vector<EvidenceId> participants;
  /// Digests of the participating statements, parallel to `participants`.
  std::vector<Digest> participant_digests;
  /// The distinct values that were asserted, sorted ascending.
  std::vector<Amount> values;
  /// Source families that asserted each value, parallel to `values`.
  std::vector<EvidenceSource> asserting_sources;
  /// Stable token explaining why the conflict could not be resolved, e.g.
  /// `equal_precedence`, `intra_source_disagreement`, `cross_unit`.
  std::string unresolvable_reason;
  /// The value the policy selected despite the conflict, if any. Present only
  /// when precedence produced a unique winner; consumers must still treat the
  /// cell as conflicted.
  std::optional<Amount> selected_value;

  friend bool operator==(const EvidenceConflict& a, const EvidenceConflict& b) noexcept {
    return a.id == b.id && a.values == b.values && a.participants == b.participants;
  }
};

/// The resolved view vector for one cell, plus the evidence that produced it.
struct CellViews {
  /// Indexed by view_index(CapacityView). Unknown when the view is absent.
  std::array<Quantity, kCapacityViewCount> views{};
  /// The evidence item selected as authoritative for each view, if any.
  std::array<std::optional<EvidenceId>, kCapacityViewCount> selected{};
  /// Source family of each selected item.
  std::array<EvidenceSource, kCapacityViewCount> selected_source{};
  /// How many evidence items contributed to each view before selection.
  std::array<std::uint32_t, kCapacityViewCount> candidate_count{};
  /// True when the selected value for the view came from a statement the
  /// producer itself marked as partial.
  std::array<bool, kCapacityViewCount> partial{};
  /// True when the view is present and usable.
  [[nodiscard]] bool has(CapacityView view) const noexcept {
    return selected[view_index(view)].has_value();
  }
};

/// One reconciled cell: a (scope, dimension key) pair and everything known
/// about it.
struct ReconciliationCell {
  ScopeIdentity scope;
  DimensionKey dimension;
  CellViews views;

  /// Exact residuals, computed with checked arithmetic.
  Quantity planned_gap;              ///< planned - installed
  Quantity observed_gap;             ///< installed - observed
  Quantity derate_gap;               ///< observed - usable
  Quantity headroom_gap;             ///< usable - declared allocatable
  Quantity reservation_pressure;     ///< reserved - installed
  Quantity reservation_overhang;     ///< reserved - declared allocatable
  Quantity derived_allocatable;      ///< usable - reserved, computed
  Quantity allocatable_skew;         ///< declared allocatable - derived

  /// Findings, in stable order. findings[0] is the primary classification when
  /// findings is non-empty.
  std::vector<Finding> findings;

  /// Explanation graph for the residuals.
  ExplanationGraph explanation;

  /// Conflicts observed for this cell.
  std::vector<EvidenceConflict> conflicts;

  /// Scope keys of the same dimension expressed in a different unit that were
  /// seen in the same scope. Recorded so that a unit split is visible rather
  /// than silent.
  std::vector<Unit> sibling_units;

  /// Number of evidence items seen for this cell (before selection).
  std::uint64_t evidence_seen = 0;
  /// Number of attribution items applied for this cell.
  std::uint64_t attributions_applied = 0;

  /// Rolled-up from descendant scopes rather than exact at `scope`.
  bool rolled_up = false;
  /// Number of descendant scopes that contributed to a rollup.
  std::uint32_t rollup_contributors = 0;
  /// Number of descendant scopes whose contribution was unknown.
  std::uint32_t rollup_missing = 0;

  /// The cell's headline classification: the first finding, in the declared
  /// precedence order, that either exceeds its configured tolerance or is a
  /// cell-level finding (conflict, staleness, incompleteness, unit split,
  /// overflow, absence of evidence). A difference recorded *within* tolerance
  /// stays visible as a finding and as an exact residual, but it never becomes
  /// the headline: a cell inside its tolerance band is classified as agreeing.
  [[nodiscard]] DiscrepancyClass primary_class() const noexcept;
  /// True when a finding of this class is present, whether or not it exceeded
  /// tolerance.
  [[nodiscard]] bool has_class(DiscrepancyClass value) const noexcept;
  [[nodiscard]] const Quantity& view(CapacityView v) const noexcept {
    return views.views[view_index(v)];
  }
};

/// Stable ordering of cells: by scope, then dimension key.
[[nodiscard]] bool cell_less(const ReconciliationCell& a, const ReconciliationCell& b) noexcept;

/// What a run was asked to do. Frozen at compute time.
struct RunRequest {
  std::vector<ScopeSelection> scopes;
  Generation generation;
  Tick evaluation_instant;
  ClockDomain clock_domain;
  ReconciliationPolicy policy;

  /// Minimum evidence generation accepted. Zero disables the check.
  Generation min_evidence_generation;

  /// When set, the run is a successor of the named run.
  std::optional<ReconciliationRunId> parent_run;

  /// When true the run is computed but explicitly marked as recovered from
  /// persisted state and not yet revalidated against current evidence.
  bool from_recovered_state = false;

  /// Optional caller label, bounded token.
  std::string label;
};

/// One immutable reconciliation run.
class ReconciliationRun {
 public:
  ReconciliationRun() = default;

  [[nodiscard]] static Result<ReconciliationRun> create(ReconciliationRunId id, RunRequest request,
                                                        Digest evidence_digest,
                                                        Digest attribution_digest,
                                                        std::vector<ReconciliationCell> cells);

  [[nodiscard]] const ReconciliationRunId& id() const noexcept { return id_; }
  [[nodiscard]] const RunRequest& request() const noexcept { return request_; }
  [[nodiscard]] Generation generation() const noexcept { return request_.generation; }
  [[nodiscard]] Tick evaluation_instant() const noexcept { return request_.evaluation_instant; }
  [[nodiscard]] const ReconciliationPolicy& policy() const noexcept { return request_.policy; }
  [[nodiscard]] const std::vector<ReconciliationCell>& cells() const noexcept { return cells_; }
  [[nodiscard]] const Digest& evidence_digest() const noexcept { return evidence_digest_; }
  [[nodiscard]] const Digest& attribution_digest() const noexcept {
    return attribution_digest_;
  }
  [[nodiscard]] const Digest& policy_digest() const noexcept { return policy_digest_; }

  /// Digest over the exact evidence vector, the policy, the request and every
  /// cell. Independent of the order evidence, attributions, scopes or cells
  /// were supplied in.
  [[nodiscard]] const Digest& run_digest() const noexcept { return run_digest_; }

  [[nodiscard]] ResolutionState resolution() const noexcept { return resolution_; }
  void set_resolution(ResolutionState state) noexcept { resolution_ = state; }

  [[nodiscard]] AttemptId commit_attempt() const noexcept { return commit_attempt_; }
  void set_commit_attempt(AttemptId attempt) noexcept { commit_attempt_ = attempt; }

  /// Generation at which the run was committed to a store, if it was.
  [[nodiscard]] std::optional<Generation> committed_generation() const noexcept {
    return committed_generation_;
  }
  void set_committed_generation(Generation generation) noexcept {
    committed_generation_ = generation;
  }

  /// The run that superseded this one, if any.
  [[nodiscard]] const std::optional<ReconciliationRunId>& superseded_by() const noexcept {
    return superseded_by_;
  }
  void set_superseded_by(ReconciliationRunId id) noexcept { superseded_by_ = id; }

  /// Scope keys covered by this run, derived from cells but retained even for
  /// empty runs so that a run over an empty facility is still identifiable.
  [[nodiscard]] const std::vector<ScopeSelection>& scopes() const noexcept {
    return request_.scopes;
  }

  /// Count of cells by primary classification.
  [[nodiscard]] std::size_t count_class(DiscrepancyClass value) const noexcept;
  /// Count of cells whose explanation retains a known non-zero unexplained
  /// residual.
  [[nodiscard]] std::size_t unexplained_cell_count() const noexcept;
  /// Count of unresolved conflicts across all cells.
  [[nodiscard]] std::size_t conflict_count() const noexcept;

  /// Recompute the resolution state from the cells. Deterministic.
  [[nodiscard]] ResolutionState derive_resolution() const noexcept;

  /// Canonical encoding of the whole run.
  [[nodiscard]] std::string canonical_bytes() const;

  /// Decode a run from its canonical content bytes. The identity, resolution
  /// and commit metadata are *not* part of the content: they are carried by the
  /// envelope (decode_envelope) precisely so that mutable metadata cannot alter
  /// the content digest.
  [[nodiscard]] static Result<ReconciliationRun> decode_content(std::string_view content,
                                                                const Limits& limits);

  /// Decode the envelope: identity, content, resolution, attempt, commit
  /// generation and supersession.
  [[nodiscard]] static Result<ReconciliationRun> decode_envelope(CanonicalReader& reader,
                                                                 const Limits& limits);

  /// Recompute run_digest_ from canonical_bytes(). Used after decoding.
  [[nodiscard]] Status reseal();

  /// Structural validation of a decoded run: bounds, ordering, index integrity.
  [[nodiscard]] Status validate(std::size_t limits_max_cells) const;

 private:
  ReconciliationRunId id_;
  RunRequest request_;
  Digest evidence_digest_;
  Digest attribution_digest_;
  Digest policy_digest_;
  Digest run_digest_;
  std::vector<ReconciliationCell> cells_;
  ResolutionState resolution_ = ResolutionState::Open;
  AttemptId commit_attempt_;
  std::optional<Generation> committed_generation_;
  std::optional<ReconciliationRunId> superseded_by_;
};

/// A single append-only history entry.
enum class HistoryEventKind : std::uint8_t {
  RunComputed = 0,
  RunCommitted = 1,
  RunSuperseded = 2,
  RunClosed = 3,
  RunReopened = 4,
  EvidenceAppended = 5,
  AttributionAppended = 6,
  PolicyChanged = 7,
  StoreRecovered = 8,
  RecordsRetired = 9,
};

[[nodiscard]] const char* to_string(HistoryEventKind kind) noexcept;
[[nodiscard]] std::optional<HistoryEventKind> history_event_kind_from_string(
    std::string_view token) noexcept;

/// One immutable history entry. `sequence` is assigned by the store and is
/// strictly increasing; it is the ordering authority for history.
struct HistoryEntry {
  std::uint64_t sequence = 0;
  HistoryEventKind kind = HistoryEventKind::RunComputed;
  Generation generation;
  AttemptId attempt;
  std::optional<ReconciliationRunId> run;
  std::optional<ReconciliationRunId> related_run;
  Digest subject_digest;
  std::string detail;
  Tick recorded_at;

  friend bool operator<(const HistoryEntry& a, const HistoryEntry& b) noexcept {
    return a.sequence < b.sequence;
  }
};

}  // namespace summon::capacity_reconciliation
