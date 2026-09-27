# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Summon Software Labs.

add_executable(example_basic_reconcile basic_reconcile.cpp)
target_link_libraries(example_basic_reconcile
  PRIVATE SummonLabs::CapacityReconciliation::capacity_reconciliation)

add_executable(example_durable_lifecycle durable_lifecycle.cpp)
target_link_libraries(example_durable_lifecycle
  PRIVATE SummonLabs::CapacityReconciliation::capacity_reconciliation)

add_executable(example_rollup_and_conflict rollup_and_conflict.cpp)
target_link_libraries(example_rollup_and_conflict
  PRIVATE SummonLabs::CapacityReconciliation::capacity_reconciliation)
