// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// A small, self-contained test harness.
//
// There is no third-party test framework here on purpose: the runtime has no
// third-party dependency, and a dependency introduced only for tests would
// still have to be installed, pinned and audited by anyone building the
// project. What the suite needs is registration, expectations, a failure count
// and a non-zero exit code, which is thirty lines.
//
// There are no timeouts anywhere in this suite. A test that hangs is a defect
// to diagnose, not a test to kill.

#pragma once

#include <cstdint>
#include <type_traits>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "summon/capacity_reconciliation/engine.hpp"

namespace cr = summon::capacity_reconciliation;

namespace crtest {

using TestFunction = void (*)();

/// Registration hook. Constructed as a static object by CR_TEST.
class Registrar {
 public:
  Registrar(const char* suite, const char* name, TestFunction function);
};

/// Indirection around `==` so that comparing two compile-time constants does
/// not produce a "conditional expression is constant" warning inside the
/// assertion macro. Comparing constants is exactly what a contract test does,
/// so the macro must not be the reason a project cannot be warnings-clean.
template <typename A, typename B>
[[nodiscard]] bool equal(const A& a, const B& b) {
  return a == b;
}

/// Record a failure and continue the current test.
void record_failure(const char* file, int line, const std::string& message);

/// Record a failure and abort the current test immediately.
[[noreturn]] void abort_test(const char* file, int line, const std::string& message);

/// Total assertions evaluated (for the summary line).
void note_assertion();

/// Run every registered test. Returns a process exit code.
int run_all(int argc, char** argv);

/// Name of the test currently running.
[[nodiscard]] const std::string& current_test();

}  // namespace crtest

#define CR_TEST(suite, name)                                                          \
  static void crtest_##suite##_##name();                                              \
  static const ::crtest::Registrar crtest_reg_##suite##_##name(#suite, #name,         \
                                                               &crtest_##suite##_##name); \
  static void crtest_##suite##_##name()

#define CR_CHECK(condition)                                                       \
  do {                                                                            \
    ::crtest::note_assertion();                                                   \
    if (!(condition)) {                                                           \
      ::crtest::record_failure(__FILE__, __LINE__, "check failed: " #condition);  \
    }                                                                             \
  } while (0)

#define CR_CHECK_MSG(condition, message)                                          \
  do {                                                                            \
    ::crtest::note_assertion();                                                   \
    if (!(condition)) {                                                           \
      ::crtest::record_failure(__FILE__, __LINE__,                                \
                               std::string("check failed: " #condition " -- ") +  \
                                   (message));                                    \
    }                                                                             \
  } while (0)

#define CR_CHECK_EQ(actual, expected)                                             \
  do {                                                                            \
    ::crtest::note_assertion();                                                   \
    /* By value, not by reference: the right-hand side is frequently a */         \
    /* Result<T>::value() on a temporary, and binding a reference to it */        \
    /* would dangle as soon as the full expression ends. */                       \
    const auto crtest_a = (actual);                                               \
    const auto crtest_b = (expected);                                             \
    if (!::crtest::equal(crtest_a, crtest_b)) {                                   \
      ::crtest::record_failure(__FILE__, __LINE__,                                \
                               std::string("expected " #actual " == " #expected) + \
                                   " (got " + ::crtest::describe(crtest_a) +      \
                                   " vs " + ::crtest::describe(crtest_b) + ")");  \
    }                                                                             \
  } while (0)

#define CR_REQUIRE(condition)                                                     \
  do {                                                                            \
    ::crtest::note_assertion();                                                   \
    if (!(condition)) {                                                           \
      ::crtest::abort_test(__FILE__, __LINE__, "requirement failed: " #condition); \
    }                                                                             \
  } while (0)

#define CR_REQUIRE_OK(expression)                                                 \
  do {                                                                            \
    ::crtest::note_assertion();                                                   \
    auto&& crtest_result = (expression);                                          \
    if (!crtest_result.ok()) {                                                    \
      ::crtest::abort_test(__FILE__, __LINE__,                                    \
                           std::string(#expression " failed: ") +                 \
                               crtest_result.error().to_string());                \
    }                                                                             \
  } while (0)

#define CR_REQUIRE_ERR(expression, expected_code)                                 \
  do {                                                                            \
    ::crtest::note_assertion();                                                   \
    auto&& crtest_result = (expression);                                          \
    if (crtest_result.ok()) {                                                     \
      ::crtest::abort_test(__FILE__, __LINE__,                                    \
                           #expression " unexpectedly succeeded");                \
    }                                                                             \
    if (crtest_result.error().code != (expected_code)) {                          \
      ::crtest::abort_test(__FILE__, __LINE__,                                    \
                           std::string(#expression " returned ") +                \
                               ::summon::capacity_reconciliation::to_string(      \
                                   crtest_result.error().code) +                  \
                               ", expected " +                                    \
                               ::summon::capacity_reconciliation::to_string(      \
                                   expected_code) +                               \
                               " (" + crtest_result.error().message + ")");       \
    }                                                                             \
  } while (0)

namespace cr = summon::capacity_reconciliation;

namespace crtest {

/// Render a few common value types for failure messages.
///
/// The overload set is written so that no integral argument is ambiguous: a
/// single constrained template covers every integral width, and the non-template
/// overloads below win over it for the types whose text form is not their
/// numeric value.
template <typename T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
[[nodiscard]] std::string describe(T value) {
  return std::to_string(value);
}

[[nodiscard]] inline std::string describe(bool value) { return value ? "true" : "false"; }
[[nodiscard]] inline std::string describe(char value) { return std::string(1, value); }
[[nodiscard]] inline std::string describe(const std::string& value) { return value; }
[[nodiscard]] inline std::string describe(std::string_view value) {
  return std::string(value);
}
[[nodiscard]] inline std::string describe(const char* value) {
  return value == nullptr ? std::string("(null)") : std::string(value);
}

template <typename T, std::enable_if_t<!std::is_integral_v<T>, int> = 0>
[[nodiscard]] std::string describe(const T&) {
  return "<value>";
}

// Enumerations are rendered by their stable token, so a failing comparison says
// which classification disagreed rather than printing two opaque numbers.
[[nodiscard]] inline std::string describe(cr::DiscrepancyClass value) {
  return cr::to_string(value);
}
[[nodiscard]] inline std::string describe(cr::ResolutionState value) {
  return cr::to_string(value);
}
[[nodiscard]] inline std::string describe(cr::CellTransition value) {
  return cr::to_string(value);
}
[[nodiscard]] inline std::string describe(cr::ErrorCode value) { return cr::to_string(value); }
[[nodiscard]] inline std::string describe(cr::UnknownReason value) {
  return cr::to_string(value);
}
[[nodiscard]] inline std::string describe(cr::EvidenceSource value) { return cr::to_string(value); }
[[nodiscard]] inline std::string describe(cr::CapacityView value) { return cr::to_string(value); }
[[nodiscard]] inline std::string describe(cr::EvidenceStatus value) { return cr::to_string(value); }
[[nodiscard]] inline std::string describe(cr::ReasonTarget value) { return cr::to_string(value); }
[[nodiscard]] inline std::string describe(cr::Unit value) { return cr::to_string(value); }
[[nodiscard]] inline std::string describe(cr::RecoveryOutcome value) {
  return cr::to_string(value);
}

}  // namespace crtest
