// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// A capacity value that can be known or unknown.
//
// The single most important distinction in this runtime is between a quantity
// that is known to be zero and a quantity that is not known at all. Conflating
// them is how capacity systems silently overcommit. Quantity makes the
// distinction a type-level one: exactly one of a known Amount or an
// UnknownReason is present.

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "summon/capacity_reconciliation/quantity.hpp"
#include "summon/capacity_reconciliation/status.hpp"

namespace summon::capacity_reconciliation {

/// Why a quantity is not known. These map onto genuine operational situations
/// and are never used as a stand-in for zero.
enum class UnknownReason : std::uint8_t {
  None = 0,
  /// The producing runtime was asked and did not report this view.
  NotReported,
  /// The producing runtime could not be reached.
  SourceUnavailable,
  /// The producing runtime does not model this dimension or view.
  Unsupported,
  /// The value exists but the producer withheld it (policy / permission).
  Withheld,
  /// Sources disagree and policy cannot order them. Resolved only by new
  /// evidence or an explicit precedence rule.
  Conflicted,
  /// The value was superseded and no replacement is authoritative yet.
  Superseded,
  /// A rollup is missing at least one contributor, so the total is not known
  /// even though a partial sum exists.
  PartialRollup,
  /// The evidence existed but is past its validity window at evaluation time
  /// and the policy does not permit using stale values.
  Expired,
  /// The evidence generation is behind the run's minimum accepted generation.
  GenerationTooOld,
};

[[nodiscard]] const char* to_string(UnknownReason reason) noexcept;
[[nodiscard]] std::optional<UnknownReason> unknown_reason_from_string(
    std::string_view token) noexcept;

/// A capacity value: exactly known, or unknown with a reason.
class Quantity {
 public:
  /// Unknown with the given reason.
  [[nodiscard]] static Quantity unknown(UnknownReason reason) noexcept {
    Quantity q;
    q.reason_ = reason;
    return q;
  }

  /// Known exact value. Zero is a legitimate known value.
  [[nodiscard]] static Quantity known(Amount value) noexcept {
    Quantity q;
    q.known_ = value;
    return q;
  }

  [[nodiscard]] static Quantity not_reported() noexcept {
    return unknown(UnknownReason::NotReported);
  }

  [[nodiscard]] bool is_known() const noexcept { return known_.has_value(); }
  [[nodiscard]] bool is_unknown() const noexcept { return !known_.has_value(); }

  /// Precondition: is_known(). Terminates otherwise: reading a value you have
  /// not established is a defect, not a recoverable condition.
  [[nodiscard]] Amount value() const noexcept;

  /// The known value, or std::nullopt.
  [[nodiscard]] const std::optional<Amount>& known() const noexcept { return known_; }

  [[nodiscard]] UnknownReason reason() const noexcept { return reason_; }

  friend bool operator==(const Quantity& a, const Quantity& b) noexcept {
    return a.known_ == b.known_ && (a.known_.has_value() || a.reason_ == b.reason_);
  }
  friend bool operator!=(const Quantity& a, const Quantity& b) noexcept { return !(a == b); }

  /// Canonical token: either the decimal value or `unknown:<reason>`.
  [[nodiscard]] std::string to_string() const;

 private:
  std::optional<Amount> known_;
  UnknownReason reason_ = UnknownReason::NotReported;
};

/// Checked addition that propagates unknown-ness conservatively.
///
/// * both known             -> exact sum, overflow checked;
/// * one known, one unknown -> unknown, precedence-ordered reason, and the
///                             known operand is *not* silently treated as 0;
/// * both unknown           -> unknown.
[[nodiscard]] Result<Quantity> add_quantities(const Quantity& a, const Quantity& b,
                                              UnknownReason unknown_result) ;

/// Checked subtraction `a - b` with the same unknown propagation rules.
[[nodiscard]] Result<Quantity> sub_quantities(const Quantity& a, const Quantity& b,
                                              UnknownReason unknown_result);

/// The residual `a - b`, or an unknown of the given reason when either side is
/// unknown.
[[nodiscard]] Result<Quantity> residual(const Quantity& a, const Quantity& b,
                                        UnknownReason unknown_result);

/// Choose the more specific of two unknown reasons for a combined result.
/// Ordering is stable and documented: the first operand's reason wins unless it
/// is None.
[[nodiscard]] UnknownReason combine_unknown_reasons(UnknownReason a, UnknownReason b) noexcept;

/// Clamp a residual to zero for display-only purposes. Never used in
/// authoritative accounting: it is provided so that a renderer can show
/// "no shortfall" without mutating the underlying Quantity.
[[nodiscard]] Quantity clamp_positive(const Quantity& q) noexcept;

}  // namespace summon::capacity_reconciliation
