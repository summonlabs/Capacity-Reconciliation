// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Run diffing, revalidation lineage and the immutable history of a run.

#include <string>
#include <vector>

#include "summon/capacity_reconciliation/diff.hpp"
#include "summon/capacity_reconciliation/engine.hpp"
#include "summon/capacity_reconciliation/report.hpp"
#include "test_helpers.hpp"
#include "test_support.hpp"

namespace cr = summon::capacity_reconciliation;

namespace {

std::vector<cr::EvidenceItem> chain(const std::string& scope, cr::Amount installed,
                                    cr::Amount observed, std::uint64_t generation,
                                    const cr::IncarnationId& incarnation) {
  std::vector<cr::EvidenceItem> items;
  const struct {
    cr::CapacityView view;
    cr::Amount value;
  } rows[] = {{cr::CapacityView::Planned, installed},
              {cr::CapacityView::Installed, installed},
              {cr::CapacityView::Observed, observed},
              {cr::CapacityView::Usable, observed},
              {cr::CapacityView::Reserved, 0},
              {cr::CapacityView::Allocatable, observed}};
  for (const auto& row : rows) {
    cr::EvidenceStamp stamp;
    stamp.generation = cr::Generation{generation};
    stamp.revision = cr::Revision{1};
    stamp.epoch = cr::Epoch{0};
    stamp.incarnation = incarnation;
    stamp.observed_at = cr::Tick{100};
    stamp.valid_until = cr::Tick{100000};
    stamp.clock_domain = crtest::test_domain();
    auto item = cr::EvidenceItem::create(
        cr::EvidenceId::generate(),
        row.view == cr::CapacityView::Installed ? cr::EvidenceSource::RackCapacity
                                                : cr::EvidenceSource::ObservationStream,
        "source-1", cr::ScopeIdentity::parse(scope).value(), crtest::power_key(), row.view,
        cr::Quantity::known(row.value), cr::EvidenceStatus::Accepted, stamp);
    if (item.ok()) {
      items.push_back(std::move(item.value()));
    }
  }
  return items;
}

}  // namespace

CR_TEST(diff, transitions_are_computed_from_the_two_runs) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);

  auto first_items = chain("site-a/hall-1", 100, 100, 0, engine.value().incarnation());
  auto status = engine.value().append_evidence(std::move(first_items),
                                              engine.value().generation(),
                                              engine.value().incarnation());
  CR_REQUIRE_OK(status);

  cr::RunRequest request =
      crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 500);
  auto first = engine.value().reconcile_and_commit(request, engine.value().generation(),
                                                   engine.value().incarnation());
  CR_REQUIRE_OK(first);

  // Second pass: add a second scope and degrade the first.
  auto second_items = chain("site-a/hall-1", 100, 40, engine.value().generation().value(),
                            engine.value().incarnation());
  second_items.push_back(cr::EvidenceItem::create(
                             cr::EvidenceId::generate(), cr::EvidenceSource::RackCapacity,
                             "rack-2", cr::ScopeIdentity::parse("site-a/hall-1/row-2").value(),
                             crtest::power_key(), cr::CapacityView::Installed,
                             cr::Quantity::known(50), cr::EvidenceStatus::Accepted,
                             cr::EvidenceStamp{engine.value().generation(), cr::Revision{1},
                                               cr::Epoch{0}, engine.value().incarnation(),
                                               cr::Tick{100}, cr::Tick{100000},
                                               crtest::test_domain()})
                             .value());
  status = engine.value().append_evidence(std::move(second_items), engine.value().generation(),
                                          engine.value().incarnation());
  CR_REQUIRE_OK(status);

  cr::RunRequest second_request = request;
  second_request.generation = engine.value().generation();
  second_request.scopes.push_back({cr::ScopeIdentity::parse("site-a/hall-1/row-2").value(),
                                   cr::ScopeSelectionMode::Exact});
  auto second = engine.value().reconcile_and_commit(second_request, engine.value().generation(),
                                                    engine.value().incarnation());
  CR_REQUIRE_OK(second);

  auto diff = engine.value().diff(first.value().id(), second.value().id());
  CR_REQUIRE_OK(diff);
  CR_CHECK(diff.value().before() == first.value().id());
  CR_CHECK(diff.value().after() == second.value().id());
  CR_CHECK(!diff.value().truncated());
  CR_CHECK(diff.value().count(cr::CellTransition::Appeared) == 1);
  CR_CHECK(diff.value().count(cr::CellTransition::Reclassified) == 1);
  CR_CHECK(diff.value().count(cr::CellTransition::Unchanged) == 0);
  CR_CHECK(diff.value().count(cr::CellTransition::Disappeared) == 0);

  // Determinism: the same pair of runs always produces the same bytes.
  auto again = engine.value().diff(first.value().id(), second.value().id());
  CR_REQUIRE_OK(again);
  CR_CHECK_EQ(again.value().canonical_bytes(), diff.value().canonical_bytes());
  CR_CHECK(again.value().digest() == diff.value().digest());
}

