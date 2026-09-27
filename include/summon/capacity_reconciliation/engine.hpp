// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The reconciliation engine.
//
// Concurrency model, stated precisely because it is a contract:
//
//   * Ownership. An Engine owns its evidence, its attributions, its run index
//     and (optionally) one Store. No other object mutates them.
//   * Lock order. Engine::mutex_ is acquired before Store::mutex_. The reverse
//     order never occurs: Store never calls Engine, and the platform lock is
//     acquired only inside Store::mutex_.
//   * Snapshots. Readers take an immutable snapshot (shared_ptr to a const
//     EvidenceSet / AttributionSet) under the lock and then work outside it, so
//     a long computation never blocks an append.
//   * Callbacks. Observers are invoked after every internal lock has been
//     released, on the calling thread, with an immutable copy of the event.
//     An observer may therefore call back into the Engine without deadlocking.
//   * Cancellation. There is none to get wrong: every operation either
//     completes or fails, and no operation waits on another thread.
//
// The audit checklist and its results are in docs/CONCURRENCY-AUDIT.md.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "summon/capacity_reconciliation/attribution.hpp"
#include "summon/capacity_reconciliation/diff.hpp"
#include "summon/capacity_reconciliation/evidence.hpp"
#include "summon/capacity_reconciliation/limits.hpp"
#include "summon/capacity_reconciliation/policy.hpp"
#include "summon/capacity_reconciliation/run.hpp"
#include "summon/capacity_reconciliation/scope.hpp"
#include "summon/capacity_reconciliation/status.hpp"
#include "summon/capacity_reconciliation/store.hpp"

namespace summon::capacity_reconciliation {

/// A ticket authorising a successor run for a reopened run.
///
/// A reopened run is explicitly *not* current: it has no authority to produce a
/// result until a successor run is computed at a later generation. Holding a
/// ticket from an older generation and presenting it after another writer has
/// advanced the store is refused, so a reopen cannot be replayed.
struct ReopenTicket {
  ReconciliationRunId reopened_run;
  Generation issued_at;
  IncarnationId incarnation;
  Tick issued_at_tick;
};

/// Options for constructing an engine.
struct EngineOptions {
  Limits limits{};
  ReconciliationPolicy policy = ReconciliationPolicy::standard();
  ClockDomain clock_domain;
  /// Maximum runs retained in memory (and in the store image).
  std::size_t max_runs_retained = 2000;
};

/// Statistics about the last reconciliation, for diagnosis only.
struct ReconcileStats {
  std::uint64_t cells = 0;
  std::uint64_t evidence_considered = 0;
  std::uint64_t attributions_considered = 0;
  std::uint64_t conflicts = 0;
  std::uint64_t unexplained_cells = 0;
};

class Engine {
 public:
  Engine() = default;
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  /// Moves every owned member and default-constructs a fresh mutex in the
  /// destination. A moved-to engine is fully usable; a moved-from engine is
  /// empty and must not be used. Moves are for construction and factory
  /// returns, never for sharing an engine between threads.
  Engine(Engine&& other) noexcept;
  Engine& operator=(Engine&& other) noexcept;

  /// Create an in-memory engine with no durable backing.
  [[nodiscard]] static Result<Engine> create(EngineOptions options);

  /// Create an engine backed by an already-open store. The engine adopts the
  /// store's committed evidence, attributions, runs and history as recovered
  /// state: nothing recovered is treated as freshly observed.
  [[nodiscard]] static Result<Engine> attach(Store store, EngineOptions options);

  // ---- observation ingestion -------------------------------------------

  /// Append evidence. The precondition generation must match the store's, or
  /// the call fails with STALE_GENERATION and nothing is recorded. Duplicate
  /// identities are refused with ALREADY_EXISTS; identity is never reused.
  [[nodiscard]] Status append_evidence(std::vector<EvidenceItem> items, Generation expected,
                                       const IncarnationId& expected_incarnation);

  /// Append attributions. Same preconditions as append_evidence.
  [[nodiscard]] Status append_attributions(std::vector<AttributionItem> items, Generation expected,
                                           const IncarnationId& expected_incarnation);

  // ---- reconciliation ---------------------------------------------------

  /// Compute a run without persisting it. Pure function of the engine's frozen
  /// evidence snapshot, the request and the policy.
  [[nodiscard]] Result<ReconciliationRun> reconcile(const RunRequest& request) const;

  /// Compute a run and commit it to the store in one durable generation.
  [[nodiscard]] Result<ReconciliationRun> reconcile_and_commit(
      const RunRequest& request, Generation expected, const IncarnationId& expected_incarnation);

  /// Recompute a run from *current* evidence and commit it as the successor of
  /// `prior`. The prior run's resolution becomes Superseded.
  [[nodiscard]] Result<ReconciliationRun> revalidate(ReconciliationRunId prior, Tick instant,
                                                     Generation expected,
                                                     const IncarnationId& expected_incarnation);

