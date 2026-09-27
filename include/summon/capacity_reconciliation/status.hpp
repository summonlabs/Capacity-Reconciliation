// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Stable typed error model.
//
// Every fallible entry point in the runtime returns a Result<T> or a Status.
// The error codes below are a public contract: they are stable, machine
// readable and documented in the README. Callers must be able to distinguish
// "unknown" from "zero", "unsupported" from "unavailable", and "stale" from
// "conflict" without parsing text.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace summon::capacity_reconciliation {

/// Machine readable outcome code. Values are stable across releases; new codes
/// are appended, never renumbered.
enum class ErrorCode : std::uint16_t {
  Ok = 0,

  /// A caller-supplied argument violates a documented precondition.
  InvalidArgument = 1,

  /// The caller's precondition generation does not match the current one.
  /// Distinct from StaleAuthority: nothing about authority was disproved, the
  /// caller simply reasoned about an older state.
  StaleGeneration = 2,

  /// Authority that was valid has been superseded (epoch, incarnation, attempt
  /// or lease). Stale authority is refused, never merged.
  StaleAuthority = 3,

  /// Two authoritative inputs disagree and the policy cannot order them.
  Conflict = 4,

  /// The referenced object does not exist in this store or engine.
  NotFound = 5,

  /// The object already exists; identity reuse is refused.
  AlreadyExists = 6,

  /// Format / schema / store version is not the one this build understands.
  IncompatibleVersion = 7,

  /// Integrity verification failed: truncated, bit-rotted, torn or forged.
  Corruption = 8,

  /// A bounded resource declaration exceeds the configured limit. The limit is
  /// reported; nothing is allocated before the check.
  LimitExceeded = 9,

  /// The operation is defined but not implemented by this runtime, or the input
  /// names a concept outside the boundary.
  Unsupported = 10,

  /// A dependency the operation needs is currently not reachable or not
  /// running. Distinct from Unknown: absence of a value is not absence of a
  /// capability.
  Unavailable = 11,

  /// The value is genuinely indeterminate. Never conflated with zero.
  Unknown = 12,

  /// The operating system refused the operation (ACL, sharing, read-only).
  PermissionDenied = 13,

  /// An OS level I/O call failed. The OS error text is carried in the message.
  IoFailure = 14,

  /// Another writer holds the store lock.
  LockConflict = 15,

  /// An internal consistency rule was violated. Always a defect.
  InvariantViolation = 16,

  /// Checked arithmetic overflowed. Operating on the value is refused rather
  /// than wrapping.
  Overflow = 17,

  /// The bytes could not be parsed as the declared format.
  Malformed = 18,

  /// A path was rejected before use: traversal, absolute component, reserved
  /// device name, non-canonical form or over-long component.
  PathRejected = 19,
};

/// Stable, lowercase, underscore separated token for an error code. Suitable
/// for machine consumption and for JSON output.
[[nodiscard]] const char* to_string(ErrorCode code) noexcept;

/// Parse the token produced by to_string. Returns std::nullopt for unknown
/// input rather than guessing.
[[nodiscard]] std::optional<ErrorCode> error_code_from_string(std::string_view token) noexcept;

/// A typed error with the machine readable context a caller needs to decide.
struct Error {
  ErrorCode code = ErrorCode::Ok;
  std::string message;

  /// Present when the failure was caused by a generation precondition.
  std::optional<std::uint64_t> expected_generation;
  /// Present when the failure was caused by a generation precondition.
  std::optional<std::uint64_t> current_generation;

  /// Present when a specific constraint (limit name, invariant name, field
  /// name) was violated. Stable token.
  std::string constraint;

  Error() = default;

  Error(ErrorCode c, std::string msg) : code(c), message(std::move(msg)) {}

  Error(ErrorCode c, std::string msg, std::string constraint_token)
      : code(c), message(std::move(msg)), constraint(std::move(constraint_token)) {}

  [[nodiscard]] bool ok() const noexcept { return code == ErrorCode::Ok; }

  /// Render as a single stable line: "code: message [constraint=...]".
  [[nodiscard]] std::string to_string() const;
};