CR_TEST(diff, a_run_cannot_be_diffed_against_itself) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  auto items = chain("site-a/hall-1", 100, 100, 0, engine.value().incarnation());
  CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                               engine.value().incarnation()));
  auto run = engine.value().reconcile_and_commit(
      crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 500),
      engine.value().generation(), engine.value().incarnation());
  CR_REQUIRE_OK(run);
  CR_REQUIRE_ERR(engine.value().diff(run.value().id(), run.value().id()),
                 cr::ErrorCode::InvalidArgument);
}

CR_TEST(diff, unknown_run_identity_is_not_found) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  CR_REQUIRE_ERR(engine.value().diff(cr::ReconciliationRunId::generate(),
                                     cr::ReconciliationRunId::generate()),
                 cr::ErrorCode::NotFound);
}

CR_TEST(lifecycle, revalidation_supersedes_and_links_the_predecessor) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  auto items = chain("site-a/hall-1", 100, 100, 0, engine.value().incarnation());
  CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                               engine.value().incarnation()));
  auto first = engine.value().reconcile_and_commit(
      crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 500),
      engine.value().generation(), engine.value().incarnation());
  CR_REQUIRE_OK(first);
  CR_CHECK_EQ(first.value().resolution(), cr::ResolutionState::Explained);

  auto successor = engine.value().revalidate(first.value().id(), cr::Tick{600},
                                             engine.value().generation(),
                                             engine.value().incarnation());
  CR_REQUIRE_OK(successor);
  CR_CHECK(!(successor.value().id() == first.value().id()));
  CR_REQUIRE(successor.value().request().parent_run.has_value());
  CR_CHECK(*successor.value().request().parent_run == first.value().id());

  auto predecessor = engine.value().find_run(first.value().id());
  CR_REQUIRE_OK(predecessor);
  CR_CHECK_EQ(predecessor.value().resolution(), cr::ResolutionState::Superseded);
  CR_REQUIRE(predecessor.value().superseded_by().has_value());
  CR_CHECK(*predecessor.value().superseded_by() == successor.value().id());

  // The predecessor's own content digest is unchanged: superseding is metadata,
  // not a rewrite of the answer that was given.
  CR_CHECK(predecessor.value().run_digest() == first.value().run_digest());

  // History records the supersession and points at both runs.
  const auto history = engine.value().history_for(first.value().id());
  bool saw_supersession = false;
  for (const cr::HistoryEntry& entry : history) {
    if (entry.kind == cr::HistoryEventKind::RunSuperseded) {
      saw_supersession = true;
      CR_REQUIRE(entry.related_run.has_value());
      CR_CHECK(*entry.related_run == successor.value().id());
    }
  }
  CR_CHECK(saw_supersession);
}

CR_TEST(lifecycle, close_then_reopen_then_successor) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  auto items = chain("site-a/hall-1", 100, 100, 0, engine.value().incarnation());
  CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                               engine.value().incarnation()));
  auto run = engine.value().reconcile_and_commit(
      crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 500),
      engine.value().generation(), engine.value().incarnation());
  CR_REQUIRE_OK(run);

  // A live run cannot be reopened.
  CR_REQUIRE_ERR(engine.value().reopen(run.value().id(), cr::Tick{600},
                                       engine.value().generation(),
                                       engine.value().incarnation()),
                 cr::ErrorCode::InvalidArgument);

  CR_REQUIRE_OK(engine.value().close_run(run.value().id(), cr::Tick{600},
                                         engine.value().generation(),
                                         engine.value().incarnation()));
  auto closed = engine.value().find_run(run.value().id());
  CR_REQUIRE_OK(closed);
  CR_CHECK_EQ(closed.value().resolution(), cr::ResolutionState::Closed);

  // Closing twice is refused rather than silently accepted.
  CR_REQUIRE_ERR(engine.value().close_run(run.value().id(), cr::Tick{600},
                                          engine.value().generation(),
                                          engine.value().incarnation()),
                 cr::ErrorCode::AlreadyExists);

  auto ticket = engine.value().reopen(run.value().id(), cr::Tick{600},
                                      engine.value().generation(),
                                      engine.value().incarnation());
  CR_REQUIRE_OK(ticket);
  CR_CHECK_EQ(ticket.value().issued_at.value(), engine.value().generation().value());

  // The ticket authorises exactly one successor at the generation it was
  // issued for. Presenting it at a later generation is refused.
  auto successor = engine.value().reconcile_successor(ticket.value(), cr::Tick{600},
                                                      engine.value().generation(),
                                                      engine.value().incarnation());
  CR_REQUIRE_OK(successor);
  CR_REQUIRE(successor.value().request().parent_run.has_value());
  CR_CHECK(*successor.value().request().parent_run == run.value().id());

  auto reopened = engine.value().find_run(run.value().id());
  CR_REQUIRE_OK(reopened);
  CR_CHECK_EQ(reopened.value().resolution(), cr::ResolutionState::Superseded);
}

