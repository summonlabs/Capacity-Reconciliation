// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "test_support.hpp"

#include <exception>
#include <string>
#include <vector>

namespace crtest {
namespace {

struct Entry {
  const char* suite;
  const char* name;
  TestFunction function;
};

/// Thrown only by abort_test, and caught only by run_all. The runtime itself
/// never throws; this is a harness-local control-flow device so that a failed
/// requirement stops the test body without running the rest of it.
struct AbortTest {};

std::vector<Entry>& registry() {
  static std::vector<Entry> entries;
  return entries;
}

std::size_t g_assertions = 0;
std::size_t g_failures_in_test = 0;
std::size_t g_failures_total = 0;
std::size_t g_tests_run = 0;
std::string g_current;

}  // namespace

Registrar::Registrar(const char* suite, const char* name, TestFunction function) {
  registry().push_back(Entry{suite, name, function});
}

void record_failure(const char* file, int line, const std::string& message) {
  ++g_failures_in_test;
  ++g_failures_total;
  std::fprintf(stderr, "  FAIL %s :: %s:%d\n    %s\n", g_current.c_str(), file, line,
               message.c_str());
  std::fflush(stderr);
}

void abort_test(const char* file, int line, const std::string& message) {
  record_failure(file, line, message + " [test aborted]");
  throw AbortTest{};
}

void note_assertion() { ++g_assertions; }

const std::string& current_test() { return g_current; }

int run_all(int argc, char** argv) {
  std::string filter;
  for (int i = 1; i < argc; ++i) {
    const std::string argument(argv[i]);
    if (argument.rfind("--filter=", 0) == 0) {
      filter = argument.substr(9);
    }
  }

  std::fprintf(stdout, "capacity reconciliation test suite\n");

  for (const Entry& entry : registry()) {
    const std::string full = std::string(entry.suite) + "." + entry.name;
    if (!filter.empty() && full.find(filter) == std::string::npos) {
      continue;
    }
    g_current = full;
    g_failures_in_test = 0;
    ++g_tests_run;

    try {
      entry.function();
    } catch (const AbortTest&) {
      // Already recorded.
    } catch (const std::exception& error) {
      record_failure(__FILE__, __LINE__,
                     std::string("unexpected exception: ") + error.what());
    } catch (...) {
      record_failure(__FILE__, __LINE__, "unexpected non-standard exception");
    }

    if (g_failures_in_test == 0) {
      std::fprintf(stdout, "  ok   %s\n", full.c_str());
    } else {
      std::fprintf(stdout, "  FAIL %s (%zu failures)\n", full.c_str(), g_failures_in_test);
    }
    std::fflush(stdout);
  }

  std::fprintf(stdout, "\n%zu tests, %zu assertions, %zu failures\n", g_tests_run, g_assertions,
               g_failures_total);
  std::fflush(stdout);
  return g_failures_total == 0 ? 0 : 1;
}

}  // namespace crtest
