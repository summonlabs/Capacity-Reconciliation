// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Bounds. Every externally supplied size is checked before it is used, and a
// declaration that exceeds a limit is refused rather than truncated.

#include <string>
#include <vector>

#include "summon/capacity_reconciliation/engine.hpp"
#include "summon/capacity_reconciliation/journal.hpp"
#include "summon/capacity_reconciliation/store.hpp"
#include "test_helpers.hpp"
#include "test_support.hpp"

namespace cr = summon::capacity_reconciliation;

namespace {

cr::Result<cr::Engine> engine_with(const cr::Limits& limits) {
  cr::EngineOptions options;
  options.clock_domain = crtest::test_domain();
  options.limits = limits;
  return cr::Engine::create(options);
}

cr::EvidenceItem item(const std::string& scope, std::uint64_t generation,
                      const cr::IncarnationId& incarnation, const std::string& instance) {
  cr::EvidenceStamp stamp;
  stamp.generation = cr::Generation{generation};
  stamp.revision = cr::Revision{1};
  stamp.incarnation = incarnation;
  stamp.observed_at = cr::Tick{1};
  stamp.valid_until = cr::Tick{100000};
  stamp.clock_domain = crtest::test_domain();
  return cr::EvidenceItem::create(
             cr::EvidenceId::generate(), cr::EvidenceSource::RackCapacity, instance,
             cr::ScopeIdentity::parse(scope).value(), crtest::power_key(),
             cr::CapacityView::Installed, cr::Quantity::known(1), cr::EvidenceStatus::Accepted,
             stamp)
      .value();
}

}  // namespace

CR_TEST(scale, the_evidence_bound_is_enforced_before_the_append_is_accepted) {
  cr::Limits limits = cr::default_limits();
  limits.max_evidence_items = 4;
  auto engine = engine_with(limits);
  CR_REQUIRE_OK(engine);

  std::vector<cr::EvidenceItem> first;
  for (int i = 0; i < 4; ++i) {
    first.push_back(item("site-a/row-" + std::to_string(i), 0, engine.value().incarnation(),
                         "instance-" + std::to_string(i)));
  }
  CR_REQUIRE_OK(engine.value().append_evidence(std::move(first), engine.value().generation(),
                                               engine.value().incarnation()));

  std::vector<cr::EvidenceItem> overflow{
      item("site-a/row-9", engine.value().generation().value(), engine.value().incarnation(), "x")};
  CR_REQUIRE_ERR(engine.value().append_evidence(std::move(overflow), engine.value().generation(),
                                                engine.value().incarnation()),
                 cr::ErrorCode::LimitExceeded);
  CR_CHECK_EQ(engine.value().evidence_snapshot()->size(), std::size_t(4));
}

CR_TEST(scale, the_per_cell_evidence_bound_names_the_cell_it_refused) {
  cr::Limits limits = cr::default_limits();
  limits.max_evidence_per_cell = 3;
  auto engine = engine_with(limits);
  CR_REQUIRE_OK(engine);
  std::vector<cr::EvidenceItem> items;
  for (int i = 0; i < 5; ++i) {
    items.push_back(item("site-a/row-1", 0, engine.value().incarnation(),
                         "instance-" + std::to_string(i)));
  }
  CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                               engine.value().incarnation()));
  auto run = engine.value().reconcile(
      crtest::exact_request("site-a/row-1", engine.value().generation().value(), 50));
  CR_REQUIRE_ERR(run, cr::ErrorCode::LimitExceeded);
  CR_CHECK(run.error().message.find("site-a/row-1") != std::string::npos);
}

CR_TEST(scale, the_cell_bound_is_enforced) {
  cr::Limits limits = cr::default_limits();
  limits.max_cells_per_run = 2;
  auto engine = engine_with(limits);
  CR_REQUIRE_OK(engine);
  std::vector<cr::EvidenceItem> items;
  for (int i = 0; i < 5; ++i) {
    items.push_back(item("site-a/row-" + std::to_string(i), 0, engine.value().incarnation(),
                         "instance-" + std::to_string(i)));
  }
  CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                               engine.value().incarnation()));
  cr::RunRequest request;
  for (int i = 0; i < 5; ++i) {
    request.scopes.push_back(
        {cr::ScopeIdentity::parse("site-a/row-" + std::to_string(i)).value(),
         cr::ScopeSelectionMode::Exact});
  }
  request.generation = engine.value().generation();
  request.evaluation_instant = cr::Tick{50};
  request.clock_domain = crtest::test_domain();
  CR_REQUIRE_ERR(engine.value().reconcile(request), cr::ErrorCode::LimitExceeded);
}

CR_TEST(scale, the_scope_selection_bound_is_enforced) {
  cr::Limits limits = cr::default_limits();
  limits.max_scope_selections = 2;
  auto engine = engine_with(limits);
  CR_REQUIRE_OK(engine);
  cr::RunRequest request;
  for (int i = 0; i < 3; ++i) {
    request.scopes.push_back(
        {cr::ScopeIdentity::parse("site-a/row-" + std::to_string(i)).value(),
         cr::ScopeSelectionMode::Exact});
  }
  request.generation = engine.value().generation();
  request.evaluation_instant = cr::Tick{50};
  request.clock_domain = crtest::test_domain();
  CR_REQUIRE_ERR(engine.value().reconcile(request), cr::ErrorCode::LimitExceeded);
}

