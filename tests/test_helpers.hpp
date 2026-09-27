// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Test-only helpers: synthetic evidence builders and a scratch directory that
// removes itself.
//
// Everything built here is SYNTHETIC. No helper in this header contacts
// hardware, a network or another runtime.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "summon/capacity_reconciliation/engine.hpp"
#include "summon/capacity_reconciliation/platform.hpp"
#include "summon/capacity_reconciliation/store.hpp"
#include "test_support.hpp"

namespace crtest {

namespace cr = summon::capacity_reconciliation;

/// A directory under the system temporary directory that is removed when it
/// goes out of scope, including on the failure path.
class ScratchDirectory {
 public:
  explicit ScratchDirectory(const std::string& label);
  ~ScratchDirectory();
  ScratchDirectory(const ScratchDirectory&) = delete;
  ScratchDirectory& operator=(const ScratchDirectory&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
  [[nodiscard]] std::filesystem::path child(const std::string& name) const {
    return path_ / name;
  }
  /// Remove the directory now. Idempotent.
  void reset();
  [[nodiscard]] bool exists() const;

 private:
  std::filesystem::path path_;
};

/// Default clock domain used by the helpers.
[[nodiscard]] const cr::ClockDomain& test_domain();

/// A builder for evidence items with sane, explicit defaults.
class EvidenceBuilder {
 public:
  EvidenceBuilder(std::string scope, cr::CapacityDimension dimension, cr::Unit unit);

  EvidenceBuilder& source(cr::EvidenceSource value) {
    source_ = value;
    return *this;
  }
  EvidenceBuilder& instance(std::string value) {
    instance_ = std::move(value);
    return *this;
  }
  EvidenceBuilder& generation(std::uint64_t value) {
    generation_ = cr::Generation{value};
    return *this;
  }
  EvidenceBuilder& revision(std::uint64_t value) {
    revision_ = cr::Revision{value};
    return *this;
  }
  EvidenceBuilder& observed_at(std::uint64_t value) {
    observed_at_ = cr::Tick{value};
    return *this;
  }
  EvidenceBuilder& valid_until(std::uint64_t value) {
    valid_until_ = cr::Tick{value};
    return *this;
  }
  EvidenceBuilder& status(cr::EvidenceStatus value) {
    status_ = value;
    return *this;
  }
  EvidenceBuilder& incarnation(const cr::IncarnationId& value) {
    incarnation_ = value;
    return *this;
  }

  [[nodiscard]] cr::Result<cr::EvidenceItem> known(cr::CapacityView view, cr::Amount value) const;
  [[nodiscard]] cr::Result<cr::EvidenceItem> unknown(cr::CapacityView view,
                                                     cr::UnknownReason reason) const;

 private:
  [[nodiscard]] cr::Result<cr::EvidenceItem> build(cr::CapacityView view,
                                                   cr::Quantity quantity) const;

  std::string scope_text_;
  cr::CapacityDimension dimension_ = cr::CapacityDimension::Power;
  cr::Unit unit_ = cr::Unit::MilliWatt;
  cr::EvidenceSource source_ = cr::EvidenceSource::FacilityCapacity;
  std::string instance_ = "synthetic";
  cr::Generation generation_{0};
  cr::Revision revision_{1};
  cr::Tick observed_at_{100};
  cr::Tick valid_until_{10000};
  cr::EvidenceStatus status_ = cr::EvidenceStatus::Accepted;
  cr::IncarnationId incarnation_ = cr::IncarnationId::generate();
};

/// Build an attribution item.
[[nodiscard]] cr::Result<cr::AttributionItem> make_attribution(
    const std::string& scope, cr::CapacityDimension dimension, cr::Unit unit,
    cr::ResidualKind target, cr::Amount amount, cr::EvidenceSource source,
    const std::string& instance, std::uint64_t generation, std::uint64_t revision,
    std::uint64_t observed_at, std::uint64_t valid_until,
    cr::AttributionReason reason = cr::AttributionReason::DeclaredDerate);

/// Store open options for an exclusive writer.
[[nodiscard]] cr::StoreOpenOptions writer_options(bool create_if_missing);

/// Store open options for a read-only inspection handle: no writer lock, no
/// creation, no mutation.
[[nodiscard]] cr::StoreOpenOptions reader_options();

/// Convenience: a RunRequest with one exact scope.
[[nodiscard]] cr::RunRequest exact_request(const std::string& scope, std::uint64_t generation,
                                           std::uint64_t instant);

/// In-memory engine with the standard policy and the test clock domain.
[[nodiscard]] cr::Result<cr::Engine> make_engine();

/// Power dimension key, milliwatts.
[[nodiscard]] cr::DimensionKey power_key();
/// Space dimension key, rack units.
[[nodiscard]] cr::DimensionKey space_key();

/// True when a JSON document is syntactically well formed: balanced
/// delimiters, only string-valued keys, no trailing commas. A deliberately
/// small validator -- enough to catch the emitter mistakes that matter.
[[nodiscard]] bool json_is_well_formed(std::string_view text);

/// Value of a top-level-or-nested string member, or empty when absent. Only
/// for assertions; never used to drive product behaviour.
[[nodiscard]] std::string json_find_string(std::string_view text, std::string_view key);

}  // namespace crtest
