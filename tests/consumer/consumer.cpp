// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// A minimal downstream lifecycle over the installed package.
//
// This program is written as if its author had never seen the runtime's source
// tree: it includes only public headers, links only the namespaced target, and
// drives a complete real lifecycle --
//
//   create store -> ingest evidence -> reconcile -> commit -> close
//   -> reopen -> verify -> read the committed run back -> remove the store
//
// and exits non-zero if any step does not behave as documented. It is the
// packaging check: if the exported target, the installed headers or the
// installed library are wrong, this program fails to build or fails to run.

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "summon/capacity_reconciliation/engine.hpp"
#include "summon/capacity_reconciliation/platform.hpp"
#include "summon/capacity_reconciliation/report.hpp"
#include "summon/capacity_reconciliation/store.hpp"
#include "summon/capacity_reconciliation/version.hpp"

namespace cr = summon::capacity_reconciliation;

namespace {

int fail(const std::string& message) {
  std::fprintf(stderr, "consumer: %s\n", message.c_str());
  return 1;
}

/// Removes the scratch store on every exit path, so a failing run leaves the
/// machine exactly as it found it.
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

 private:
  std::filesystem::path root_;
};

}  // namespace

int main() {
  std::printf("consumer: runtime version %s\n", cr::version_string());
  if (std::string(cr::version_string()) != "1.0.0") {
    return fail("unexpected version string");
  }

  const auto domain = cr::ClockDomain::create("consumer-monotonic");
  if (!domain.ok()) {
    return fail("clock domain: " + domain.error().message);
  }
  const auto scope = cr::ScopeIdentity::parse("site-consumer/hall-1/row-1");
  if (!scope.ok()) {
    return fail("scope: " + scope.error().message);
  }
  const auto dimensions =
      cr::DimensionKey::create(cr::CapacityDimension::Power, cr::Unit::MilliWatt);
  if (!dimensions.ok()) {
    return fail("dimension: " + dimensions.error().message);
  }

  const ScratchTree scratch(std::filesystem::temp_directory_path() /
                            ("capacity-reconciliation-consumer-" +
                             std::to_string(cr::platform::current_process_id())));
  const auto& root = scratch.path();

  cr::ReconciliationRunId committed;
  cr::Digest committed_digest;

  {
    cr::StoreOpenOptions open_options;
    open_options.create_if_missing = true;
    open_options.writer = true;
    auto store = cr::Store::open(root, open_options);
    if (!store.ok()) {
      return fail("open store: " + store.error().to_string());
    }

    cr::EngineOptions options;
    options.clock_domain = domain.value();
    auto engine = cr::Engine::attach(std::move(store.value()), options);
    if (!engine.ok()) {
      return fail("attach engine: " + engine.error().to_string());
    }

    const cr::Generation generation = engine.value().generation();
    cr::EvidenceStamp stamp;
    stamp.generation = generation;
    stamp.revision = cr::Revision{1};
    stamp.epoch = cr::Epoch{0};
    stamp.incarnation = engine.value().incarnation();
    stamp.observed_at = cr::Tick{100};
    stamp.valid_until = cr::Tick{10000};
    stamp.clock_domain = domain.value();

    const struct {
      cr::CapacityView view;
      cr::Amount value;
      cr::EvidenceSource source;
    } rows[] = {
        {cr::CapacityView::Planned, 2000, cr::EvidenceSource::FacilityCapacity},
        {cr::CapacityView::Installed, 2000, cr::EvidenceSource::RackCapacity},
        {cr::CapacityView::Observed, 1800, cr::EvidenceSource::ObservationStream},
        {cr::CapacityView::Usable, 1800, cr::EvidenceSource::CoolingCapacity},
        {cr::CapacityView::Reserved, 300, cr::EvidenceSource::ReservationRegister},
        {cr::CapacityView::Allocatable, 1500, cr::EvidenceSource::FacilityCapacity},
    };
    std::vector<cr::EvidenceItem> items;
    for (const auto& row : rows) {
      auto item = cr::EvidenceItem::create(cr::EvidenceId::generate(), row.source, "consumer",
                                           scope.value(), dimensions.value(), row.view,
                                           cr::Quantity::known(row.value),
                                           cr::EvidenceStatus::Accepted, stamp);
      if (!item.ok()) {
        return fail("evidence: " + item.error().to_string());
      }
      items.push_back(std::move(item.value()));
    }
    auto status = engine.value().append_evidence(std::move(items), generation,
                                                engine.value().incarnation());
    if (!status.ok()) {
      return fail("append evidence: " + status.error().to_string());
    }

    cr::RunRequest request;
    cr::ScopeSelection selection;
    selection.scope = scope.value();
    selection.mode = cr::ScopeSelectionMode::Exact;
    request.scopes.push_back(selection);
    request.generation = engine.value().generation();
    request.evaluation_instant = cr::Tick{500};
    request.clock_domain = domain.value();

    auto run = engine.value().reconcile_and_commit(request, engine.value().generation(),
                                                   engine.value().incarnation());
    if (!run.ok()) {
      return fail("reconcile: " + run.error().to_string());
    }
    committed = run.value().id();
    committed_digest = run.value().run_digest();
    if (run.value().cells().size() != 1) {
      return fail("expected exactly one cell");
    }
    // This scenario has an observation shortfall that nothing explains, so the
    // run must say so rather than claiming the facility agrees.
    if (run.value().resolution() != cr::ResolutionState::PartiallyExplained) {
      return fail("expected a partially explained run, got " +
                  std::string(cr::to_string(run.value().resolution())));
    }
    const std::string json = cr::render_run_json(run.value(), true);
    if (json.find("\"schema\": \"capacity-reconciliation/v1\"") == std::string::npos) {
      return fail("rendered run is missing the schema token");
    }
    std::printf("consumer: first pass %s\n", cr::render_run_text(run.value()).c_str());
  }

  {
    cr::StoreOpenOptions open_options;
    open_options.create_if_missing = false;
    open_options.writer = false;
    auto store = cr::Store::open(root, open_options);
    if (!store.ok()) {
      return fail("reopen store: " + store.error().to_string());
    }
    if (store.value().image().runs.size() != 1) {
      return fail("expected one committed run after reopen");
    }
    cr::EngineOptions options;
    options.clock_domain = domain.value();
    auto engine = cr::Engine::attach(std::move(store.value()), options);
    if (!engine.ok()) {
      return fail("re-attach engine: " + engine.error().to_string());
    }
    auto recovered = engine.value().find_run(committed);
    if (!recovered.ok()) {
      return fail("find committed run: " + recovered.error().to_string());
    }
    if (!(recovered.value().run_digest() == committed_digest)) {
      return fail("recovered run digest does not match the committed one");
    }
    std::printf("consumer: reopened run verified\n");
  }

  const auto verification = cr::Store::verify(root, cr::default_limits());
  if (!verification.ok) {
    return fail("store verification failed: " +
                (verification.problems.empty() ? std::string("no detail")
                                               : verification.problems.front()));
  }
  std::printf("consumer: store verified at generation %llu\n",
              static_cast<unsigned long long>(verification.generation.value()));

  std::printf("consumer: ok\n");
  return 0;
}