/// Convenience constructors used throughout the runtime.
[[nodiscard]] inline Error make_error(ErrorCode code, std::string message) {
  return Error{code, std::move(message)};
}

[[nodiscard]] inline Error make_error(ErrorCode code, std::string message, std::string constraint) {
  return Error{code, std::move(message), std::move(constraint)};
}

[[nodiscard]] inline Error generation_mismatch(std::uint64_t expected, std::uint64_t current) {
  Error e{ErrorCode::StaleGeneration,
          "generation precondition not satisfied: expected " + std::to_string(expected) +
              ", current " + std::to_string(current),
          "generation"};
  e.expected_generation = expected;
  e.current_generation = current;
  return e;
}

/// Result of an operation that produces no value.
class Status {
 public:
  Status() noexcept = default;
  Status(Error error) : error_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] static Status success() noexcept { return Status{}; }

  [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] const Error& error() const noexcept {
    static const Error kOk{};
    return error_ ? *error_ : kOk;
  }

  [[nodiscard]] ErrorCode code() const noexcept { return error_ ? error_->code : ErrorCode::Ok; }

 private:
  std::optional<Error> error_;
};

/// Result of an operation that produces a value.
///
/// `value()` is a precondition-checked accessor: calling it on an empty Result
/// is a programming defect and terminates with a diagnostic. Always test
/// `ok()` first, or use `value_or`.
template <typename T>
class Result {
  static_assert(!std::is_reference_v<T>, "Result<T&> is not supported; use Result<T*>");

 public:
  Result(T value) : storage_(std::move(value)) {}  // NOLINT(google-explicit-constructor)
  Result(Error error) : storage_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool ok() const noexcept { return std::holds_alternative<T>(storage_); }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] const Error& error() const noexcept {
    static const Error kOk{};
    const auto* e = std::get_if<Error>(&storage_);
    return e != nullptr ? *e : kOk;
  }

  [[nodiscard]] ErrorCode code() const noexcept { return error().code; }

  /// Precondition: `ok()`. Terminates otherwise.
  [[nodiscard]] T& value() & {
    T* v = std::get_if<T>(&storage_);
    if (v == nullptr) {
      fail_fast();
    }
    return *v;
  }

  /// Precondition: `ok()`. Terminates otherwise.
  [[nodiscard]] const T& value() const& {
    const T* v = std::get_if<T>(&storage_);
    if (v == nullptr) {
      fail_fast();
    }
    return *v;
  }

  /// Precondition: `ok()`. Terminates otherwise.
  [[nodiscard]] T&& value() && {
    T* v = std::get_if<T>(&storage_);
    if (v == nullptr) {
      fail_fast();
    }
    return std::move(*v);
  }

  [[nodiscard]] T* operator->() { return &value(); }
  [[nodiscard]] const T* operator->() const { return &value(); }
  [[nodiscard]] T& operator*() { return value(); }
  [[nodiscard]] const T& operator*() const { return value(); }

  [[nodiscard]] T value_or(T fallback) const {
    const T* v = std::get_if<T>(&storage_);
    return v != nullptr ? *v : std::move(fallback);
  }

  /// Access the error (if any) as a Status-compatible view.
  [[nodiscard]] Status status() const {
    return ok() ? Status::success() : Status{error()};
  }

 private:
  /// Reported and terminated in the header so that Result<T> works for types
  /// with internal linkage (kernel-local structs) as well as for public ones.
  [[noreturn]] void fail_fast() const {
    const Error& e = error();
    std::string text =
        "capacity_reconciliation: Result<T>::value() called on an error result: ";
    text += to_string(e.code);
    if (!e.message.empty()) {
      text += ": ";
      text += e.message;
    }
    text += '\n';
    std::fputs(text.c_str(), stderr);
    std::fflush(stderr);
    std::abort();
  }

  std::variant<T, Error> storage_;
};

/// Convert a Status into a Result of any type.
template <typename T>
[[nodiscard]] Result<T> status_as(Status status) {
  if (status.ok()) {
    return Error{ErrorCode::InvariantViolation,
                 "status_as<T> called on a successful status that carries no value",
                 "result"};
  }
  return status.error();
}

using Bytes = std::string;

}  // namespace summon::capacity_reconciliation
