# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Summon Software Labs.
#
# Capacity Reconciliation -- build, test and install.
#
# The runtime is portable C++20 with no third-party dependency. It uses the
# C++20 standard library and, on Windows, the operating system libraries it
# already has (bcrypt for entropy, kernel32 for files, locks and durability).

cmake_minimum_required(VERSION 3.20)

project(CapacityReconciliation
  VERSION 1.0.0
  DESCRIPTION "Deterministic reconciliation and discrepancy classification across facility capacity views"
  HOMEPAGE_URL "https://github.com/summonlabs/Capacity-Reconciliation"
  LANGUAGES CXX)

set(CAPACITY_RECONCILIATION_VERSION "${PROJECT_VERSION}")

option(CAPACITY_RECONCILIATION_BUILD_TOOLS "Build the command line tools" ON)
option(CAPACITY_RECONCILIATION_BUILD_TESTS "Build the test suites" ON)
option(CAPACITY_RECONCILIATION_BUILD_EXAMPLES "Build the examples" ON)
option(CAPACITY_RECONCILIATION_BUILD_BENCH "Build the benchmarks" ON)
option(CAPACITY_RECONCILIATION_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(CAPACITY_RECONCILIATION_ENABLE_SANITIZERS "Build with AddressSanitizer where supported" OFF)
option(CAPACITY_RECONCILIATION_BUILD_SHARED "Build the runtime as a shared library" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(CAPACITY_RECONCILIATION_BUILD_SHARED)
  set(CAPACITY_RECONCILIATION_LIBRARY_TYPE SHARED)
else()
  set(CAPACITY_RECONCILIATION_LIBRARY_TYPE STATIC)
endif()

set(CAPACITY_RECONCILIATION_SOURCES
  src/attribution.cpp
  src/canonical.cpp
  src/classify.cpp
  src/diff.cpp
  src/engine.cpp
  src/evidence.cpp
  src/explain.cpp
  src/hash.cpp
  src/identity.cpp
  src/journal.cpp
  src/platform.cpp
  src/policy.cpp
  src/reconcile.cpp
  src/report.cpp
  src/run.cpp
  src/scope.cpp
  src/status.cpp
  src/store.cpp
  src/units.cpp
  src/value.cpp
  src/version.cpp)

# ---- warnings ------------------------------------------------------------
# First-party code is warnings-clean at this level. Nothing is suppressed
# globally: a warning here is a defect to fix, not noise to silence.
if(MSVC)
  set(CAPACITY_RECONCILIATION_WARNINGS
    /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc /Zc:preprocessor
    /w14242 /w14254 /w14263 /w14265 /w14287 /w14296 /w14311 /w14545 /w14546 /w14547
    /w14549 /w14555 /w14619 /w14640 /w14826 /w14905 /w14906 /w14928)
  if(CAPACITY_RECONCILIATION_WARNINGS_AS_ERRORS)
    list(APPEND CAPACITY_RECONCILIATION_WARNINGS /WX)
  endif()
else()
  set(CAPACITY_RECONCILIATION_WARNINGS
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
  if(CAPACITY_RECONCILIATION_WARNINGS_AS_ERRORS)
    list(APPEND CAPACITY_RECONCILIATION_WARNINGS -Werror)
  endif()
endif()

add_compile_options(${CAPACITY_RECONCILIATION_WARNINGS})

if(CAPACITY_RECONCILIATION_ENABLE_SANITIZERS)
  if(MSVC)
    add_compile_options(/fsanitize=address)
    add_link_options(/fsanitize=address)
  else()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
  endif()
endif()
add_library(capacity_reconciliation ${CAPACITY_RECONCILIATION_LIBRARY_TYPE}
  ${CAPACITY_RECONCILIATION_SOURCES})

add_library(SummonLabs::CapacityReconciliation::capacity_reconciliation
  ALIAS capacity_reconciliation)

target_include_directories(capacity_reconciliation
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(capacity_reconciliation PUBLIC cxx_std_20)

if(CAPACITY_RECONCILIATION_BUILD_SHARED)
  target_compile_definitions(capacity_reconciliation
    PUBLIC CAPACITY_RECONCILIATION_SHARED
    PRIVATE CAPACITY_RECONCILIATION_BUILDING_LIBRARY)
endif()

find_package(Threads REQUIRED)
target_link_libraries(capacity_reconciliation PRIVATE Threads::Threads)

if(WIN32)
  # bcrypt: BCryptGenRandom, the platform CSPRNG used for identity and store
  # identity entropy.
  target_link_libraries(capacity_reconciliation PRIVATE bcrypt)
endif()


# ---- components ----------------------------------------------------------
if(CAPACITY_RECONCILIATION_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(CAPACITY_RECONCILIATION_BUILD_BENCH)
  add_subdirectory(bench)
endif()

if(CAPACITY_RECONCILIATION_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(CAPACITY_RECONCILIATION_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---- installation --------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS capacity_reconciliation
  EXPORT CapacityReconciliationTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT CapacityReconciliationTargets
  FILE CapacityReconciliationTargets.cmake
  NAMESPACE SummonLabs::CapacityReconciliation::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CapacityReconciliation)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CapacityReconciliationConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/CapacityReconciliationConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CapacityReconciliation)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/CapacityReconciliationConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/CapacityReconciliationConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/CapacityReconciliationConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CapacityReconciliation)

install(FILES README.md LICENSE NOTICE
  DESTINATION ${CMAKE_INSTALL_DATADIR}/CapacityReconciliation)
