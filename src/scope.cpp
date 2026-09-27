// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/capacity_reconciliation/scope.hpp"

#include "summon/capacity_reconciliation/limits.hpp"

namespace summon::capacity_reconciliation {
namespace {

struct ModeToken {
  ScopeSelectionMode mode;
  const char* token;
};

constexpr ModeToken kModeTokens[] = {
    {ScopeSelectionMode::Exact, "exact"},
    {ScopeSelectionMode::SubtreeRollup, "subtree_rollup"},
};

bool is_segment_char(char raw) noexcept {
  const auto c = static_cast<unsigned char>(raw);
  const bool alpha =
      (c >= static_cast<unsigned char>('a')) && (c <= static_cast<unsigned char>('z'));
  const bool digit =
      (c >= static_cast<unsigned char>('0')) && (c <= static_cast<unsigned char>('9'));
  const bool punct = c == static_cast<unsigned char>('_') || c == static_cast<unsigned char>('.') ||
                     c == static_cast<unsigned char>('-');
  return alpha || digit || punct;
}

}  // namespace

Result<ScopeSegment> ScopeSegment::create(std::string_view text) {
  if (text.empty()) {
    return make_error(ErrorCode::PathRejected, "scope segment must not be empty", "scope_segment");
  }
  if (text.size() > kMaxScopeSegmentBytes) {
    return make_error(ErrorCode::LimitExceeded,
                      "scope segment exceeds " + std::to_string(kMaxScopeSegmentBytes) + " bytes",
                      "scope_segment");
  }
  if (text == "." || text == "..") {
    return make_error(ErrorCode::PathRejected,
                      "'.' and '..' are not valid scope segments: a scope must not be able to "
                      "escape its tree",
                      "scope_segment");
  }
  if (text.front() == '.' || text.front() == '-') {
    return make_error(ErrorCode::PathRejected,
                      "scope segment must not begin with '.' or '-'", "scope_segment");
  }
  for (char c : text) {
    if (!is_segment_char(c)) {
      return make_error(ErrorCode::PathRejected,
                        "scope segment must match [a-z0-9._-]+ : '" + std::string(text) +
                            "' contains a rejected character",
                        "scope_segment");
    }
  }
  ScopeSegment segment;
  segment.value_.assign(text);
  return segment;
}

Result<ScopeIdentity> ScopeIdentity::parse(std::string_view path) {
  if (path.empty()) {
    return make_error(ErrorCode::InvalidArgument, "scope path must not be empty", "scope");
  }
  if (path.size() > kMaxScopeDepth * (kMaxScopeSegmentBytes + 1u)) {
    return make_error(ErrorCode::LimitExceeded, "scope path is longer than any legal scope",
                      "scope");
  }
  if (path.find('\\') != std::string_view::npos) {
    return make_error(ErrorCode::PathRejected,
                      "scope paths use '/' separators only; '\\' is rejected so that a scope "
                      "cannot be reinterpreted as a filesystem path",
                      "scope");
  }
  std::vector<ScopeSegment> segments;
  std::size_t start = 0;
  while (true) {
    const std::size_t slash = path.find('/', start);
    const std::string_view piece =
        (slash == std::string_view::npos) ? path.substr(start) : path.substr(start, slash - start);
    auto segment = ScopeSegment::create(piece);
    if (!segment.ok()) {
      return segment.error();
    }
    segments.push_back(segment.value());
    if (segments.size() > kMaxScopeDepth) {
      return make_error(ErrorCode::LimitExceeded,
                        "scope path is deeper than " + std::to_string(kMaxScopeDepth) +
                            " segments",
                        "scope");
    }
    if (slash == std::string_view::npos) {
      break;
    }
    start = slash + 1;
  }
  ScopeIdentity identity;
  identity.segments_ = std::move(segments);
  return identity;
}

Result<ScopeIdentity> ScopeIdentity::from_segments(const std::vector<ScopeSegment>& segments) {
  if (segments.empty()) {
    return make_error(ErrorCode::InvalidArgument, "scope must have at least one segment",
                      "scope");
  }
  if (segments.size() > kMaxScopeDepth) {
    return make_error(ErrorCode::LimitExceeded,
                      "scope is deeper than " + std::to_string(kMaxScopeDepth) + " segments",
                      "scope");
  }
  for (const ScopeSegment& segment : segments) {
    if (segment.empty()) {
      return make_error(ErrorCode::InvalidArgument, "scope contains an empty segment", "scope");
    }
  }
  ScopeIdentity identity;
  identity.segments_ = segments;
  return identity;
}

std::string ScopeIdentity::to_string() const {
  std::string out;
  for (std::size_t i = 0; i < segments_.size(); ++i) {
    if (i != 0) {
      out.push_back('/');
    }
    out.append(segments_[i].value());
  }
  return out;
}

bool ScopeIdentity::is_ancestor_of(const ScopeIdentity& other) const noexcept {
  if (segments_.size() > other.segments_.size()) {
    return false;
  }
  for (std::size_t i = 0; i < segments_.size(); ++i) {
    if (!(segments_[i] == other.segments_[i])) {
      return false;
    }
  }
  return true;
}

std::optional<ScopeIdentity> ScopeIdentity::parent() const {
  if (segments_.size() <= 1) {
    return std::nullopt;
  }
  ScopeIdentity out;
  out.segments_.assign(segments_.begin(), segments_.end() - 1);
  return out;
}

const char* to_string(ScopeSelectionMode mode) noexcept {
  for (const ModeToken& entry : kModeTokens) {
    if (entry.mode == mode) {
      return entry.token;
    }
  }
  return "unrecognised";
}

std::optional<ScopeSelectionMode> scope_selection_mode_from_string(
    std::string_view token) noexcept {
  for (const ModeToken& entry : kModeTokens) {
    if (token == entry.token) {
      return entry.mode;
    }
  }
  return std::nullopt;
}

}  // namespace summon::capacity_reconciliation