CR_TEST(scale, the_conflict_bound_is_enforced_rather_than_truncating) {
  cr::Limits limits = cr::default_limits();
  limits.max_conflicts_per_cell = 1;
  auto engine = engine_with(limits);
  CR_REQUIRE_OK(engine);
  // Four statements at equal precedence in the same view produce one conflict;
  // add six views' worth to exceed a bound of one.
  std::vector<cr::EvidenceItem> items;
  const cr::CapacityView views[] = {cr::CapacityView::Planned,  cr::CapacityView::Reserved,
                                    cr::CapacityView::Installed, cr::CapacityView::Observed,
                                    cr::CapacityView::Usable,   cr::CapacityView::Allocatable};
  for (std::size_t v = 0; v < 6; ++v) {
    for (int i = 0; i < 3; ++i) {
      cr::EvidenceStamp stamp;
      stamp.generation = cr::Generation{0};
      stamp.revision = cr::Revision{1};
      stamp.incarnation = engine.value().incarnation();
      stamp.observed_at = cr::Tick{1};
      stamp.valid_until = cr::Tick{100000};
      stamp.clock_domain = crtest::test_domain();
      auto built = cr::EvidenceItem::create(
          cr::EvidenceId::generate(), cr::EvidenceSource::ObservationStream,
          "instance-" + std::to_string(i), cr::ScopeIdentity::parse("site-a/row-1").value(),
          crtest::power_key(), views[v], cr::Quantity::known(static_cast<cr::Amount>(i)),
          cr::EvidenceStatus::Accepted, stamp);
      CR_REQUIRE_OK(built);
      items.push_back(std::move(built.value()));
    }
  }
  CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                               engine.value().incarnation()));
  auto run = engine.value().reconcile(
      crtest::exact_request("site-a/row-1", engine.value().generation().value(), 50));
  CR_REQUIRE_ERR(run, cr::ErrorCode::LimitExceeded);
}

CR_TEST(scale, the_explanation_graph_bound_is_enforced) {
  cr::Limits limits = cr::default_limits();
  limits.max_explanation_nodes_per_cell = 4;
  auto engine = engine_with(limits);
  CR_REQUIRE_OK(engine);
  std::vector<cr::EvidenceItem> items;
  for (int i = 0; i < 3; ++i) {
    items.push_back(item("site-a/row-1", 0, engine.value().incarnation(),
                         "instance-" + std::to_string(i)));
  }
  CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                               engine.value().incarnation()));
  auto run = engine.value().reconcile(
      crtest::exact_request("site-a/row-1", engine.value().generation().value(), 50));
  CR_REQUIRE_ERR(run, cr::ErrorCode::LimitExceeded);
}

CR_TEST(scale, the_run_retention_bound_retires_the_oldest_unreferenced_run) {
  cr::Limits limits = cr::default_limits();
  limits.max_runs_retained = 3;
  cr::EngineOptions options;
  options.clock_domain = crtest::test_domain();
  options.limits = limits;
  options.max_runs_retained = 3;
  auto engine = cr::Engine::create(options);
  CR_REQUIRE_OK(engine);
  std::vector<cr::EvidenceItem> items;
  for (int i = 0; i < 3; ++i) {
    items.push_back(item("site-a/row-1", 0, engine.value().incarnation(),
                         "instance-" + std::to_string(i)));
  }
  CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                               engine.value().incarnation()));
  for (int i = 0; i < 6; ++i) {
    auto run = engine.value().reconcile_and_commit(
        crtest::exact_request("site-a/row-1", engine.value().generation().value(), 50),
        engine.value().generation(), engine.value().incarnation());
    CR_REQUIRE_OK(run);
  }
  CR_CHECK_EQ(engine.value().runs().size(), std::size_t(3));
}

CR_TEST(scale, a_retained_run_limit_with_fully_referenced_lineage_is_refused) {
  cr::Limits limits = cr::default_limits();
  limits.max_runs_retained = 2;
  cr::EngineOptions options;
  options.clock_domain = crtest::test_domain();
  options.limits = limits;
  options.max_runs_retained = 2;
  auto engine = cr::Engine::create(options);
  CR_REQUIRE_OK(engine);
  std::vector<cr::EvidenceItem> items;
  for (int i = 0; i < 3; ++i) {
    items.push_back(item("site-a/row-1", 0, engine.value().incarnation(),
                         "instance-" + std::to_string(i)));
  }
  CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                               engine.value().incarnation()));

  auto first = engine.value().reconcile_and_commit(
      crtest::exact_request("site-a/row-1", engine.value().generation().value(), 50),
      engine.value().generation(), engine.value().incarnation());
  CR_REQUIRE_OK(first);
  // Each revalidation names its predecessor, so no retained run is free to
  // retire and the third attempt is refused rather than breaking lineage.
  auto second = engine.value().revalidate(first.value().id(), cr::Tick{60},
                                          engine.value().generation(),
                                          engine.value().incarnation());
  CR_REQUIRE_OK(second);
  auto third = engine.value().revalidate(second.value().id(), cr::Tick{70},
                                         engine.value().generation(),
                                         engine.value().incarnation());
  CR_REQUIRE_ERR(third, cr::ErrorCode::LimitExceeded);
}

