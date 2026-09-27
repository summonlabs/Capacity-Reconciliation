# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Summon Software Labs.
#
# Packaging check, run as a CMake script:
#
#   cmake -DSOURCE_DIR=<repo> -DWORK_DIR=<scratch> -P cmake/PackageCheck.cmake
#
# CMAKE_COMMAND is predefined by CMake in script mode, so it does not need to be
# supplied and cannot be mistyped.
#
# It performs the whole installation proof end to end:
#
#   1. configure and build the runtime in Release;
#   2. run its test suite;
#   3. install it into a scratch prefix;
#   4. configure the *separate* consumer project against that prefix with
#      find_package, so it sees only installed headers, library and package
#      files;
#   5. build and run the consumer through a real lifecycle;
#   6. remove the scratch tree, leaving nothing behind.
#
# Every step fails loudly. Nothing is retried and nothing is timed out.

cmake_minimum_required(VERSION 3.20)

foreach(required SOURCE_DIR WORK_DIR)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "PackageCheck requires -D${required}=...")
  endif()
endforeach()

if(NOT DEFINED GENERATOR)
  set(GENERATOR "Ninja")
endif()

set(build_dir "${WORK_DIR}/build")
set(install_dir "${WORK_DIR}/install")
set(consumer_build "${WORK_DIR}/consumer-build")

function(run_step description)
  execute_process(
    COMMAND ${ARGN}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "${description} failed (${result})\n${output}\n${error}")
  endif()
  message(STATUS "${description}: ok")
endfunction()

message(STATUS "packaging check: configure")
run_step("configure"
  "${CMAKE_COMMAND}" -S "${SOURCE_DIR}" -B "${build_dir}" -G "${GENERATOR}"
  "-DCMAKE_BUILD_TYPE=Release"
  "-DCMAKE_INSTALL_PREFIX=${install_dir}")

message(STATUS "packaging check: build")
run_step("build" "${CMAKE_COMMAND}" --build "${build_dir}")

message(STATUS "packaging check: test")
run_step("test" "${CMAKE_COMMAND}" --build "${build_dir}" --target test)

message(STATUS "packaging check: install")
run_step("install" "${CMAKE_COMMAND}" --install "${build_dir}")

message(STATUS "packaging check: configure downstream consumer")
run_step("consumer configure"
  "${CMAKE_COMMAND}" -S "${SOURCE_DIR}/tests/consumer" -B "${consumer_build}"
  -G "${GENERATOR}"
  "-DCMAKE_BUILD_TYPE=Release"
  "-DCapacityReconciliation_DIR=${install_dir}/lib/cmake/CapacityReconciliation")

message(STATUS "packaging check: build downstream consumer")
run_step("consumer build" "${CMAKE_COMMAND}" --build "${consumer_build}")

message(STATUS "packaging check: run downstream consumer")
execute_process(
  COMMAND "${consumer_build}/capacity_reconciliation_consumer"
  RESULT_VARIABLE consumer_result
  OUTPUT_VARIABLE consumer_output
  ERROR_VARIABLE consumer_error)
if(NOT consumer_result EQUAL 0)
  message(FATAL_ERROR "consumer run failed (${consumer_result})\n${consumer_output}\n${consumer_error}")
endif()
message(STATUS "packaging check: consumer output:\n${consumer_output}")

message(STATUS "packaging check: remove scratch tree")
file(REMOVE_RECURSE "${WORK_DIR}")

message(STATUS "packaging check: ok")
