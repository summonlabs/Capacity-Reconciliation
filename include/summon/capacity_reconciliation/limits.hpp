// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Bounded resource limits.
//
// Every externally supplied size is validated against these limits *before*
// any allocation happens. Limits are compiled-in constants so that a corrupt
// or hostile declaration cannot talk the runtime into a larger allocation by
// lying about a header field.

#pragma once

#include <cstddef>
#include <cstdint>

namespace summon::capacity_reconciliation {

/// Bound on the persisted record payload. A record larger than this is
/// refused before allocation.
inline constexpr std::size_t kMaxRecordBytes = 64u * 1024u * 1024u;

/// Bound on the store marker file.
inline constexpr std::size_t kMaxMarkerBytes = 4u * 1024u;

/// Bound on the HEAD manifest file.
inline constexpr std::size_t kMaxHeadBytes = 64u * 1024u;

/// Bound on a single scope path segment.
inline constexpr std::size_t kMaxScopeSegmentBytes = 64u;

/// Bound on the number of segments in a scope path.
inline constexpr std::size_t kMaxScopeDepth = 8u;

/// Bound on a source runtime identifier string.
inline constexpr std::size_t kMaxSourceNameBytes = 64u;

/// Bound on a clock domain identifier string.
inline constexpr std::size_t kMaxClockDomainBytes = 32u;

/// Bound on a free-form reason token.
inline constexpr std::size_t kMaxReasonTokenBytes = 48u;

/// Bound on the number of digits accepted when parsing an identifier.
inline constexpr std::size_t kMaxNumericTokenBytes = 24u;

/// Default bounds for an engine instance. These are the values used when a
/// caller does not supply explicit bounds; they bound memory for hostile input.
struct Limits {
  /// Maximum evidence items resident in one engine.
  std::size_t max_evidence_items = 200000;
  /// Maximum attribution items resident in one engine.
  std::size_t max_attribution_items = 100000;
  /// Maximum distinct (scope, dimension key) cells in one reconciliation run.
  std::size_t max_cells_per_run = 20000;
  /// Maximum evidence items considered for one cell. Exceeding this is a hard
  /// error naming the cell: silently dropping evidence would be a silent fix.
  std::size_t max_evidence_per_cell = 64;
  /// Maximum conflicts recorded per cell.
  std::size_t max_conflicts_per_cell = 16;
  /// Maximum explanation nodes per cell.
  std::size_t max_explanation_nodes_per_cell = 512;
  /// Maximum residual explanations per cell.
  std::size_t max_explanation_residuals_per_cell = 16;
  /// Maximum runs retained in the store image.
  std::size_t max_runs_retained = 2000;
  /// Maximum history entries retained in the store image.
  std::size_t max_history_retained = 200000;
  /// Maximum record files retained on disk.
  std::size_t max_records_retained = 8;
  /// Maximum diff entries produced by one run diff.
  std::size_t max_diff_entries = 20000;
  /// Maximum scope selections in one run request.
  std::size_t max_scope_selections = 4096;
  /// Maximum total bytes accepted when reading a persisted file.
  std::size_t max_read_bytes = kMaxRecordBytes;
};

/// The process-wide conservative defaults.
[[nodiscard]] const Limits& default_limits() noexcept;

}  // namespace summon::capacity_reconciliation
