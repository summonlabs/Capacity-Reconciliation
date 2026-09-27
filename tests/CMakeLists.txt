# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Summon Software Labs.

add_executable(capacity_reconciliation_tests
  test_main.cpp
  test_support.cpp
  test_helpers.cpp
  test_core.cpp
  test_evidence.cpp
  test_policy.cpp
  test_reconcile.cpp
  test_reference.cpp
  test_property.cpp
  test_diff.cpp
  test_persistence.cpp
  test_adversarial.cpp
  test_adversarial_hardening.cpp
  test_concurrency.cpp
  test_multiprocess.cpp
  test_scale.cpp
  test_render.cpp)

target_link_libraries(capacity_reconciliation_tests
  PRIVATE SummonLabs::CapacityReconciliation::capacity_reconciliation)

# The multiprocess suite launches the worker tool as an independent operating
# system process, so it must exist before the tests run.
if(TARGET capacity_reconciliation_worker)
  add_dependencies(capacity_reconciliation_tests capacity_reconciliation_worker)
endif()

# No TIMEOUT property is set anywhere. A test that hangs is a defect to
# diagnose, not a test to kill.
add_test(NAME capacity_reconciliation_tests
  COMMAND capacity_reconciliation_tests)

add_test(NAME example_basic_reconcile
  COMMAND example_basic_reconcile)
add_test(NAME example_rollup_and_conflict
  COMMAND example_rollup_and_conflict)
add_test(NAME example_durable_lifecycle
  COMMAND example_durable_lifecycle)

# tests/consumer is deliberately a separate project. Run it with
# cmake/PackageCheck.cmake after installing, so that it reads the installed
# package rather than this source tree.
