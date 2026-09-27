# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Summon Software Labs.

add_executable(capacity_reconcile_bench capacity_reconcile_bench.cpp)
target_link_libraries(capacity_reconcile_bench
  PRIVATE SummonLabs::CapacityReconciliation::capacity_reconciliation)
