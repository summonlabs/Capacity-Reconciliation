// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Entry point for the whole suite.
//
// On Windows the process error mode is set before any test runs so that a
// defect can never surface as an interactive dialog: a crash dialog would both
// hide the defect and violate the rule that no C++ popup is ever left open.

#include "test_support.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

int main(int argc, char** argv) {
#if defined(_WIN32)
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  _set_abort_behavior(0, 0);
#endif
  return crtest::run_all(argc, argv);
}
