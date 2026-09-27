// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Checked arithmetic.
//
// Capacity accounting is done in exact signed 64-bit integers. Every operation
// that could overflow is checked, and overflow is reported as an error rather
// than wrapped. There is no floating point anywhere in authoritative
// accounting.

#pragma once

#include <cstdint>
#include <limits>
#include <type_traits>

#include "summon/capacity_reconciliation/status.hpp"

namespace summon::capacity_reconciliation {

/// Exact signed magnitude used for all capacity quantities and residuals.
using Amount = std::int64_t;

/// Exact unsigned magnitude used for counts, sizes and intervals.
using Count = std::uint64_t;

[[nodiscard]] constexpr Amount amount_min() noexcept {
  return std::numeric_limits<Amount>::min();
}
[[nodiscard]] constexpr Amount amount_max() noexcept {
  return std::numeric_limits<Amount>::max();
}

/// True when `value` fits exactly in Amount without loss.
template <typename T>
[[nodiscard]] constexpr bool fits_amount(T value) noexcept {
  if constexpr (std::is_signed_v<T>) {
    return static_cast<std::int64_t>(value) >= amount_min() &&
           static_cast<std::int64_t>(value) <= amount_max();
  } else {
    return static_cast<std::uint64_t>(value) <=
           static_cast<std::uint64_t>(amount_max());
  }
}

/// Exact narrowing of an unsigned value into Amount.
[[nodiscard]] inline Result<Amount> to_amount(std::uint64_t value) {
  if (value > static_cast<std::uint64_t>(amount_max())) {
    return make_error(ErrorCode::Overflow,
                      "unsigned value does not fit in the exact Amount domain",
                      "amount_range");
  }
  return static_cast<Amount>(value);
}

/// Checked addition.
[[nodiscard]] inline Result<Amount> checked_add(Amount a, Amount b) {
#if defined(__GNUC__) || defined(__clang__)
  Amount out = 0;
  if (__builtin_add_overflow(a, b, &out)) {
    return make_error(ErrorCode::Overflow, "capacity addition overflowed", "add");
  }
  return out;
#else
  if (b > 0 && a > amount_max() - b) {
    return make_error(ErrorCode::Overflow, "capacity addition overflowed", "add");
  }
  if (b < 0 && a < amount_min() - b) {
    return make_error(ErrorCode::Overflow, "capacity addition overflowed", "add");
  }
  return static_cast<Amount>(a + b);
#endif
}

/// Checked subtraction.
[[nodiscard]] inline Result<Amount> checked_sub(Amount a, Amount b) {
#if defined(__GNUC__) || defined(__clang__)
  Amount out = 0;
  if (__builtin_sub_overflow(a, b, &out)) {
    return make_error(ErrorCode::Overflow, "capacity subtraction overflowed", "sub");
  }
  return out;
#else
  if (b < 0 && a > amount_max() + b) {
    return make_error(ErrorCode::Overflow, "capacity subtraction overflowed", "sub");
  }
  if (b > 0 && a < amount_min() + b) {
    return make_error(ErrorCode::Overflow, "capacity subtraction overflowed", "sub");
  }
  return static_cast<Amount>(a - b);
#endif
}

/// Checked negation. Negating the minimum value is refused.
[[nodiscard]] inline Result<Amount> checked_neg(Amount a) {
  if (a == amount_min()) {
    return make_error(ErrorCode::Overflow, "capacity negation overflowed", "neg");
  }
  return -a;
}

/// Exact unsigned magnitude of a signed value. The minimum value is refused
/// because its magnitude is not representable in the same width.
[[nodiscard]] inline Result<std::uint64_t> checked_magnitude(Amount a) {
  if (a == amount_min()) {
    return make_error(ErrorCode::Overflow,
                      "the magnitude of the minimum capacity value is not representable",
                      "magnitude");
  }
  if (a < 0) {
    return static_cast<std::uint64_t>(-a);
  }
  return static_cast<std::uint64_t>(a);
}

/// Checked multiplication. Implemented over unsigned magnitudes so that the
/// check is exact and never relies on signed overflow behaviour.
[[nodiscard]] inline Result<Amount> checked_mul(Amount a, Amount b) {
  // Handle the cases whose result is representable regardless of the width of
  // the operands *first*. In particular `x * 1` must succeed for every x,
  // including the minimum value, whose magnitude is not representable -- the
  // manual path below would otherwise report a spurious overflow.
  if (a == 0 || b == 0) {
    return static_cast<Amount>(0);
  }
  if (a == 1) {
    return b;
  }
  if (b == 1) {
    return a;
  }
#if defined(__GNUC__) || defined(__clang__)
  Amount out = 0;
  if (__builtin_mul_overflow(a, b, &out)) {
    return make_error(ErrorCode::Overflow, "capacity multiplication overflowed", "mul");
  }
  return out;
#else
  if (a == static_cast<Amount>(-1)) {
    return checked_neg(b);
  }
  if (b == static_cast<Amount>(-1)) {
    return checked_neg(a);
  }
  auto magnitude_a = checked_magnitude(a);
  if (!magnitude_a.ok()) {
    return make_error(ErrorCode::Overflow, "capacity multiplication overflowed", "mul");
  }
  auto magnitude_b = checked_magnitude(b);
  if (!magnitude_b.ok()) {
    return make_error(ErrorCode::Overflow, "capacity multiplication overflowed", "mul");
  }
  if (magnitude_a.value() > std::numeric_limits<std::uint64_t>::max() / magnitude_b.value()) {
    return make_error(ErrorCode::Overflow, "capacity multiplication overflowed", "mul");
  }
  const std::uint64_t product = magnitude_a.value() * magnitude_b.value();
  const std::uint64_t positive_limit = static_cast<std::uint64_t>(amount_max());
  const std::uint64_t negative_limit = positive_limit + 1u;
  if ((a < 0) != (b < 0)) {
    if (product > negative_limit) {
      return make_error(ErrorCode::Overflow, "capacity multiplication overflowed", "mul");
    }
    if (product == negative_limit) {
      return amount_min();
    }
    return -static_cast<Amount>(product);
  }
  if (product > positive_limit) {
    return make_error(ErrorCode::Overflow, "capacity multiplication overflowed", "mul");
  }
  return static_cast<Amount>(product);
#endif
}

/// Checked absolute value.
[[nodiscard]] inline Result<Amount> checked_abs(Amount a) {
  if (a == amount_min()) {
    return make_error(ErrorCode::Overflow, "capacity absolute value overflowed", "abs");
  }
  return a < 0 ? -a : a;
}

/// Checked increment of an unsigned counter. Refuses to wrap.
[[nodiscard]] inline Result<std::uint64_t> checked_increment(std::uint64_t value) {
  if (value == std::numeric_limits<std::uint64_t>::max()) {
    return make_error(ErrorCode::Overflow, "counter increment overflowed", "increment");
  }
  return value + 1;
}

/// Checked addition of two unsigned counters.
[[nodiscard]] inline Result<std::uint64_t> checked_add_u64(std::uint64_t a, std::uint64_t b) {
  if (a > std::numeric_limits<std::uint64_t>::max() - b) {
    return make_error(ErrorCode::Overflow, "counter addition overflowed", "add");
  }
  return a + b;
}

/// Checked multiplication of two unsigned counters.
[[nodiscard]] inline Result<std::uint64_t> checked_mul_u64(std::uint64_t a, std::uint64_t b) {
  if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) {
    return make_error(ErrorCode::Overflow, "counter multiplication overflowed", "mul");
  }
  return a * b;
}

/// Exact conversion of a size into Amount, rejecting values that do not fit.
[[nodiscard]] inline Result<Amount> size_to_amount(std::size_t value) {
  return to_amount(static_cast<std::uint64_t>(value));
}

}  // namespace summon::capacity_reconciliation