  /// Reopen a terminal run. The run's resolution becomes Reopened and a ticket
  /// is returned authorising exactly one successor run.
  [[nodiscard]] Result<ReopenTicket> reopen(ReconciliationRunId prior, Tick instant,
                                            Generation expected,
                                            const IncarnationId& expected_incarnation);

  /// Close a run without a successor.
  [[nodiscard]] Status close_run(ReconciliationRunId run, Tick instant, Generation expected,
                                 const IncarnationId& expected_incarnation);

  /// Compute the successor authorised by a ticket. Refuses a ticket whose
  /// generation or incarnation is no longer current.
  [[nodiscard]] Result<ReconciliationRun> reconcile_successor(
      const ReopenTicket& ticket, Tick instant, Generation expected,
      const IncarnationId& expected_incarnation);

  // ---- queries ----------------------------------------------------------

  [[nodiscard]] Result<ReconciliationRun> find_run(ReconciliationRunId id) const;
  [[nodiscard]] const std::vector<ReconciliationRun>& runs() const noexcept { return runs_; }
  [[nodiscard]] const std::vector<HistoryEntry>& history() const noexcept { return history_; }
  [[nodiscard]] std::vector<HistoryEntry> history_for(ReconciliationRunId id) const;
  [[nodiscard]] Result<RunDiff> diff(ReconciliationRunId before, ReconciliationRunId after) const;

  [[nodiscard]] std::shared_ptr<const EvidenceSet> evidence_snapshot() const;
  [[nodiscard]] std::shared_ptr<const AttributionSet> attribution_snapshot() const;
  [[nodiscard]] const Digest& evidence_digest() const noexcept { return evidence_digest_; }
  [[nodiscard]] const Digest& attribution_digest() const noexcept {
    return attribution_digest_;
  }

  [[nodiscard]] Generation generation() const;
  [[nodiscard]] IncarnationId incarnation() const;
  /// Control-plane epoch. Zero for an in-memory engine, which has no persisted
  /// epoch to advance.
  [[nodiscard]] Epoch epoch() const;
  [[nodiscard]] const Limits& limits() const noexcept { return options_.limits; }
  [[nodiscard]] const EngineOptions& options() const noexcept { return options_; }
  [[nodiscard]] bool durable() const noexcept { return store_.has_value(); }
  [[nodiscard]] ReconcileStats last_stats() const;

  /// Observer invoked after all internal locks are released. Setting an
  /// observer while another thread is mid-append is safe: the pointer swap is
  /// itself serialised by the engine lock, and no observer ever runs under a
  /// lock.
  using Observer = std::function<void(const HistoryEntry&)>;
  void set_observer(Observer observer);

  /// Explicitly change the policy. Recorded as a history entry and, when
  /// durable, committed as its own generation so that the policy a run was
  /// computed under is always recoverable.
  [[nodiscard]] Status set_policy(ReconciliationPolicy policy, Generation expected,
                                  const IncarnationId& expected_incarnation);

  /// Advance the control-plane epoch. Every piece of authority granted under
  /// the previous epoch is thereby fenced. Durable engines only.
  [[nodiscard]] Status advance_epoch(Epoch new_epoch, Generation expected,
                                     const IncarnationId& expected_incarnation);

 private:
  [[nodiscard]] Result<ReconciliationRun> compute(const RunRequest& request,
                                                  ReconcileStats* stats) const;

  /// Assign the next sequence number to `entry`, record it, and (when durable)
  /// stage it into `image`. Takes the entry by reference so that the caller --
  /// and therefore the observer it notifies -- sees the assigned sequence.
  [[nodiscard]] Status append_history(HistoryEntry& entry, StoreImage* image);
  /// Undo the most recent append_history when the durable mutation that was
  /// supposed to carry it failed. An in-memory history entry must never survive
  /// a commit that did not happen.
  void append_history_rollback();
  /// True when the caller's incarnation is not the current one. An in-memory
  /// engine and a durable engine differ only in where the incarnation lives.
  [[nodiscard]] bool stale_incarnation(const IncarnationId& expected) const;
  void notify(const HistoryEntry& entry) const;
  /// Attempt counter, from the store when durable and from memory otherwise.
  [[nodiscard]] AttemptId attempt() const;
  /// The store's current whole state, stamped with this engine's clock domain,
  /// ready to be mutated and committed. Precondition: durable().
  [[nodiscard]] StoreImage stage_image() const;

  EngineOptions options_;
  mutable std::mutex mutex_;
  std::optional<Store> store_;
  std::shared_ptr<const EvidenceSet> evidence_;
  std::shared_ptr<const AttributionSet> attributions_;
  Digest evidence_digest_;
  Digest attribution_digest_;
  std::vector<ReconciliationRun> runs_;
  std::vector<HistoryEntry> history_;
  std::uint64_t history_sequence_ = 0;
  /// Generation used only when the engine has no durable backing.
  Generation generation_;
  /// Incarnation used only when the engine has no durable backing.
  IncarnationId incarnation_;
  /// Attempt counter used only when the engine has no durable backing.
  AttemptId attempt_;
  Observer observer_;
  mutable ReconcileStats last_stats_{};
};

}  // namespace summon::capacity_reconciliation
