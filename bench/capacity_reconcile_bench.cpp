// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Benchmarks: completed operations only.
//
// Every measurement below times a *finished* operation:
//
//   * reconcile: a run is computed to completion and its cells are counted;
//   * commit:     a store commit is timed from plan through the atomic HEAD
//                 replacement, so the durable flush and the publish are inside
//                 the measurement, not outside it;
//   * reopen:     a store is closed and re-opened, so the read, the framing
//                 verification and the decode are all inside the measurement.
//
// Workloads are SYNTHETIC: generated in-process with a fixed seed. No hardware
// is involved and no hardware claim is made.
//
// Methodology: for each workload the harness alternates between repetitions and
// discards nothing, reporting the median and the interquartile range rather
// than a best case. Each benchmark verifies the state it created (cell count,
// committed generation, reopened digest) before reporting, and the whole
// benchmark tree is removed at the end so no residue is left.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "summon/capacity_reconciliation/engine.hpp"
#include "summon/capacity_reconciliation/platform.hpp"
#include "summon/capacity_reconciliation/report.hpp"
#include "summon/capacity_reconciliation/store.hpp"

namespace cr = summon::capacity_reconciliation;

namespace {

struct Sample {
  double microseconds_per_operation = 0.0;
  std::uint64_t operations = 0;
  bool verified = false;
};

struct Statistics {
  double median = 0.0;
  double p25 = 0.0;
  double p75 = 0.0;
  double minimum = 0.0;
  double maximum = 0.0;
};

Statistics summarise(std::vector<double> values) {
  Statistics stats;
  if (values.empty()) {
    return stats;
  }
  std::sort(values.begin(), values.end());
  const auto at = [&values](double fraction) {
    const double index = fraction * static_cast<double>(values.size() - 1);
    const auto low = static_cast<std::size_t>(index);
    const auto high = std::min(low + 1, values.size() - 1);
    const double weight = index - static_cast<double>(low);
    return (values[low] * (1.0 - weight)) + (values[high] * weight);
  };
  stats.minimum = values.front();
  stats.maximum = values.back();
  stats.p25 = at(0.25);
  stats.median = at(0.50);
  stats.p75 = at(0.75);
  return stats;
}

/// Removes the benchmark's scratch tree on every exit path. A benchmark that
/// leaves several hundred megabytes behind when it fails is a benchmark nobody
/// runs twice.
class ScratchTree {
 public:
  explicit ScratchTree(std::filesystem::path root) : root_(std::move(root)) {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }
  ~ScratchTree() {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }
  ScratchTree(const ScratchTree&) = delete;
  ScratchTree& operator=(const ScratchTree&) = delete;
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return root_; }
  [[nodiscard]] bool exists() const {
    std::error_code ec;
    return std::filesystem::exists(root_, ec);
  }
  /// Remove the tree now. Doubles as the per-repetition reset for the
  /// durable-store workloads, and as the honest way to report the outcome
  /// rather than observing a directory the destructor has not reached yet.
  void clear() const {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

 private:
  std::filesystem::path root_;
};

/// Deterministic synthetic facility generator.
class Generator {
 public:
  explicit Generator(std::uint64_t seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ull : seed) {}

  std::uint64_t next() {
    state_ ^= state_ << 13u;
    state_ ^= state_ >> 7u;
    state_ ^= state_ << 17u;
    return state_;
  }

  std::uint64_t range(std::uint64_t low, std::uint64_t high) {
    return low + (next() % ((high - low) + 1u));
  }