CR_TEST(scale, the_record_payload_bound_is_enforced_before_a_write) {
  cr::RecordHeader header;
  header.generation = cr::Generation{1};
  const std::string oversized(cr::kMaxRecordBytes + 1, 'x');
  CR_REQUIRE_ERR(cr::frame_record(header, oversized), cr::ErrorCode::LimitExceeded);
}

CR_TEST(scale, the_history_bound_retires_the_oldest_entries) {
  cr::Limits limits = cr::default_limits();
  limits.max_history_retained = 3;
  cr::EngineOptions options;
  options.clock_domain = crtest::test_domain();
  options.limits = limits;
  auto engine = cr::Engine::create(options);
  CR_REQUIRE_OK(engine);
  for (int i = 0; i < 8; ++i) {
    std::vector<cr::EvidenceItem> items{
        item("site-a/row-" + std::to_string(i), engine.value().generation().value(),
             engine.value().incarnation(), "instance")};
    CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                                 engine.value().incarnation()));
  }
  CR_CHECK_EQ(engine.value().history().size(), std::size_t(3));
  for (std::size_t i = 1; i < engine.value().history().size(); ++i) {
    CR_CHECK(engine.value().history()[i].sequence > engine.value().history()[i - 1].sequence);
  }
}

CR_TEST(scale, a_large_facility_reconciles_within_its_bounds) {
  cr::EngineOptions options;
  options.clock_domain = crtest::test_domain();
  // A single rollup cell over the whole site sees every statement in it, so the
  // per-cell bound has to be raised deliberately for this workload. That the
  // bound is enforced by default is proved by the_per_cell_evidence_bound test.
  options.limits.max_evidence_per_cell = 4000;
  options.limits.max_explanation_nodes_per_cell = 4096;
  auto engine = cr::Engine::create(options);
  CR_REQUIRE_OK(engine);

  constexpr int kScopes = 400;
  std::vector<cr::EvidenceItem> items;
  items.reserve(static_cast<std::size_t>(kScopes) * 6);
  for (int scope = 0; scope < kScopes; ++scope) {
    const std::string path = "site-a/hall-" + std::to_string(scope / 100) + "/row-" +
                             std::to_string(scope % 100);
    const cr::CapacityView views[] = {cr::CapacityView::Planned,  cr::CapacityView::Reserved,
                                      cr::CapacityView::Installed, cr::CapacityView::Observed,
                                      cr::CapacityView::Usable,   cr::CapacityView::Allocatable};
    for (std::size_t v = 0; v < 6; ++v) {
      cr::EvidenceStamp stamp;
      stamp.generation = cr::Generation{0};
      stamp.revision = cr::Revision{1};
      stamp.incarnation = engine.value().incarnation();
      stamp.observed_at = cr::Tick{1};
      stamp.valid_until = cr::Tick{100000};
      stamp.clock_domain = crtest::test_domain();
      auto built = cr::EvidenceItem::create(
          cr::EvidenceId::generate(), cr::EvidenceSource::RackCapacity, "r",
          cr::ScopeIdentity::parse(path).value(), crtest::power_key(), views[v],
          cr::Quantity::known(100), cr::EvidenceStatus::Accepted, stamp);
      CR_REQUIRE_OK(built);
      items.push_back(std::move(built.value()));
    }
  }
  CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                               engine.value().incarnation()));

  cr::RunRequest request;
  request.scopes.push_back({cr::ScopeIdentity::parse("site-a").value(),
                            cr::ScopeSelectionMode::SubtreeRollup});
  request.generation = engine.value().generation();
  request.evaluation_instant = cr::Tick{50};
  request.clock_domain = crtest::test_domain();
  auto run = engine.value().reconcile(request);
  CR_REQUIRE_OK(run);
  CR_REQUIRE(run.value().cells().size() == 1);
  const cr::ReconciliationCell& cell = run.value().cells().front();
  CR_CHECK_EQ(cell.view(cr::CapacityView::Installed).value(),
              static_cast<cr::Amount>(kScopes) * 100);
  CR_CHECK_EQ(cell.rollup_contributors, static_cast<std::uint32_t>(kScopes));
  CR_CHECK_EQ(cell.rollup_missing, std::uint32_t(0));
}

CR_TEST(scale, deeply_nested_scopes_are_bounded) {
  // The maximum legal depth is kMaxScopeDepth segments; one more is refused.
  std::string path;
  for (std::size_t i = 0; i < cr::kMaxScopeDepth; ++i) {
    if (!path.empty()) {
      path.push_back('/');
    }
    path.push_back('a');
  }
  CR_REQUIRE_OK(cr::ScopeIdentity::parse(path));
  path += "/a";
  CR_REQUIRE_ERR(cr::ScopeIdentity::parse(path), cr::ErrorCode::LimitExceeded);
}