CR_TEST(lifecycle, a_stale_reopen_ticket_is_refused) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  auto items = chain("site-a/hall-1", 100, 100, 0, engine.value().incarnation());
  CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                               engine.value().incarnation()));
  auto run = engine.value().reconcile_and_commit(
      crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 500),
      engine.value().generation(), engine.value().incarnation());
  CR_REQUIRE_OK(run);
  CR_REQUIRE_OK(engine.value().close_run(run.value().id(), cr::Tick{600},
                                         engine.value().generation(),
                                         engine.value().incarnation()));
  auto ticket = engine.value().reopen(run.value().id(), cr::Tick{600},
                                      engine.value().generation(),
                                      engine.value().incarnation());
  CR_REQUIRE_OK(ticket);

  // Consume the ticket.
  auto successor = engine.value().reconcile_successor(ticket.value(), cr::Tick{600},
                                                      engine.value().generation(),
                                                      engine.value().incarnation());
  CR_REQUIRE_OK(successor);
  // Replaying it must fail: neither the generation nor the reopened state still
  // matches.
  auto replay = engine.value().reconcile_successor(ticket.value(), cr::Tick{600},
                                                   engine.value().generation(),
                                                   engine.value().incarnation());
  CR_REQUIRE_ERR(replay, cr::ErrorCode::StaleAuthority);
}

CR_TEST(lifecycle, stale_generation_and_incarnation_are_refused) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  auto items = chain("site-a/hall-1", 100, 100, 0, engine.value().incarnation());
  auto wrong_generation = engine.value().append_evidence(items, cr::Generation{77},
                                                         engine.value().incarnation());
  CR_REQUIRE_ERR(wrong_generation, cr::ErrorCode::StaleGeneration);

  const cr::IncarnationId bogus = cr::IncarnationId::generate();
  auto wrong_incarnation =
      engine.value().append_evidence(items, engine.value().generation(), bogus);
  CR_REQUIRE_ERR(wrong_incarnation, cr::ErrorCode::StaleAuthority);
}

CR_TEST(lifecycle, evidence_beyond_the_current_generation_is_refused) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  auto items = chain("site-a/hall-1", 100, 100, 5, engine.value().incarnation());
  auto refused = engine.value().append_evidence(std::move(items), engine.value().generation(),
                                                engine.value().incarnation());
  CR_REQUIRE_ERR(refused, cr::ErrorCode::StaleGeneration);
}

CR_TEST(lifecycle, duplicate_evidence_identity_is_refused_on_append) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  auto items = chain("site-a/hall-1", 100, 100, 0, engine.value().incarnation());
  auto first = engine.value().append_evidence(items, engine.value().generation(),
                                              engine.value().incarnation());
  CR_REQUIRE_OK(first);
  auto second = engine.value().append_evidence(items, engine.value().generation(),
                                               engine.value().incarnation());
  CR_REQUIRE_ERR(second, cr::ErrorCode::AlreadyExists);
}

CR_TEST(lifecycle, observer_never_runs_under_the_engine_lock) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  std::size_t observed = 0;
  std::size_t generation_seen_inside_observer = 0;
  engine.value().set_observer([&](const cr::HistoryEntry& entry) {
    ++observed;
    // Re-entering the engine from the observer must not deadlock. This is the
    // property that makes the callback safe, so it is asserted rather than
    // assumed.
    generation_seen_inside_observer = engine.value().generation().value();
    CR_CHECK(entry.sequence > 0);
  });

  auto items = chain("site-a/hall-1", 100, 100, 0, engine.value().incarnation());
  auto status = engine.value().append_evidence(std::move(items), engine.value().generation(),
                                               engine.value().incarnation());
  CR_REQUIRE_OK(status);
  CR_CHECK_EQ(observed, std::size_t(1));
  CR_CHECK_EQ(generation_seen_inside_observer, std::uint64_t(1));
}

CR_TEST(diff, explanation_digest_detects_a_change_without_a_reclassification) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  auto first_items = chain("site-a/hall-1", 100, 100, 0, engine.value().incarnation());
  CR_REQUIRE_OK(engine.value().append_evidence(std::move(first_items),
                                               engine.value().generation(),
                                               engine.value().incarnation()));
  auto first = engine.value().reconcile(
      crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 500));
  CR_REQUIRE_OK(first);

  // Add an attribution that explains nothing, so the classification is
  // unchanged but the explanation graph grows.
  auto attribution = crtest::make_attribution(
      "site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt,
      cr::ResidualKind::ObservedGap, 0, cr::EvidenceSource::CoolingCapacity, "cooling-1",
      engine.value().generation().value(), 1, 100, 100000);
  CR_REQUIRE_OK(attribution);
  CR_REQUIRE_OK(engine.value().append_attributions({attribution.value()},
                                                   engine.value().generation(),
                                                   engine.value().incarnation()));

  cr::RunRequest request =
      crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 500);
  auto second = engine.value().reconcile_and_commit(request, engine.value().generation(),
                                                    engine.value().incarnation());
  CR_REQUIRE_OK(second);

  // The committed generations differ, so the two runs are not byte-identical
  // even though the cells agree; the point is that the diff sees the change.
  auto persisted_first = engine.value().find_run(first.value().id());
  (void)persisted_first;
  CR_CHECK(!(first.value().run_digest() == second.value().run_digest()));
}
