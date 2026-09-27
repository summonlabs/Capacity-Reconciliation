// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/capacity_reconciliation/status.hpp"

#include <array>
#include <cstdlib>

namespace summon::capacity_reconciliation {
namespace {

struct CodeToken {
  ErrorCode code;
  const char* token;
};

// The token table is the public contract. Tokens are lowercase and stable; new
// codes are appended.
constexpr std::array<CodeToken, 20> kTokens{{
    {ErrorCode::Ok, "ok"},
    {ErrorCode::InvalidArgument, "invalid_argument"},
    {ErrorCode::StaleGeneration, "stale_generation"},
    {ErrorCode::StaleAuthority, "stale_authority"},
    {ErrorCode::Conflict, "conflict"},
    {ErrorCode::NotFound, "not_found"},
    {ErrorCode::AlreadyExists, "already_exists"},
    {ErrorCode::IncompatibleVersion, "incompatible_version"},
    {ErrorCode::Corruption, "corruption"},
    {ErrorCode::LimitExceeded, "limit_exceeded"},
    {ErrorCode::Unsupported, "unsupported"},
    {ErrorCode::Unavailable, "unavailable"},
    {ErrorCode::Unknown, "unknown"},
    {ErrorCode::PermissionDenied, "permission_denied"},
    {ErrorCode::IoFailure, "io_failure"},
    {ErrorCode::LockConflict, "lock_conflict"},
    {ErrorCode::InvariantViolation, "invariant_violation"},
    {ErrorCode::Overflow, "overflow"},
    {ErrorCode::Malformed, "malformed"},
    {ErrorCode::PathRejected, "path_rejected"},
}};

}  // namespace

const char* to_string(ErrorCode code) noexcept {
  for (const CodeToken& entry : kTokens) {
    if (entry.code == code) {
      return entry.token;
    }
  }
  return "unrecognised";
}

std::optional<ErrorCode> error_code_from_string(std::string_view token) noexcept {
  for (const CodeToken& entry : kTokens) {
    if (token == entry.token) {
      return entry.code;
    }
  }
  return std::nullopt;
}

std::string Error::to_string() const {
  std::string out = summon::capacity_reconciliation::to_string(code);
  out += ": ";
  out += message;
  if (!constraint.empty()) {
    out += " [constraint=";
    out += constraint;
    out += "]";
  }
  if (expected_generation.has_value() || current_generation.has_value()) {
    out += " [expected=";
    out += expected_generation.has_value() ? std::to_string(*expected_generation) : "n/a";
    out += " current=";
    out += current_generation.has_value() ? std::to_string(*current_generation) : "n/a";
    out += "]";
  }
  return out;
}

}  // namespace summon::capacity_reconciliation