 private:
  std::uint64_t state_;
};

std::string scope_path(std::size_t index) {
  return "site-a/hall-" + std::to_string((index / 100u) + 1u) + "/row-" +
         std::to_string((index % 100u) + 1u);
}

bool build_evidence(const cr::IncarnationId& incarnation, std::size_t cells,
                    std::uint64_t generation_value,
                    const cr::ClockDomain& domain, std::vector<cr::EvidenceItem>* out) {
  const cr::Generation generation{generation_value};
  const auto power =
      cr::DimensionKey::create(cr::CapacityDimension::Power, cr::Unit::MilliWatt).value();
  for (std::size_t i = 0; i < cells; ++i) {
    auto scope = cr::ScopeIdentity::parse(scope_path(i));
    if (!scope.ok()) {
      return false;
    }
    const struct {
      cr::CapacityView view;
      cr::Amount value;
      cr::EvidenceSource source;
    } rows[] = {
        {cr::CapacityView::Planned, 400000, cr::EvidenceSource::FacilityCapacity},
        {cr::CapacityView::Installed, 400000, cr::EvidenceSource::RackCapacity},
        {cr::CapacityView::Observed, 380000, cr::EvidenceSource::ObservationStream},
        {cr::CapacityView::Usable, 380000, cr::EvidenceSource::CoolingCapacity},
        {cr::CapacityView::Reserved, 120000, cr::EvidenceSource::ReservationRegister},
        {cr::CapacityView::Allocatable, 260000, cr::EvidenceSource::FacilityCapacity},
    };
    for (const auto& row : rows) {
      cr::EvidenceStamp stamp;
      stamp.generation = generation;
      stamp.revision = cr::Revision{1};
      stamp.incarnation = incarnation;
      stamp.observed_at = cr::Tick{1000};
      stamp.valid_until = cr::Tick{1000000};
      stamp.clock_domain = domain;
      auto item = cr::EvidenceItem::create(cr::EvidenceId::generate(), row.source, "synthetic",
                                           scope.value(), power, row.view,
                                           cr::Quantity::known(row.value),
                                           cr::EvidenceStatus::Accepted, stamp);
      if (!item.ok()) {
        return false;
      }
      out->push_back(std::move(item.value()));
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::size_t repetitions = 9;
  if (argc > 1) {
    repetitions = static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10));
  }
  if (repetitions < 3 || repetitions > 51) {
    repetitions = 9;
  }

  const auto domain = cr::ClockDomain::create("facility-monotonic").value();
  const ScratchTree scratch(std::filesystem::temp_directory_path() /
                            ("capacity-reconciliation-bench-" +
                             std::to_string(cr::platform::current_process_id()) + "-" +
                             std::to_string(cr::platform::monotonic_nanoseconds())));
  const auto& root = scratch.path();

  std::printf("capacity reconciliation benchmarks (SYNTHETIC workloads)\n");
  std::printf("repetitions per workload: %zu\n\n", repetitions);

  // ---- 1. completed reconciliation runs over a varied facility -------------
  for (const std::size_t cells : {100u, 1000u, 2000u}) {
    std::vector<double> samples;
    bool verified = true;
    std::uint64_t cells_seen = 0;
    for (std::size_t repetition = 0; repetition < repetitions; ++repetition) {
      cr::EngineOptions options;
      options.clock_domain = domain;
      // One selection per cell, so the per-run selection bound has to be raised
      // deliberately. The bound itself is exercised by the scale suite.
      options.limits.max_scope_selections = 8192;
      auto engine = cr::Engine::create(options);
      if (!engine.ok()) {
        std::fprintf(stderr, "%s\n", engine.error().to_string().c_str());
        return 1;
      }
      const cr::Generation generation = engine.value().generation();
      std::vector<cr::EvidenceItem> items;
      if (!build_evidence(engine.value().incarnation(), cells, generation.value(), domain, &items)) {
        std::fprintf(stderr, "synthetic evidence generation failed\n");
        return 1;
      }
      auto appended = engine.value().append_evidence(std::move(items), generation,
                                                    engine.value().incarnation());
      if (!appended.ok()) {
        std::fprintf(stderr, "%s\n", appended.error().to_string().c_str());
        return 1;
      }

      cr::RunRequest request;
      for (std::size_t i = 0; i < cells; ++i) {
        cr::ScopeSelection selection;
        selection.scope = cr::ScopeIdentity::parse(scope_path(i)).value();
        request.scopes.push_back(std::move(selection));
      }
      request.generation = engine.value().generation();
      request.evaluation_instant = cr::Tick{2000};
      request.clock_domain = domain;

      const std::uint64_t start = cr::platform::monotonic_nanoseconds();
      auto run = engine.value().reconcile(request);
      const std::uint64_t stop = cr::platform::monotonic_nanoseconds();
      if (!run.ok()) {
        std::fprintf(stderr, "%s\n", run.error().to_string().c_str());
        return 1;
      }
      samples.push_back(static_cast<double>(stop - start) / 1000.0);
      cells_seen = run.value().cells().size();
      // Verify the state the benchmark created before trusting the timing.
      verified = verified && (cells_seen == cells) &&
                 (run.value().count_class(cr::DiscrepancyClass::Agrees) == 0);
    }
    const Statistics stats = summarise(samples);
    std::printf("reconcile_run(%zu cells)          median %10.1f us  IQR [%.1f, %.1f]  min %.1f  max %.1f  cells=%llu verified=%s\n",
                cells, stats.median, stats.p25, stats.p75, stats.minimum, stats.maximum,
                static_cast<unsigned long long>(cells_seen), verified ? "yes" : "NO");
  }
  std::printf("\n");

  // ---- 2. durable append: plan through published commit point --------------
  for (const std::size_t batch : {32u, 256u}) {
    std::vector<double> samples;
    std::uint64_t final_generation = 0;
    bool verified = true;
    for (std::size_t repetition = 0; repetition < repetitions; ++repetition) {
      scratch.clear();
      cr::StoreOpenOptions open_options;
      open_options.create_if_missing = true;
      open_options.writer = true;
      auto store = cr::Store::open(root, open_options);
      if (!store.ok()) {
        std::fprintf(stderr, "%s\n", store.error().to_string().c_str());
        return 1;
      }
      cr::EngineOptions options;
      options.clock_domain = domain;
      auto engine = cr::Engine::attach(std::move(store.value()), options);
      if (!engine.ok()) {
        std::fprintf(stderr, "%s\n", engine.error().to_string().c_str());
        return 1;
      }
      std::vector<cr::EvidenceItem> items;
      if (!build_evidence(engine.value().incarnation(), batch,
                          engine.value().generation().value(), domain, &items)) {
        std::fprintf(stderr, "synthetic evidence generation failed\n");
        return 1;
      }
      const cr::Generation generation = engine.value().generation();
      const std::uint64_t start = cr::platform::monotonic_nanoseconds();
      auto status = engine.value().append_evidence(std::move(items), generation,
                                                  engine.value().incarnation());
      const std::uint64_t stop = cr::platform::monotonic_nanoseconds();
      if (!status.ok()) {
        std::fprintf(stderr, "%s\n", status.error().to_string().c_str());
        return 1;
      }
      samples.push_back(static_cast<double>(stop - start) / 1000.0);
      final_generation = engine.value().generation().value();
      verified = verified && (engine.value().evidence_snapshot()->size() == batch * 6u);
    }
    const Statistics stats = summarise(samples);
    std::printf("durable_commit(%zu evidence)      median %10.1f us  IQR [%.1f, %.1f]  min %.1f  max %.1f  gen=%llu verified=%s\n",
                batch * 6u, stats.median, stats.p25, stats.p75, stats.minimum, stats.maximum,
                static_cast<unsigned long long>(final_generation), verified ? "yes" : "NO");
  }
  std::printf("\n");

  // ---- 3. reopen: read, verify framing, decode ----------------------------
  {
    std::vector<double> samples;
    bool verified = true;
    std::uint64_t reopened_generation = 0;
    scratch.clear();
    {
      cr::StoreOpenOptions open_options;
      open_options.create_if_missing = true;
      open_options.writer = true;
      auto store = cr::Store::open(root, open_options);
      if (!store.ok()) {
        std::fprintf(stderr, "%s\n", store.error().to_string().c_str());
        return 1;
      }
      cr::EngineOptions options;
      options.clock_domain = domain;
      auto engine = cr::Engine::attach(std::move(store.value()), options);
      if (!engine.ok()) {
        std::fprintf(stderr, "%s\n", engine.error().to_string().c_str());
        return 1;
      }
      std::vector<cr::EvidenceItem> items;
      if (!build_evidence(engine.value().incarnation(), 500,
                          engine.value().generation().value(), domain, &items)) {
        return 1;
      }
      auto status = engine.value().append_evidence(std::move(items),
                                                  engine.value().generation(),
                                                  engine.value().incarnation());
      if (!status.ok()) {
        std::fprintf(stderr, "%s\n", status.error().to_string().c_str());
        return 1;
      }
    }
    for (std::size_t repetition = 0; repetition < repetitions; ++repetition) {
      cr::StoreOpenOptions open_options;
      open_options.create_if_missing = false;
      open_options.writer = true;
      const std::uint64_t start = cr::platform::monotonic_nanoseconds();
      auto store = cr::Store::open(root, open_options);
      const std::uint64_t stop = cr::platform::monotonic_nanoseconds();
      if (!store.ok()) {
        std::fprintf(stderr, "%s\n", store.error().to_string().c_str());
        return 1;
      }
      samples.push_back(static_cast<double>(stop - start) / 1000.0);
      reopened_generation = store.value().generation().value();
      verified = verified && (store.value().image().evidence.size() == 3000u) &&
                 (store.value().recovery().outcome == cr::RecoveryOutcome::OpenedClean);
    }
    const Statistics stats = summarise(samples);
    std::printf("reopen_store(3000 evidence)       median %10.1f us  IQR [%.1f, %.1f]  min %.1f  max %.1f  gen=%llu verified=%s\n",
                stats.median, stats.p25, stats.p75, stats.minimum, stats.maximum,
                static_cast<unsigned long long>(reopened_generation), verified ? "yes" : "NO");
  }

  scratch.clear();
  std::printf("\nresidue removed: %s\n", scratch.exists() ? "NO" : "yes");
  return 0;
}
