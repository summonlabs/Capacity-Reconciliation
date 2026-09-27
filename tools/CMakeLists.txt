# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Summon Software Labs.

add_executable(capacity_reconciliation_cli
  capacity_reconciliation_cli.cpp
  cli_support.cpp)

target_link_libraries(capacity_reconciliation_cli
  PRIVATE SummonLabs::CapacityReconciliation::capacity_reconciliation)

add_executable(capacity_reconciliation_worker
  capacity_reconciliation_worker.cpp
  cli_support.cpp)

target_link_libraries(capacity_reconciliation_worker
  PRIVATE SummonLabs::CapacityReconciliation::capacity_reconciliation)

install(TARGETS capacity_reconciliation_cli
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
