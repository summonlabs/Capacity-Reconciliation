// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Capacity Reconciliation -- foundational value types.
//
// This header intentionally depends only on the C++20 standard library.

#pragma once

#include <cstddef>
#include <cstdint>

/// Portable C++20 runtime for facility capacity reconciliation across the
/// planned / reserved / installed / observed / usable / allocatable views.
namespace summon::capacity_reconciliation {

/// Semantic version of the runtime, mirrored by the CMake package version.
inline constexpr int kVersionMajor = 1;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 0;

/// Human readable version string, e.g. "1.0.0".
[[nodiscard]] const char* version_string() noexcept;

/// On-disk store format version. Bumping this refuses older stores instead of
/// guessing at their meaning.
inline constexpr std::uint32_t kStoreFormatVersion = 1;

/// On-disk record framing version.
inline constexpr std::uint32_t kRecordFormatVersion = 1;

/// Canonical serialization schema version for engines / explanation digests.
inline constexpr std::uint32_t kCanonicalSchemaVersion = 1;

}  // namespace summon::capacity_reconciliation
