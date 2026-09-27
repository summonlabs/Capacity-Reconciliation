// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Example 2: the durable lifecycle.
//
// open -> ingest -> reconcile -> commit -> close -> reopen -> revalidate.
//
// The reopen in this example is a real close-and-reopen of the store: the
// second handle re-reads the published commit point from disk, verifies it, and
// takes a fresh incarnation. Runs recovered this way are never treated as
// freshly observed, which is why the example ends by revalidating against a new
// evaluation instant rather than trusting the recovered answer.
//
// The store directory is created under the system temporary directory and is
// removed at the end, so running the example leaves nothing behind.

#include <chrono>
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

/// Removes a scratch tree on every exit path, including the error paths.
///
/// A demonstration that leaves its own state behind when it fails is a poor
/// demonstration, and the habit it teaches -- clean up only when everything
/// worked -- is the wrong one for a durable runtime.
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
  /// Remove the tree now. Used to report the outcome honestly rather than
  /// observing a directory that the destructor has not reached yet.
  void clear() const {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

 private:
  std::filesystem::path root_;
};

}  // namespace

int main() {
  const ScratchTree scratch(std::filesystem::temp_directory_path() /
                            ("capacity-reconciliation-example-" +
                             std::to_string(cr::platform::current_process_id()) + "-" +
                             std::to_string(cr::platform::monotonic_nanoseconds())));
  const auto& root = scratch.path();

  const auto domain = cr::ClockDomain::create("facility-monotonic").value();
  cr::EngineOptions options;
  options.clock_domain = domain;

  const auto scope = cr::ScopeIdentity::parse("site-b/hall-2/row-1").value();
  const auto space = cr::DimensionKey::create(cr::CapacityDimension::Space, cr::Unit::RackUnit).value();

  cr::ReconciliationRunId committed_run;

  {
    cr::StoreOpenOptions open_options;
    open_options.create_if_missing = true;
    open_options.writer = true;
    auto store = cr::Store::open(root, open_options);
    if (!store.ok()) {
      std::fprintf(stderr, "%s\n", store.error().to_string().c_str());
      return 1;
    }
    auto engine = cr::Engine::attach(std::move(store.value()), options);
    if (!engine.ok()) {
      std::fprintf(stderr, "%s\n", engine.error().to_string().c_str());
      return 1;
    }

    const cr::Generation generation = engine.value().generation();
    std::vector<cr::EvidenceItem> items;
    const struct {
      cr::CapacityView view;
      cr::Amount value;
    } rows[] = {{cr::CapacityView::Planned, 42},
                {cr::CapacityView::Installed, 42},
                {cr::CapacityView::Observed, 40},
                {cr::CapacityView::Usable, 40},
                {cr::CapacityView::Allocatable, 10}};
    for (const auto& row : rows) {
      cr::EvidenceStamp stamp;
      stamp.generation = generation;
      stamp.revision = cr::Revision{1};
      stamp.incarnation = engine.value().incarnation();
      stamp.observed_at = cr::Tick{500};
      stamp.valid_until = cr::Tick{1500};
      stamp.clock_domain = domain;
      auto item = cr::EvidenceItem::create(cr::EvidenceId::generate(),
                                           cr::EvidenceSource::RackRegistry, "rack-registry-1",
                                           scope, space, row.view, cr::Quantity::known(row.value),
                                           cr::EvidenceStatus::Accepted, stamp);
      if (!item.ok()) {
        std::fprintf(stderr, "%s\n", item.error().to_string().c_str());
        return 1;
      }
      items.push_back(std::move(item.value()));
    }
    auto status = engine.value().append_evidence(std::move(items), generation,
                                                engine.value().incarnation());
    if (!status.ok()) {
      std::fprintf(stderr, "%s\n", status.error().to_string().c_str());
      return 1;
    }

    cr::RunRequest request;
    cr::ScopeSelection selection;
    selection.scope = scope;
    request.scopes.push_back(selection);
    request.generation = engine.value().generation();
    request.evaluation_instant = cr::Tick{800};
    request.clock_domain = domain;
    request.label = "first_pass";

    auto run = engine.value().reconcile_and_commit(request, engine.value().generation(),
                                                   engine.value().incarnation());
    if (!run.ok()) {
      std::fprintf(stderr, "%s\n", run.error().to_string().c_str());
      return 1;
    }
    committed_run = run.value().id();
    std::printf("first pass : %s\n", cr::render_run_text(run.value()).c_str());
  }

  // The store handle is gone. Reopen it: this re-reads and re-verifies the
  // published commit point from disk and takes a fresh incarnation.
  {
    cr::StoreOpenOptions open_options;
    open_options.create_if_missing = false;
    open_options.writer = true;
    auto store = cr::Store::open(root, open_options);
    if (!store.ok()) {
      std::fprintf(stderr, "%s\n", store.error().to_string().c_str());
      return 1;
    }
    std::printf("recovered  : %s\n", cr::render_recovery_json(store.value().recovery()).c_str());

    auto engine = cr::Engine::attach(std::move(store.value()), options);
    if (!engine.ok()) {
      std::fprintf(stderr, "%s\n", engine.error().to_string().c_str());
      return 1;
    }
    auto recovered = engine.value().find_run(committed_run);
    if (!recovered.ok()) {
      std::fprintf(stderr, "%s\n", recovered.error().to_string().c_str());
      return 1;
    }
    std::printf("as recovered: %s\n", cr::render_run_text(recovered.value()).c_str());

    // Revalidate against current evidence at a later instant. This produces a
    // successor run and supersedes the recovered one; the recovered run's own
    // record is not rewritten.
    auto successor = engine.value().revalidate(committed_run, cr::Tick{1200},
                                               engine.value().generation(),
                                               engine.value().incarnation());
    if (!successor.ok()) {
      std::fprintf(stderr, "%s\n", successor.error().to_string().c_str());
      return 1;
    }
    std::printf("revalidated: %s\n", cr::render_run_text(successor.value()).c_str());

    auto diff = engine.value().diff(committed_run, successor.value().id());
    if (diff.ok()) {
      std::printf("diff       : %zu entries, %zu reclassified\n", diff.value().entries().size(),
                  diff.value().count(cr::CellTransition::Reclassified));
    }
  }

  // Offline verification, using the path a separate operator would use.
  const auto verification = cr::Store::verify(root, cr::default_limits());
  std::printf("verify     : %s\n", verification.ok ? "ok" : "failed");
  for (const std::string& problem : verification.problems) {
    std::printf("  problem  : %s\n", problem.c_str());
  }

  scratch.clear();
  std::printf("residue    : %s\n", scratch.exists() ? "PRESENT" : "removed");
  return verification.ok ? 0 : 1;
}
