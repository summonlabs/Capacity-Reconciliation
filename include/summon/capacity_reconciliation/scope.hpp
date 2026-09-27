// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Scope identity.
//
// A scope names a position in the facility hierarchy: site / hall / row / rack
// / slot, or any prefix of it. Scope identity is a validated canonical path,
// not a free-form string, so that two spellings of the same place cannot be
// reconciled separately, and so that a path can never escape its tree.
//
// Path segments are restricted to `[a-z0-9._-]`, at most kMaxScopeSegmentBytes
// bytes, at most kMaxScopeDepth segments. Segment comparison is byte-wise and
// case-sensitive: there is no Unicode normalisation, no case folding and no
// locale, because any of those would make scope identity depend on the host.

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "summon/capacity_reconciliation/status.hpp"

namespace summon::capacity_reconciliation {

/// One validated path segment.
class ScopeSegment {
 public:
  ScopeSegment() = default;

  [[nodiscard]] static Result<ScopeSegment> create(std::string_view text);

  [[nodiscard]] const std::string& value() const noexcept { return value_; }
  [[nodiscard]] bool empty() const noexcept { return value_.empty(); }

  friend bool operator==(const ScopeSegment& a, const ScopeSegment& b) noexcept {
    return a.value_ == b.value_;
  }
  friend bool operator<(const ScopeSegment& a, const ScopeSegment& b) noexcept {
    return a.value_ < b.value_;
  }

 private:
  std::string value_;
};

/// A validated, canonical facility path.
class ScopeIdentity {
 public:
  ScopeIdentity() = default;

  /// Parse a `site/hall/row` style path. Rejects empty segments, leading or
  /// trailing separators, `.`/`..` segments, backslashes, drive letters and
  /// over-long or over-deep paths. The result is fully canonical.
  [[nodiscard]] static Result<ScopeIdentity> parse(std::string_view path);

  /// Build from already validated segments.
  [[nodiscard]] static Result<ScopeIdentity> from_segments(
      const std::vector<ScopeSegment>& segments);

  /// The canonical `/`-joined rendering.
  [[nodiscard]] std::string to_string() const;

  [[nodiscard]] const std::vector<ScopeSegment>& segments() const noexcept { return segments_; }
  [[nodiscard]] std::size_t depth() const noexcept { return segments_.size(); }
  [[nodiscard]] bool empty() const noexcept { return segments_.empty(); }

  /// True when `this` is `other` or an ancestor of `other`.
  [[nodiscard]] bool is_ancestor_of(const ScopeIdentity& other) const noexcept;

  /// The parent scope, or nullopt at the root.
  [[nodiscard]] std::optional<ScopeIdentity> parent() const;

  friend bool operator==(const ScopeIdentity& a, const ScopeIdentity& b) noexcept {
    return a.segments_ == b.segments_;
  }
  friend bool operator!=(const ScopeIdentity& a, const ScopeIdentity& b) noexcept {
    return !(a == b);
  }
  /// Canonical total order: lexicographic over segments. Deliberately *not*
  /// the display order of the rendered path: ordering is defined over the
  /// segment vector so that ordering never depends on the separator choice.
  friend bool operator<(const ScopeIdentity& a, const ScopeIdentity& b) noexcept {
    return a.segments_ < b.segments_;
  }

 private:
  std::vector<ScopeSegment> segments_;
};

/// How a run maps requested scopes onto the cells it reconciles.
enum class ScopeSelectionMode : std::uint8_t {
  /// Only evidence whose scope is exactly the selected scope.
  Exact = 0,
  /// Evidence at the selected scope or any descendant scope is rolled up into
  /// the selected scope. Missing descendants make a rolled-up quantity
  /// Unknown(PartialRollup) rather than silently under-reporting.
  SubtreeRollup = 1,
};

[[nodiscard]] const char* to_string(ScopeSelectionMode mode) noexcept;
[[nodiscard]] std::optional<ScopeSelectionMode> scope_selection_mode_from_string(
    std::string_view token) noexcept;

/// One requested scope together with its selection mode.
struct ScopeSelection {
  ScopeIdentity scope;
  ScopeSelectionMode mode = ScopeSelectionMode::Exact;
};

}  // namespace summon::capacity_reconciliation
