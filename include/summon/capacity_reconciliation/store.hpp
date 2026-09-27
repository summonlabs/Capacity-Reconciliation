// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Durable, versioned, integrity-checked store.
//
// Layout under the store root:
//
//   CAPACITY-RECONCILIATION-STORE   identity marker: magic, format version,
//                                   store uuid, endian marker
//   LOCK                            exclusive OS lock held by the writer
//   HEAD                            published manifest: the single commit point
//   HEAD.prev                       previous published manifest
//   records/record-<gen>.crr        immutable committed records
//   staging/                        staging area; any survivor is residue
//
// Commit protocol, in order, each step verified before the next:
//
//   1. plan      compute the successor state image
//   2. validate  bounds, invariants, generation monotonicity, chain link
//   3. reserve   generation = head.generation + 1, attempt = head.attempt + 1
//   4. stage     write staging/record-<gen>.tmp, flush to durable storage
//   5. verify    re-read the staged file from disk and re-verify framing,
//                digest and decodability
//   6. publish   atomic rename into records/, then atomically replace HEAD
//   7. cleanup   remove staging residue and retire unreferenced records
//
// HEAD is the commit point. A reader therefore sees either the previous whole
// state or the new whole state, never a mixture, and a crash before step 6
// leaves the store exactly as it was.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "summon/capacity_reconciliation/attribution.hpp"
#include "summon/capacity_reconciliation/evidence.hpp"
#include "summon/capacity_reconciliation/hash.hpp"
#include "summon/capacity_reconciliation/identity.hpp"
#include "summon/capacity_reconciliation/limits.hpp"
#include "summon/capacity_reconciliation/policy.hpp"
#include "summon/capacity_reconciliation/run.hpp"
#include "summon/capacity_reconciliation/status.hpp"
#include "summon/capacity_reconciliation/version.hpp"

namespace summon::capacity_reconciliation {

/// The complete authoritative content of a store at one generation.
///
/// This is a whole-value type: recovery produces exactly one of these, never a
/// merger of two.
struct StoreImage {
  std::uint32_t format_version = kStoreFormatVersion;
  Digest store_identity;
  Generation generation;
  AttemptId attempt;
  IncarnationId incarnation;
  Epoch epoch;
  Tick recorded_at;
  ClockDomain clock_domain;
  Digest previous_record_digest;

  std::vector<EvidenceItem> evidence;
  std::vector<AttributionItem> attributions;
  std::vector<ReconciliationRun> runs;
  std::vector<HistoryEntry> history;
  std::uint64_t history_sequence = 0;

  /// Set when this image was produced by recovering from the previous commit
  /// after the current HEAD failed verification. Never set on a normal open.
  bool recovered_from_previous_commit = false;
  std::string recovery_note;

  [[nodiscard]] std::string canonical_bytes() const;
  [[nodiscard]] static Result<StoreImage> decode(std::string_view bytes, const Limits& limits);
  [[nodiscard]] Status validate(const Limits& limits) const;
};

/// What happened when a store was opened.
enum class RecoveryOutcome : std::uint8_t {
  /// A new store was created.
  Created = 0,
  /// The published HEAD was read and verified.
  OpenedClean = 1,
  /// HEAD failed verification and the previous commit was adopted whole.
  RecoveredFromPreviousCommit = 2,
};

[[nodiscard]] const char* to_string(RecoveryOutcome outcome) noexcept;

/// Everything a caller needs to know about how the store came up.
struct RecoveryReport {
  RecoveryOutcome outcome = RecoveryOutcome::Created;
  Generation generation;
  AttemptId attempt;
  Digest record_digest;
  /// Non-empty when the recovery was not clean.
  std::string note;
  /// True when stale staging residue was removed during open.
  bool staging_residue_removed = false;
  /// Number of unreferenced record files retired during open.
  std::uint64_t records_retired = 0;
};

/// Options for opening a store.
struct StoreOpenOptions {
  /// When true, an absent store root is created. When false, a missing store
  /// is NOT_FOUND.
  bool create_if_missing = false;
  /// Take the exclusive writer lock. A read-only inspection opens with
  /// `writer = false` and never mutates anything.
  bool writer = true;
  Limits limits{};
  /// Retire record files that no retained commit references.
  bool retire_unreferenced_records = true;
};

/// Exclusive writer handle on a store directory.
///
/// Ownership: one Store owns one directory and one lock. Constructing a second
/// writer against the same directory from any process fails with LOCK_CONFLICT
/// until the first is destroyed or its process dies.
class Store {
 public:
  Store() = default;
  ~Store();
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;
  Store(Store&&) noexcept;
  Store& operator=(Store&&) noexcept;

