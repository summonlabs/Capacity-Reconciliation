# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Summon Software Labs.
#
# Downstream consumer project.
#
# This is deliberately *not* part of the runtime's own build tree. It exists to
# prove that a third party who has only the installed package -- headers,
# library and CMake package files, and nothing else -- can configure, build,
# link and run against it. Wiring it into the same build tree would defeat the
# purpose, because it would then be reading the source tree instead of the
# installed artefacts.
#
# Configure it with:
#
#   cmake -S tests/consumer -B <build> \
#         -DCapacityReconciliation_DIR=<prefix>/lib/cmake/CapacityReconciliation

cmake_minimum_required(VERSION 3.20)

project(CapacityReconciliationConsumer
  VERSION 1.0.0
  DESCRIPTION "Out-of-tree consumer of the installed Capacity Reconciliation package"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CapacityReconciliation 1.0 CONFIG REQUIRED)

add_executable(capacity_reconciliation_consumer consumer.cpp)

# The namespaced target carries the include directories and the compile
# features, so a consumer needs nothing else.
target_link_libraries(capacity_reconciliation_consumer
  PRIVATE SummonLabs::CapacityReconciliation::capacity_reconciliation)

if(MSVC)
  target_compile_options(capacity_reconciliation_consumer
    PRIVATE /W4 /WX /permissive- /utf-8 /Zc:__cplusplus /EHsc /Zc:preprocessor)
endif()

enable_testing()
add_test(NAME consumer_lifecycle COMMAND capacity_reconciliation_consumer)
