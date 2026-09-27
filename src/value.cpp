// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/capacity_reconciliation/value.hpp"

#include <cstdlib>
#include <string>

namespace summon::capacity_reconciliation {
namespace {

struct ReasonToken {
  UnknownReason reason;
  const char* token;
};

constexpr ReasonToken kReasonTokens[] = {
    {UnknownReason::None, "none"},
    {UnknownReason::NotReported, "not_reported"},
    {UnknownReason::SourceUnavailable, "source_unavailable"},
    {UnknownReason::Unsupported, "unsupported"},
    {UnknownReason::Withheld, "withheld"},
    {UnknownReason::Conflicted, "conflicted"},
    {UnknownReason::Superseded, "superseded"},
    {UnknownReason::PartialRollup, "partial_rollup"},
    {UnknownReason::Expired, "expired"},
    {UnknownReason::GenerationTooOld, "generation_too_old"},
};

}  // namespace

const char* to_string(UnknownReason reason) noexcept {
  for (const ReasonToken& entry : kReasonTokens) {
    if (entry.reason == reason) {
      return entry.token;
    }
  }
  return "unrecognised";
}

std::optional<UnknownReason> unknown_reason_from_string(std::string_view token) noexcept {
  for (const ReasonToken& entry : kReasonTokens) {
    if (token == entry.token) {
      return entry.reason;
    }
  }
  return std::nullopt;
}

Amount Quantity::value() const noexcept {
  if (!known_.has_value()) {
    // Reading a quantity that was never established is a defect: the whole
    // point of this type is that the caller must decide what to do about an
    // unknown. Terminating is preferable to silently returning zero.
    std::fputs(
        "capacity_reconciliation: Quantity::value() called on an unknown quantity; test is_known() "
        "first.\n",
        stderr);
    std::fflush(stderr);
    std::abort();
  }
  return *known_;
}

std::string Quantity::to_string() const {
  if (known_.has_value()) {
    return std::to_string(*known_);
  }
  std::string out = "unknown:";
  out += summon::capacity_reconciliation::to_string(reason_);
  return out;
}

UnknownReason combine_unknown_reasons(UnknownReason a, UnknownReason b) noexcept {
  if (a != UnknownReason::None) {
    return a;
  }
  return b;
}

Result<Quantity> add_quantities(const Quantity& a, const Quantity& b,
                                UnknownReason unknown_result) {
  if (a.is_known() && b.is_known()) {
    auto sum = checked_add(a.value(), b.value());
    if (!sum.ok()) {
      return sum.error();
    }
    return Quantity::known(sum.value());
  }
  UnknownReason reason = UnknownReason::None;
  if (a.is_unknown()) {
    reason = combine_unknown_reasons(reason, a.reason());
  }
  if (b.is_unknown()) {
    reason = combine_unknown_reasons(reason, b.reason());
  }
  if (reason == UnknownReason::None) {
    reason = unknown_result;
  }
  return Quantity::unknown(reason);
}

Result<Quantity> sub_quantities(const Quantity& a, const Quantity& b,
                                UnknownReason unknown_result) {
  if (a.is_known() && b.is_known()) {
    auto difference = checked_sub(a.value(), b.value());
    if (!difference.ok()) {
      return difference.error();
    }
    return Quantity::known(difference.value());
  }
  UnknownReason reason = UnknownReason::None;
  if (a.is_unknown()) {
    reason = combine_unknown_reasons(reason, a.reason());
  }
  if (b.is_unknown()) {
    reason = combine_unknown_reasons(reason, b.reason());
  }
  if (reason == UnknownReason::None) {
    reason = unknown_result;
  }
  return Quantity::unknown(reason);
}

Result<Quantity> residual(const Quantity& a, const Quantity& b, UnknownReason unknown_result) {
  return sub_quantities(a, b, unknown_result);
}

Quantity clamp_positive(const Quantity& q) noexcept {
  if (q.is_unknown()) {
    return q;
  }
  if (q.value() < 0) {
    return Quantity::known(0);
  }
  return q;
}

}  // namespace summon::capacity_reconciliation