  /// Open (and optionally create) a store.
  [[nodiscard]] static Result<Store> open(const std::filesystem::path& root,
                                          const StoreOpenOptions& options);

  [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }
  [[nodiscard]] bool is_writer() const noexcept { return writer_; }
  [[nodiscard]] const RecoveryReport& recovery() const noexcept { return recovery_; }

  /// Current authoritative generation. This is the precondition token every
  /// mutation must carry.
  [[nodiscard]] Generation generation() const noexcept { return image_.generation; }
  [[nodiscard]] AttemptId attempt() const noexcept { return image_.attempt; }
  [[nodiscard]] const IncarnationId& incarnation() const noexcept { return image_.incarnation; }
  [[nodiscard]] Epoch epoch() const noexcept { return image_.epoch; }
  [[nodiscard]] const Digest& store_identity() const noexcept { return image_.store_identity; }

  /// A copy of the current whole image. Callers mutate the copy, never the
  /// live state.
  [[nodiscard]] const StoreImage& image() const noexcept { return image_; }

  /// Commit a successor image. `expected` must equal the current generation or
  /// the call fails with STALE_GENERATION and nothing is written.
  ///
  /// `expected_incarnation` must equal the current incarnation or the call
  /// fails with STALE_AUTHORITY: a reopened store has a new incarnation, so a
  /// writer that survived a restart cannot commit against it.
  [[nodiscard]] Result<RecoveryReport> commit(StoreImage successor, Generation expected,
                                              const IncarnationId& expected_incarnation);

  /// Convenience: mutate a copy of the image and commit it.
  template <typename Fn>
  [[nodiscard]] Result<RecoveryReport> mutate(Generation expected,
                                              const IncarnationId& expected_incarnation, Fn&& fn) {
    StoreImage next = image_;
    Status status = fn(next);
    if (!status.ok()) {
      return status.error();
    }
    return commit(std::move(next), expected, expected_incarnation);
  }

  /// Bump the epoch. Refuses when the caller's view is stale.
  [[nodiscard]] Result<RecoveryReport> advance_epoch(Generation expected,
                                                     const IncarnationId& expected_incarnation,
                                                     Epoch new_epoch);

  /// Verify the store on disk without opening it for writing: re-read HEAD,
  /// re-read the referenced record, re-verify framing and digest, and check
  /// that the store marker agrees. Strictly at least as strict as open().
  struct VerificationReport {
    bool ok = false;
    Generation generation;
    Digest record_digest;
    std::uint64_t records_present = 0;
    std::uint64_t records_referenced = 0;
    std::vector<std::string> problems;
  };
  [[nodiscard]] static VerificationReport verify(const std::filesystem::path& root,
                                                 const Limits& limits);

  /// Path helpers, exposed so that tooling can be exactly as strict as the
  /// runtime rather than re-deriving layout.
  [[nodiscard]] static std::filesystem::path marker_path(const std::filesystem::path& root);
  [[nodiscard]] static std::filesystem::path lock_path(const std::filesystem::path& root);
  [[nodiscard]] static std::filesystem::path head_path(const std::filesystem::path& root);
  [[nodiscard]] static std::filesystem::path head_prev_path(const std::filesystem::path& root);
  [[nodiscard]] static std::filesystem::path records_dir(const std::filesystem::path& root);
  [[nodiscard]] static std::filesystem::path staging_dir(const std::filesystem::path& root);
  [[nodiscard]] static std::filesystem::path record_path(const std::filesystem::path& root,
                                                         Generation generation);

 private:
  struct LockHolder;

  [[nodiscard]] Status load_from_head(const Limits& limits, RecoveryReport* report);
  [[nodiscard]] Status adopt_image(StoreImage image, RecoveryReport* report);
  [[nodiscard]] Status write_marker(const Limits& limits);
  [[nodiscard]] Status cleanup_staging(RecoveryReport* report);
  [[nodiscard]] Status retire_records(const Limits& limits, RecoveryReport* report);

  std::filesystem::path root_;
  std::unique_ptr<LockHolder> lock_;
  /// Serialises commits on this handle. Lock order, stated once and honoured
  /// everywhere: Engine::mutex_ is acquired before Store::mutex_. Store never
  /// calls Engine, so the reverse order cannot arise.
  std::unique_ptr<std::mutex> mutex_;
  StoreImage image_;
  RecoveryReport recovery_;
  bool writer_ = false;
  Limits limits_{};
};

/// Read-only view of the last committed record, without taking the writer lock
/// and without creating anything. Used by inspection tooling.
[[nodiscard]] Result<std::pair<RecoveryReport, StoreImage>> read_published(
    const std::filesystem::path& root, const Limits& limits);

}  // namespace summon::capacity_reconciliation
