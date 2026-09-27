// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The reconciliation kernel: selection, precedence, conflicts, rollup,
// attribution, residual preservation and determinism.

#include <algorithm>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "summon/capacity_reconciliation/engine.hpp"
#include "summon/capacity_reconciliation/report.hpp"
#include "test_helpers.hpp"
#include "test_support.hpp"

namespace cr = summon::capacity_reconciliation;

namespace {

crtest::EvidenceBuilder builder(const std::string& scope, cr::CapacityDimension dimension,
                                cr::Unit unit) {
  return crtest::EvidenceBuilder(scope, dimension, unit);
}

void append_all(cr::Engine& engine, const std::vector<cr::Result<cr::EvidenceItem>>& items) {
  std::vector<cr::EvidenceItem> flat;
  for (const auto& item : items) {
    CR_REQUIRE_OK(item);
    flat.push_back(item.value());
  }
  auto status =
      engine.append_evidence(std::move(flat), engine.generation(), engine.incarnation());
  CR_REQUIRE_OK(status);
}

/// A cell with planned == installed == observed == usable, reserved 0, and
/// allocatable == usable: the healthy reference case.
std::vector<cr::Result<cr::EvidenceItem>> healthy_cell(const std::string& scope,
                                                       cr::Amount amount) {
  return {
      builder(scope, cr::CapacityDimension::Power, cr::Unit::MilliWatt)
          .source(cr::EvidenceSource::FacilityCapacity)
          .instance("facility-1")
          .known(cr::CapacityView::Planned, amount),
      builder(scope, cr::CapacityDimension::Power, cr::Unit::MilliWatt)
          .source(cr::EvidenceSource::RackCapacity)
          .instance("rack-1")
          .known(cr::CapacityView::Installed, amount),
      builder(scope, cr::CapacityDimension::Power, cr::Unit::MilliWatt)
          .source(cr::EvidenceSource::ObservationStream)
          .instance("meter-1")
          .known(cr::CapacityView::Observed, amount),
      builder(scope, cr::CapacityDimension::Power, cr::Unit::MilliWatt)
          .source(cr::EvidenceSource::CoolingCapacity)
          .instance("cooling-1")
          .known(cr::CapacityView::Usable, amount),
      builder(scope, cr::CapacityDimension::Power, cr::Unit::MilliWatt)
          .source(cr::EvidenceSource::ReservationRegister)
          .instance("reservations")
          .known(cr::CapacityView::Reserved, 0),
      builder(scope, cr::CapacityDimension::Power, cr::Unit::MilliWatt)
          .source(cr::EvidenceSource::FacilityCapacity)
          .instance("facility-1")
          .known(cr::CapacityView::Allocatable, amount),
  };
}

/// A complete chain with independently chosen values. Used where the shape of
/// every residual matters, so that a test cannot accidentally trip an
/// unrelated finding.
std::vector<cr::Result<cr::EvidenceItem>> full_chain(const std::string& scope,
                                                     cr::Amount planned, cr::Amount installed,
                                                     cr::Amount observed, cr::Amount usable,
                                                     cr::Amount reserved,
                                                     cr::Amount allocatable) {
  return {
      builder(scope, cr::CapacityDimension::Power, cr::Unit::MilliWatt)
          .source(cr::EvidenceSource::FacilityCapacity)
          .instance("facility-1")
          .known(cr::CapacityView::Planned, planned),
      builder(scope, cr::CapacityDimension::Power, cr::Unit::MilliWatt)
          .source(cr::EvidenceSource::RackCapacity)
          .instance("rack-1")
          .known(cr::CapacityView::Installed, installed),
      builder(scope, cr::CapacityDimension::Power, cr::Unit::MilliWatt)
          .source(cr::EvidenceSource::ObservationStream)
          .instance("meter-1")
          .known(cr::CapacityView::Observed, observed),
      builder(scope, cr::CapacityDimension::Power, cr::Unit::MilliWatt)
          .source(cr::EvidenceSource::CoolingCapacity)
          .instance("cooling-1")
          .known(cr::CapacityView::Usable, usable),
      builder(scope, cr::CapacityDimension::Power, cr::Unit::MilliWatt)
          .source(cr::EvidenceSource::ReservationRegister)
          .instance("reservations")
          .known(cr::CapacityView::Reserved, reserved),
      builder(scope, cr::CapacityDimension::Power, cr::Unit::MilliWatt)
          .source(cr::EvidenceSource::FacilityCapacity)
          .instance("facility-1")
          .known(cr::CapacityView::Allocatable, allocatable),
  };
}

}  // namespace

CR_TEST(reconcile, healthy_cell_agrees) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  append_all(engine.value(), healthy_cell("site-a/hall-1/row-1", 1000));

  auto run = engine.value().reconcile(
      crtest::exact_request("site-a/hall-1/row-1", engine.value().generation().value(), 500));
  CR_REQUIRE_OK(run);
  CR_REQUIRE(run.value().cells().size() == 1);
  const cr::ReconciliationCell& cell = run.value().cells().front();
  CR_CHECK_EQ(cell.primary_class(), cr::DiscrepancyClass::Agrees);
  CR_CHECK_EQ(cell.view(cr::CapacityView::Installed).value(), cr::Amount(1000));
  CR_CHECK_EQ(cell.observed_gap.value(), cr::Amount(0));
  CR_CHECK_EQ(cell.planned_gap.value(), cr::Amount(0));
  CR_CHECK_EQ(cell.allocatable_skew.value(), cr::Amount(0));
  CR_CHECK(!cell.explanation.has_unexplained());
  CR_CHECK_EQ(run.value().resolution(), cr::ResolutionState::Explained);
}

CR_TEST(reconcile, observed_shortfall_with_partial_attribution_keeps_the_remainder) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  append_all(engine.value(), full_chain("site-a/hall-1/row-1", 1000, 1000, 900, 900, 0, 900));

  auto attribution = crtest::make_attribution(
      "site-a/hall-1/row-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt,
      cr::ResidualKind::ObservedGap, 40, cr::EvidenceSource::CoolingCapacity, "cooling-1",
      engine.value().generation().value(), 1, 100, 900, cr::AttributionReason::ThermalLimit);
  CR_REQUIRE_OK(attribution);
  auto status = engine.value().append_attributions({attribution.value()},
                                                   engine.value().generation(),
                                                   engine.value().incarnation());
  CR_REQUIRE_OK(status);

  auto run = engine.value().reconcile(
      crtest::exact_request("site-a/hall-1/row-1", engine.value().generation().value(), 500));
  CR_REQUIRE_OK(run);
  const cr::ReconciliationCell& cell = run.value().cells().front();
  CR_CHECK_EQ(cell.observed_gap.value(), cr::Amount(100));
  const cr::ResidualExplanation* explanation =
      cell.explanation.find_residual(cr::ReasonTarget::ObservedGap);
  CR_REQUIRE(explanation != nullptr);
  CR_CHECK_EQ(explanation->raw.value(), cr::Amount(100));
  CR_CHECK_EQ(explanation->attributed.value(), cr::Amount(40));
  CR_CHECK_EQ(explanation->unexplained.value(), cr::Amount(60));
  CR_CHECK_EQ(cell.primary_class(), cr::DiscrepancyClass::ObservedShortfall);
  CR_CHECK(cell.has_class(cr::DiscrepancyClass::UnexplainedResidual));
  CR_CHECK_EQ(run.value().resolution(), cr::ResolutionState::PartiallyExplained);
  CR_CHECK_EQ(run.value().unexplained_cell_count(), std::size_t(1));
}

CR_TEST(reconcile, full_attribution_explains_the_residual_completely) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  append_all(engine.value(), full_chain("site-a/hall-1/row-1", 1000, 1000, 900, 900, 0, 900));

  auto attribution = crtest::make_attribution(
      "site-a/hall-1/row-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt,
      cr::ResidualKind::ObservedGap, 100, cr::EvidenceSource::CoolingCapacity, "cooling-1",
      engine.value().generation().value(), 1, 100, 900, cr::AttributionReason::ThermalLimit);
  CR_REQUIRE_OK(attribution);
  CR_REQUIRE_OK(engine.value().append_attributions({attribution.value()},
                                                   engine.value().generation(),
                                                   engine.value().incarnation()));

  auto run = engine.value().reconcile(
      crtest::exact_request("site-a/hall-1/row-1", engine.value().generation().value(), 500));
  CR_REQUIRE_OK(run);
  const cr::ReconciliationCell& cell = run.value().cells().front();
  CR_CHECK_EQ(cell.primary_class(), cr::DiscrepancyClass::ObservedShortfall);
  CR_CHECK(!cell.has_class(cr::DiscrepancyClass::UnexplainedResidual));
  CR_CHECK_EQ(run.value().resolution(), cr::ResolutionState::Explained);
}

CR_TEST(reconcile, attribution_for_a_different_target_does_not_apply) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  append_all(engine.value(), full_chain("site-a/hall-1/row-1", 1000, 1000, 900, 900, 0, 900));

  auto attribution = crtest::make_attribution(
      "site-a/hall-1/row-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt,
      cr::ResidualKind::PlannedGap, 100, cr::EvidenceSource::CoolingCapacity, "cooling-1",
      engine.value().generation().value(), 1, 100, 900);
  CR_REQUIRE_OK(attribution);
  CR_REQUIRE_OK(engine.value().append_attributions({attribution.value()},
                                                   engine.value().generation(),
                                                   engine.value().incarnation()));

  auto run = engine.value().reconcile(
      crtest::exact_request("site-a/hall-1/row-1", engine.value().generation().value(), 500));
  CR_REQUIRE_OK(run);
  const cr::ReconciliationCell& cell = run.value().cells().front();
  CR_CHECK_EQ(cell.explanation.find_residual(cr::ReasonTarget::ObservedGap)->unexplained.value(),
              cr::Amount(100));
  CR_CHECK(cell.has_class(cr::DiscrepancyClass::UnexplainedResidual));
}

CR_TEST(reconcile, attribution_for_a_different_scope_does_not_apply) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  append_all(engine.value(), full_chain("site-a/hall-1/row-1", 1000, 1000, 900, 900, 0, 900));
  auto attribution = crtest::make_attribution(
      "site-a/hall-1/row-2", cr::CapacityDimension::Power, cr::Unit::MilliWatt,
      cr::ResidualKind::ObservedGap, 100, cr::EvidenceSource::CoolingCapacity, "cooling-1",
      engine.value().generation().value(), 1, 100, 900);
  CR_REQUIRE_OK(attribution);
  CR_REQUIRE_OK(engine.value().append_attributions({attribution.value()},
                                                   engine.value().generation(),
                                                   engine.value().incarnation()));
  auto run = engine.value().reconcile(
      crtest::exact_request("site-a/hall-1/row-1", engine.value().generation().value(), 500));
  CR_REQUIRE_OK(run);
  CR_CHECK_EQ(run.value().cells().front()
                  .explanation.find_residual(cr::ReasonTarget::ObservedGap)
                  ->unexplained.value(),
              cr::Amount(100));
}

CR_TEST(reconcile, precedence_selects_the_more_authoritative_source) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  // AssetRegistry (rank 2) outranks RackCapacity (rank 6) in the standard
  // policy, so the registries' value wins -- and the disagreement is recorded.
  append_all(engine.value(),
             {builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::RackCapacity)
                  .instance("rack-capacity-1")
                  .known(cr::CapacityView::Installed, 100),
              builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::AssetRegistry)
                  .instance("asset-registry-1")
                  .known(cr::CapacityView::Installed, 120),
              builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::ObservationStream)
                  .instance("meter-1")
                  .known(cr::CapacityView::Observed, 120)});

  auto run = engine.value().reconcile(
      crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 500));
  CR_REQUIRE_OK(run);
  const cr::ReconciliationCell& cell = run.value().cells().front();
  CR_CHECK_EQ(cell.view(cr::CapacityView::Installed).value(), cr::Amount(120));
  // The disagreement was ordered by policy, so the cell is not conflicted; the
  // cell has no other view and so has nothing else to say.
  CR_CHECK_EQ(cell.primary_class(), cr::DiscrepancyClass::Agrees);
  CR_REQUIRE(cell.conflicts.size() == 1);
  CR_CHECK_EQ(cell.conflicts.front().unresolvable_reason, std::string("outranked_by_precedence"));
  CR_REQUIRE(cell.conflicts.front().selected_value.has_value());
  CR_CHECK_EQ(*cell.conflicts.front().selected_value, cr::Amount(120));
  CR_CHECK_EQ(cell.conflicts.front().values.size(), std::size_t(2));
  CR_CHECK(!cell.has_class(cr::DiscrepancyClass::EvidenceConflict));
}

CR_TEST(reconcile, equal_precedence_disagreement_is_unresolvable) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  append_all(engine.value(),
             {builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::ObservationStream)
                  .instance("meter-1")
                  .known(cr::CapacityView::Installed, 100),
              builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::ObservationStream)
                  .instance("meter-2")
                  .known(cr::CapacityView::Installed, 140)});

  auto run = engine.value().reconcile(
      crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 500));
  CR_REQUIRE_OK(run);
  const cr::ReconciliationCell& cell = run.value().cells().front();
  CR_CHECK(cell.view(cr::CapacityView::Installed).is_unknown());
  CR_CHECK_EQ(cell.view(cr::CapacityView::Installed).reason(), cr::UnknownReason::Conflicted);
  CR_CHECK_EQ(cell.primary_class(), cr::DiscrepancyClass::EvidenceConflict);
  CR_REQUIRE(cell.conflicts.size() == 1);
  CR_CHECK_EQ(cell.conflicts.front().unresolvable_reason, std::string("equal_precedence"));
  CR_CHECK(!cell.conflicts.front().selected_value.has_value());
  CR_CHECK_EQ(run.value().resolution(), cr::ResolutionState::Conflicted);
}

CR_TEST(reconcile, intra_source_disagreement_at_equal_revision_is_a_conflict) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  append_all(engine.value(),
             {builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::RackCapacity)
                  .instance("rack-capacity-1")
                  .revision(3)
                  .known(cr::CapacityView::Installed, 100),
              builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::RackCapacity)
                  .instance("rack-capacity-1")
                  .revision(3)
                  .known(cr::CapacityView::Installed, 110)});
  auto run = engine.value().reconcile(
      crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 500));
  CR_REQUIRE_OK(run);
  const cr::ReconciliationCell& cell = run.value().cells().front();
  CR_REQUIRE(cell.conflicts.size() == 1);
  CR_CHECK_EQ(cell.conflicts.front().unresolvable_reason,
              std::string("intra_source_disagreement"));
  CR_CHECK_EQ(cell.primary_class(), cr::DiscrepancyClass::EvidenceConflict);
}

CR_TEST(reconcile, revision_supersession_is_not_a_conflict) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  append_all(engine.value(),
             {builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::RackCapacity)
                  .instance("rack-capacity-1")
                  .revision(1)
                  .known(cr::CapacityView::Installed, 100),
              builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::RackCapacity)
                  .instance("rack-capacity-1")
                  .revision(2)
                  .known(cr::CapacityView::Installed, 130)});
  auto run = engine.value().reconcile(
      crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 500));
  CR_REQUIRE_OK(run);
  const cr::ReconciliationCell& cell = run.value().cells().front();
  CR_CHECK_EQ(cell.view(cr::CapacityView::Installed).value(), cr::Amount(130));
  CR_CHECK(cell.conflicts.empty());
}

CR_TEST(reconcile, an_authoritative_source_reporting_unknown_blocks_the_view) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  append_all(engine.value(),
             {builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::ObservationStream)
                  .instance("meter-1")
                  .unknown(cr::CapacityView::Observed, cr::UnknownReason::SourceUnavailable),
              builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::AssetRegistry)
                  .instance("asset-registry-1")
                  .known(cr::CapacityView::Observed, 90)});
  auto run = engine.value().reconcile(
      crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 500));
  CR_REQUIRE_OK(run);
  const cr::ReconciliationCell& cell = run.value().cells().front();
  CR_CHECK(cell.view(cr::CapacityView::Observed).is_unknown());
  CR_CHECK_EQ(cell.view(cr::CapacityView::Observed).reason(),
              cr::UnknownReason::SourceUnavailable);
  CR_CHECK(cell.has_class(cr::DiscrepancyClass::Incomplete));
}

CR_TEST(reconcile, a_lower_ranked_silent_source_does_not_block_a_known_value) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  append_all(engine.value(),
             {builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::AssetRegistry)
                  .instance("asset-registry-1")
                  .known(cr::CapacityView::Installed, 120),
              builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::RackCapacity)
                  .instance("rack-capacity-1")
                  .unknown(cr::CapacityView::Installed, cr::UnknownReason::NotReported)});
  auto run = engine.value().reconcile(
      crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 500));
  CR_REQUIRE_OK(run);
  CR_CHECK_EQ(run.value().cells().front().view(cr::CapacityView::Installed).value(),
              cr::Amount(120));
}

CR_TEST(reconcile, subtree_rollup_sums_known_contributors) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  for (int row = 1; row <= 3; ++row) {
    const std::string scope = "site-a/hall-1/row-" + std::to_string(row);
    append_all(engine.value(),
               {builder(scope, cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                    .source(cr::EvidenceSource::RackCapacity)
                    .instance("rack-" + std::to_string(row))
                    .known(cr::CapacityView::Installed, 100),
                builder(scope, cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                    .source(cr::EvidenceSource::ObservationStream)
                    .instance("meter-" + std::to_string(row))
                    .known(cr::CapacityView::Observed, 100)});
  }
  cr::RunRequest request = crtest::exact_request("site-a/hall-1", 0, 500);
  request.scopes.front().mode = cr::ScopeSelectionMode::SubtreeRollup;
  request.generation = engine.value().generation();
  auto run = engine.value().reconcile(request);
  CR_REQUIRE_OK(run);
  CR_REQUIRE(run.value().cells().size() == 1);
  const cr::ReconciliationCell& cell = run.value().cells().front();
  CR_CHECK_EQ(cell.view(cr::CapacityView::Installed).value(), cr::Amount(300));
  CR_CHECK_EQ(cell.view(cr::CapacityView::Observed).value(), cr::Amount(300));
  CR_CHECK(cell.rolled_up);
  CR_CHECK_EQ(cell.rollup_contributors, std::uint32_t(3));
  CR_CHECK_EQ(cell.primary_class(), cr::DiscrepancyClass::Agrees);
}

CR_TEST(reconcile, rollup_missing_a_descendant_is_unknown_not_a_smaller_total) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  for (int row = 1; row <= 3; ++row) {
    const std::string scope = "site-a/hall-1/row-" + std::to_string(row);
    append_all(engine.value(),
               {builder(scope, cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                    .source(cr::EvidenceSource::RackCapacity)
                    .instance("rack-" + std::to_string(row))
                    .known(cr::CapacityView::Installed, 100)});
    if (row <= 2) {
      append_all(engine.value(),
                 {builder(scope, cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                      .source(cr::EvidenceSource::ObservationStream)
                      .instance("meter-" + std::to_string(row))
                      .known(cr::CapacityView::Observed, 100)});
    }
  }
  cr::RunRequest request = crtest::exact_request("site-a/hall-1", 0, 500);
  request.scopes.front().mode = cr::ScopeSelectionMode::SubtreeRollup;
  request.generation = engine.value().generation();
  auto run = engine.value().reconcile(request);
  CR_REQUIRE_OK(run);
  const cr::ReconciliationCell& cell = run.value().cells().front();
  CR_CHECK_EQ(cell.view(cr::CapacityView::Installed).value(), cr::Amount(300));
  // 200 would be the partial sum. Reporting it would silently under-report.
  CR_CHECK(cell.view(cr::CapacityView::Observed).is_unknown());
  CR_CHECK_EQ(cell.view(cr::CapacityView::Observed).reason(),
              cr::UnknownReason::PartialRollup);
  // rollup_missing counts (contributing scope, view) contributions that could
  // not be established. Three scopes times six views is eighteen slots; the two
  // observed and three installed contributions are present, so thirteen are
  // missing. The exact number is asserted so that a change in the meaning of
  // the field cannot pass unnoticed.
  CR_CHECK_EQ(cell.rollup_missing, std::uint32_t(13));
  CR_CHECK(cell.has_class(cr::DiscrepancyClass::Incomplete));
}

CR_TEST(reconcile, rollup_does_not_cross_scope_selection) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  append_all(engine.value(),
             {builder("site-a/hall-1/row-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::RackCapacity)
                  .instance("rack-1")
                  .known(cr::CapacityView::Installed, 100),
              builder("site-a/hall-2/row-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::RackCapacity)
                  .instance("rack-2")
                  .known(cr::CapacityView::Installed, 200)});
  auto run = engine.value().reconcile(
      crtest::exact_request("site-a/hall-1/row-1", engine.value().generation().value(), 500));
  CR_REQUIRE_OK(run);
  CR_REQUIRE(run.value().cells().size() == 1);
  CR_CHECK_EQ(run.value().cells().front().view(cr::CapacityView::Installed).value(),
              cr::Amount(100));
}

CR_TEST(reconcile, unit_separation_is_reported_and_never_summed) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  append_all(engine.value(),
             {builder("site-a/hall-1", cr::CapacityDimension::Space, cr::Unit::RackUnit)
                  .source(cr::EvidenceSource::SpaceCapacity)
                  .instance("space-1")
                  .known(cr::CapacityView::Installed, 42),
              builder("site-a/hall-1", cr::CapacityDimension::Space, cr::Unit::SquareMilliMetre)
                  .source(cr::EvidenceSource::SpaceCapacity)
                  .instance("space-1")
                  .known(cr::CapacityView::Installed, 5000000)});
  auto run = engine.value().reconcile(
      crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 500));
  CR_REQUIRE_OK(run);
  CR_REQUIRE(run.value().cells().size() == 2);
  for (const cr::ReconciliationCell& cell : run.value().cells()) {
    CR_CHECK(cell.has_class(cr::DiscrepancyClass::UnitSeparated));
    CR_CHECK_EQ(cell.sibling_units.size(), std::size_t(1));
    CR_CHECK(cell.view(cr::CapacityView::Installed).value() == 42 ||
             cell.view(cr::CapacityView::Installed).value() == 5000000);
  }
}

CR_TEST(reconcile, stale_evidence_is_rejected_by_default) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  append_all(engine.value(),
             {builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::ObservationStream)
                  .instance("meter-1")
                  .observed_at(100)
                  .valid_until(200)
                  .known(cr::CapacityView::Observed, 100),
              builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::RackCapacity)
                  .instance("rack-1")
                  .observed_at(100)
                  .valid_until(10000)
                  .known(cr::CapacityView::Installed, 100)});
  // Evaluate after the observation expired.
  auto run = engine.value().reconcile(
      crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 900));
  CR_REQUIRE_OK(run);
  const cr::ReconciliationCell& cell = run.value().cells().front();
  CR_CHECK(cell.view(cr::CapacityView::Observed).is_unknown());
  CR_CHECK_EQ(cell.view(cr::CapacityView::Observed).reason(), cr::UnknownReason::Expired);
  CR_CHECK(cell.has_class(cr::DiscrepancyClass::Stale));
  CR_CHECK_EQ(run.value().resolution(), cr::ResolutionState::Stale);
}

CR_TEST(reconcile, stale_evidence_may_be_used_when_policy_says_so_and_is_still_marked) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  cr::ReconciliationPolicy policy = cr::ReconciliationPolicy::standard();
  policy.stale_evidence = cr::StaleEvidencePolicy::UseButMarkStale;
  auto status = engine.value().set_policy(policy, engine.value().generation(),
                                          engine.value().incarnation());
  CR_REQUIRE_OK(status);

  append_all(engine.value(),
             {builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::ObservationStream)
                  .instance("meter-1")
                  .observed_at(100)
                  .valid_until(200)
                  .known(cr::CapacityView::Observed, 100),
              builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::RackCapacity)
                  .instance("rack-1")
                  .observed_at(100)
                  .valid_until(10000)
                  .known(cr::CapacityView::Installed, 100)});

  auto run = engine.value().reconcile(
      crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 900));
  CR_REQUIRE_OK(run);
  const cr::ReconciliationCell& cell = run.value().cells().front();
  CR_CHECK_EQ(cell.view(cr::CapacityView::Observed).value(), cr::Amount(100));
  // The value is used, but the cell still says so: a stale answer can never be
  // mistaken for a current one.
  CR_CHECK(cell.has_class(cr::DiscrepancyClass::Stale));
}

CR_TEST(reconcile, age_by_generation_is_fenced) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  cr::ReconciliationPolicy policy = cr::ReconciliationPolicy::standard();
  policy.max_generation_lag = 0;  // disabled: only min_evidence_generation applies
  CR_REQUIRE_OK(engine.value().set_policy(policy, engine.value().generation(),
                                          engine.value().incarnation()));
  append_all(engine.value(),
             {builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::RackCapacity)
                  .instance("rack-1")
                  .generation(0)
                  .known(cr::CapacityView::Installed, 100)});

  cr::RunRequest request =
      crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 500);
  request.min_evidence_generation = cr::Generation{1};
  auto run = engine.value().reconcile(request);
  CR_REQUIRE_OK(run);
  const cr::ReconciliationCell& cell = run.value().cells().front();
  CR_CHECK(cell.view(cr::CapacityView::Installed).is_unknown());
  CR_CHECK_EQ(cell.view(cr::CapacityView::Installed).reason(),
              cr::UnknownReason::GenerationTooOld);
  CR_CHECK(cell.has_class(cr::DiscrepancyClass::GenerationRegression));
  CR_CHECK_EQ(run.value().resolution(), cr::ResolutionState::Stale);
}

CR_TEST(reconcile, evidence_from_beyond_the_run_generation_is_refused) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  append_all(engine.value(),
             {builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::RackCapacity)
                  .instance("rack-1")
                  .generation(0)
                  .known(cr::CapacityView::Installed, 100)});
  // The engine is now at generation 1; a request that claims generation 0 must
  // not consume evidence stamped 0 only by accident -- it is refused because the
  // request's generation is stale, and it is refused for the future case below.
  cr::RunRequest request = crtest::exact_request("site-a/hall-1", 0, 500);
  auto stale = engine.value().reconcile(request);
  CR_REQUIRE_ERR(stale, cr::ErrorCode::StaleGeneration);
}

CR_TEST(reconcile, conflicting_clock_domains_are_refused) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  auto other_domain = cr::ClockDomain::create("other-domain").value();
  auto item = cr::EvidenceItem::create(
      cr::EvidenceId::generate(), cr::EvidenceSource::RackCapacity, "rack-1",
      cr::ScopeIdentity::parse("site-a/hall-1").value(), crtest::power_key(),
      cr::CapacityView::Installed, cr::Quantity::known(1), cr::EvidenceStatus::Accepted,
      cr::EvidenceStamp{cr::Generation{0}, cr::Revision{1}, cr::Epoch{0},
                        cr::IncarnationId::generate(), cr::Tick{100}, cr::Tick{900},
                        other_domain});
  CR_REQUIRE_OK(item);
  auto appended = engine.value().append_evidence({item.value()}, engine.value().generation(),
                                                 engine.value().incarnation());
  CR_REQUIRE_OK(appended);
  auto run = engine.value().reconcile(
      crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 500));
  CR_REQUIRE_ERR(run, cr::ErrorCode::InvalidArgument);
}

CR_TEST(reconcile, arithmetic_overflow_makes_the_cell_explicit) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  append_all(engine.value(),
             {builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::RackCapacity)
                  .instance("rack-1")
                  .known(cr::CapacityView::Installed, cr::amount_max()),
              builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::ObservationStream)
                  .instance("meter-1")
                  .known(cr::CapacityView::Observed, cr::amount_min())});
  auto run = engine.value().reconcile(
      crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 500));
  CR_REQUIRE_OK(run);
  const cr::ReconciliationCell& cell = run.value().cells().front();
  // installed - observed overflows, and the cell says exactly that instead of
  // wrapping to a plausible-looking negative capacity.
  CR_CHECK(cell.has_class(cr::DiscrepancyClass::ArithmeticOverflow));
  CR_CHECK_EQ(cell.primary_class(), cr::DiscrepancyClass::ArithmeticOverflow);
  CR_CHECK(cell.observed_gap.is_unknown());
}

CR_TEST(reconcile, rollup_overflow_is_reported_not_wrapped) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  append_all(engine.value(),
             {builder("site-a/hall-1/row-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::RackCapacity)
                  .instance("rack-1")
                  .known(cr::CapacityView::Installed, cr::amount_max()),
              builder("site-a/hall-1/row-2", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::RackCapacity)
                  .instance("rack-2")
                  .known(cr::CapacityView::Installed, cr::amount_max())});
  cr::RunRequest request = crtest::exact_request("site-a/hall-1", 0, 500);
  request.scopes.front().mode = cr::ScopeSelectionMode::SubtreeRollup;
  request.generation = engine.value().generation();
  auto run = engine.value().reconcile(request);
  CR_REQUIRE_OK(run);
  const cr::ReconciliationCell& cell = run.value().cells().front();
  CR_CHECK(cell.view(cr::CapacityView::Installed).is_unknown());
  CR_CHECK_EQ(cell.view(cr::CapacityView::Installed).reason(),
              cr::UnknownReason::PartialRollup);
}

CR_TEST(reconcile, reservation_exceeding_installed_is_classified) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  append_all(engine.value(),
             {builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::RackCapacity)
                  .instance("rack-1")
                  .known(cr::CapacityView::Installed, 100),
              builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::ReservationRegister)
                  .instance("reservations")
                  .known(cr::CapacityView::Reserved, 150)});
  auto run = engine.value().reconcile(
      crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 500));
  CR_REQUIRE_OK(run);
  const cr::ReconciliationCell& cell = run.value().cells().front();
  CR_CHECK(cell.has_class(cr::DiscrepancyClass::ReservationExceedsInstalled));
  CR_CHECK_EQ(cell.reservation_pressure.value(), cr::Amount(50));
  CR_CHECK(cell.has_class(cr::DiscrepancyClass::UnexplainedResidual));
}

CR_TEST(reconcile, reservations_below_nameplate_are_not_an_anomaly) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  append_all(engine.value(),
             {builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::RackCapacity)
                  .instance("rack-1")
                  .known(cr::CapacityView::Installed, 100),
              builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::ReservationRegister)
                  .instance("reservations")
                  .known(cr::CapacityView::Reserved, 20)});
  auto run = engine.value().reconcile(
      crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 500));
  CR_REQUIRE_OK(run);
  const cr::ReconciliationCell& cell = run.value().cells().front();
  CR_CHECK(!cell.has_class(cr::DiscrepancyClass::UnexplainedResidual));
  CR_CHECK_EQ(cell.reservation_pressure.value(), cr::Amount(-80));
}

CR_TEST(reconcile, declared_allocatable_above_usable_is_overstated) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  append_all(engine.value(),
             {builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::CoolingCapacity)
                  .instance("cooling-1")
                  .known(cr::CapacityView::Usable, 100),
              builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::ReservationRegister)
                  .instance("reservations")
                  .known(cr::CapacityView::Reserved, 0),
              builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::FacilityCapacity)
                  .instance("facility-1")
                  .known(cr::CapacityView::Allocatable, 140)});
  auto run = engine.value().reconcile(
      crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 500));
  CR_REQUIRE_OK(run);
  const cr::ReconciliationCell& cell = run.value().cells().front();
  CR_CHECK(cell.has_class(cr::DiscrepancyClass::AllocatableOverstated));
  CR_CHECK_EQ(cell.allocatable_skew.value(), cr::Amount(40));
  CR_CHECK_EQ(cell.headroom_gap.value(), cr::Amount(-40));
  CR_CHECK(cell.has_class(cr::DiscrepancyClass::UnexplainedResidual));
}

CR_TEST(reconcile, tolerance_bands_suppress_small_differences) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  cr::ReconciliationPolicy policy = cr::ReconciliationPolicy::standard();
  cr::ToleranceBand band;
  band.absolute = 50;
  policy.set_tolerance(crtest::power_key(), band);
  CR_REQUIRE_OK(engine.value().set_policy(policy, engine.value().generation(),
                                          engine.value().incarnation()));

  append_all(engine.value(),
             {builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::RackCapacity)
                  .instance("rack-1")
                  .known(cr::CapacityView::Installed, 1000),
              builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::ObservationStream)
                  .instance("meter-1")
                  .known(cr::CapacityView::Observed, 960)});
  auto run = engine.value().reconcile(
      crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 500));
  CR_REQUIRE_OK(run);
  const cr::ReconciliationCell& cell = run.value().cells().front();
  // The headline says the cell agrees, and the exact residual is still there.
  CR_CHECK_EQ(cell.primary_class(), cr::DiscrepancyClass::Agrees);
  CR_CHECK_EQ(cell.observed_gap.value(), cr::Amount(40));
  // The difference is still recorded, with the tolerance that absorbed it and
  // an explicit flag saying it did not exceed it. Nothing is hidden.
  bool saw_tolerance = false;
  for (const cr::Finding& finding : cell.findings) {
    if (finding.target == cr::ReasonTarget::ObservedGap) {
      saw_tolerance = true;
      CR_CHECK_EQ(finding.tolerance, cr::Amount(50));
      CR_CHECK(!finding.exceeds_tolerance);
      CR_CHECK_EQ(finding.magnitude.value_or(0), cr::Amount(40));
    }
  }
  CR_CHECK(saw_tolerance);
  CR_CHECK(!cell.has_class(cr::DiscrepancyClass::UnexplainedResidual));
}

CR_TEST(reconcile, partial_evidence_is_never_silently_complete) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  // The most authoritative statement for the observed view declares itself
  // partial. The value is still used -- it is the best available -- but the
  // cell must say that the view is not fully established.
  std::vector<cr::Result<cr::EvidenceItem>> items = healthy_cell("site-a/hall-1", 100);
  items.push_back(builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                      .source(cr::EvidenceSource::ObservationStream)
                      .instance("meter-partial")
                      .status(cr::EvidenceStatus::Partial)
                      .known(cr::CapacityView::Observed, 100));
  append_all(engine.value(), items);
  auto run = engine.value().reconcile(
      crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 500));
  CR_REQUIRE_OK(run);
  const cr::ReconciliationCell& cell = run.value().cells().front();
  CR_CHECK(cell.views.partial[cr::view_index(cr::CapacityView::Observed)]);
  CR_CHECK(cell.has_class(cr::DiscrepancyClass::Incomplete));
  CR_CHECK_EQ(cell.primary_class(), cr::DiscrepancyClass::Incomplete);
}

CR_TEST(reconcile, unsupported_and_unavailable_statuses_become_unknowns) {
  for (const auto pair : {std::make_pair(cr::EvidenceStatus::Unsupported,
                                         cr::UnknownReason::Unsupported),
                          std::make_pair(cr::EvidenceStatus::Unavailable,
                                         cr::UnknownReason::SourceUnavailable),
                          std::make_pair(cr::EvidenceStatus::Expired,
                                         cr::UnknownReason::Expired)}) {
    auto engine = crtest::make_engine();
    CR_REQUIRE_OK(engine);
    append_all(engine.value(),
               {builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                    .source(cr::EvidenceSource::ObservationStream)
                    .instance("meter-1")
                    .status(pair.first)
                    .known(cr::CapacityView::Observed, 100)});
    auto run = engine.value().reconcile(
        crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 500));
    CR_REQUIRE_OK(run);
    const cr::ReconciliationCell& cell = run.value().cells().front();
    CR_CHECK(cell.view(cr::CapacityView::Observed).is_unknown());
    CR_CHECK_EQ(cell.view(cr::CapacityView::Observed).reason(), pair.second);
  }
}

CR_TEST(reconcile, a_scope_with_no_evidence_produces_no_cell) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  append_all(engine.value(), healthy_cell("site-a/hall-1", 100));
  auto run = engine.value().reconcile(
      crtest::exact_request("site-z/hall-9", engine.value().generation().value(), 500));
  CR_REQUIRE_OK(run);
  CR_CHECK(run.value().cells().empty());
  CR_CHECK_EQ(run.value().resolution(), cr::ResolutionState::Explained);
}

CR_TEST(reconcile, duplicate_scope_selection_is_refused) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  append_all(engine.value(), healthy_cell("site-a/hall-1", 100));
  cr::RunRequest request =
      crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 500);
  request.scopes.push_back(request.scopes.front());
  CR_REQUIRE_ERR(engine.value().reconcile(request), cr::ErrorCode::AlreadyExists);
}

CR_TEST(reconcile, an_exact_scope_and_an_enclosing_rollup_coexist_as_separate_cells) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  append_all(engine.value(), healthy_cell("site-a/hall-1/row-1", 100));
  cr::RunRequest request =
      crtest::exact_request("site-a/hall-1/row-1", engine.value().generation().value(), 500);
  cr::ScopeSelection rolled;
  rolled.scope = cr::ScopeIdentity::parse("site-a/hall-1").value();
  rolled.mode = cr::ScopeSelectionMode::SubtreeRollup;
  request.scopes.push_back(std::move(rolled));
  auto run = engine.value().reconcile(request);
  CR_REQUIRE_OK(run);
  // Two distinct cells: the detail at row-1 and the rollup at hall-1. The
  // rollup is not silently merged into the exact cell, and the exact cell is
  // not silently dropped.
  CR_REQUIRE(run.value().cells().size() == 2);
  CR_CHECK_EQ(run.value().cells()[0].scope.to_string(), std::string("site-a/hall-1"));
  CR_CHECK(run.value().cells()[0].rolled_up);
  CR_CHECK_EQ(run.value().cells()[1].scope.to_string(), std::string("site-a/hall-1/row-1"));
  CR_CHECK(!run.value().cells()[1].rolled_up);
  CR_CHECK_EQ(run.value().cells()[0].view(cr::CapacityView::Installed).value(), cr::Amount(100));
  CR_CHECK_EQ(run.value().cells()[1].view(cr::CapacityView::Installed).value(), cr::Amount(100));
}

CR_TEST(reconcile, run_digest_is_independent_of_evidence_insertion_order) {
  std::vector<cr::Result<cr::EvidenceItem>> items;
  for (int row = 1; row <= 6; ++row) {
    const std::string scope = "site-a/hall-1/row-" + std::to_string(row);
    auto base = healthy_cell(scope, 100 * row);
    items.insert(items.end(), base.begin(), base.end());
  }

  cr::Digest reference;
  std::mt19937_64 engine_seed(97);
  for (int trial = 0; trial < 25; ++trial) {
    std::vector<cr::EvidenceItem> flat;
    for (const auto& item : items) {
      CR_REQUIRE_OK(item);
      flat.push_back(item.value());
    }
    std::shuffle(flat.begin(), flat.end(), engine_seed);
    auto set = cr::EvidenceSet::build(std::move(flat), 10000);
    CR_REQUIRE_OK(set);
    auto attributions = cr::AttributionSet::build({}, 10);
    CR_REQUIRE_OK(attributions);

    cr::RunRequest request;
    for (int row = 1; row <= 6; ++row) {
      cr::ScopeSelection selection;
      selection.scope = cr::ScopeIdentity::parse("site-a/hall-1/row-" + std::to_string(row)).value();
      request.scopes.push_back(std::move(selection));
    }
    // Reverse the scope order on odd trials: canonical ordering must absorb it.
    if (trial % 2 == 1) {
      std::reverse(request.scopes.begin(), request.scopes.end());
    }
    request.generation = cr::Generation{1};
    request.evaluation_instant = cr::Tick{500};
    request.clock_domain = crtest::test_domain();

    auto engine = crtest::make_engine();
    CR_REQUIRE_OK(engine);
    append_all(engine.value(), items);
    auto run = engine.value().reconcile(request);
    CR_REQUIRE_OK(run);
    if (trial == 0) {
      reference = run.value().run_digest();
    } else {
      CR_CHECK(run.value().run_digest() == reference);
    }
  }
}

CR_TEST(reconcile, two_runs_over_identical_inputs_have_equal_digests_and_different_ids) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  append_all(engine.value(), healthy_cell("site-a/hall-1", 100));
  const auto request =
      crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 500);
  auto first = engine.value().reconcile(request);
  auto second = engine.value().reconcile(request);
  CR_REQUIRE_OK(first);
  CR_REQUIRE_OK(second);
  CR_CHECK(first.value().run_digest() == second.value().run_digest());
  CR_CHECK(first.value().id() != second.value().id());
  CR_CHECK_EQ(first.value().canonical_bytes(), second.value().canonical_bytes());
}

CR_TEST(reconcile, a_request_cannot_smuggle_a_different_policy) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  append_all(engine.value(), healthy_cell("site-a/hall-1", 100));

  cr::ReconciliationPolicy other = cr::ReconciliationPolicy::standard();
  other.max_generation_lag = 999;
  cr::RunRequest request =
      crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 500);
  request.policy = other;
  auto run = engine.value().reconcile(request);
  CR_REQUIRE_OK(run);
  // The engine's policy is authoritative, so the run records the engine's
  // digest, not the caller's.
  CR_CHECK(run.value().policy_digest() == engine.value().options().policy.digest());
  CR_CHECK(!(run.value().policy_digest() == other.digest()));
}

CR_TEST(reconcile, validation_precedence_is_the_declared_order) {
  // The declared order is the enumerator order of DiscrepancyClass, and it is a
  // contract: a cell's primary classification is the first applicable entry.
  const std::pair<cr::DiscrepancyClass, const char*> expected[] = {
      {cr::DiscrepancyClass::ArithmeticOverflow, "arithmetic_overflow"},
      {cr::DiscrepancyClass::EvidenceConflict, "evidence_conflict"},
      {cr::DiscrepancyClass::NoEvidence, "no_evidence"},
      {cr::DiscrepancyClass::GenerationRegression, "generation_regression"},
      {cr::DiscrepancyClass::Stale, "stale"},
      {cr::DiscrepancyClass::UnitSeparated, "unit_separated"},
      {cr::DiscrepancyClass::Incomplete, "incomplete"},
      {cr::DiscrepancyClass::ObservedShortfall, "observed_shortfall"},
      {cr::DiscrepancyClass::ObservationExceedsInstalled, "observation_exceeds_installed"},
      {cr::DiscrepancyClass::PlannedGap, "planned_gap"},
      {cr::DiscrepancyClass::UnplannedInstall, "unplanned_install"},
      {cr::DiscrepancyClass::UsableDerated, "usable_derated"},
      {cr::DiscrepancyClass::UsableExceedsObserved, "usable_exceeds_observed"},
      {cr::DiscrepancyClass::AllocatableOverstated, "allocatable_overstated"},
      {cr::DiscrepancyClass::AllocatableUnderstated, "allocatable_understated"},
      {cr::DiscrepancyClass::ReservationExceedsInstalled, "reservation_exceeds_installed"},
      {cr::DiscrepancyClass::ReservationExceedsAllocatable, "reservation_exceeds_allocatable"},
      {cr::DiscrepancyClass::UnexplainedResidual, "unexplained_residual"},
      {cr::DiscrepancyClass::Agrees, "agrees"},
  };
  std::uint8_t ordinal = 0;
  for (const auto& entry : expected) {
    CR_CHECK_EQ(static_cast<std::uint8_t>(entry.first), ordinal);
    CR_CHECK_EQ(std::string(cr::to_string(entry.first)), std::string(entry.second));
    ++ordinal;
  }
  CR_CHECK_EQ(std::size_t(ordinal), std::size_t(19));
}

CR_TEST(reconcile, conflict_outranks_a_shortfall_in_the_primary_classification) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  append_all(engine.value(),
             {builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::ObservationStream)
                  .instance("meter-1")
                  .known(cr::CapacityView::Installed, 100),
              builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::ObservationStream)
                  .instance("meter-2")
                  .known(cr::CapacityView::Installed, 200),
              builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::ObservationStream)
                  .instance("meter-3")
                  .known(cr::CapacityView::Observed, 10)});
  auto run = engine.value().reconcile(
      crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 500));
  CR_REQUIRE_OK(run);
  const cr::ReconciliationCell& cell = run.value().cells().front();
  CR_CHECK_EQ(cell.primary_class(), cr::DiscrepancyClass::EvidenceConflict);
  // The conflict does not hide the fact that something else is also wrong.
  CR_CHECK(cell.findings.size() >= 1);
}

CR_TEST(reconcile, straddling_residuals_are_exact) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  append_all(engine.value(),
             {builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::FacilityCapacity)
                  .instance("facility-1")
                  .known(cr::CapacityView::Planned, 1000),
              builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::RackCapacity)
                  .instance("rack-1")
                  .known(cr::CapacityView::Installed, 900),
              builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::ObservationStream)
                  .instance("meter-1")
                  .known(cr::CapacityView::Observed, 800),
              builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::CoolingCapacity)
                  .instance("cooling-1")
                  .known(cr::CapacityView::Usable, 700),
              builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::ReservationRegister)
                  .instance("reservations")
                  .known(cr::CapacityView::Reserved, 300),
              builder("site-a/hall-1", cr::CapacityDimension::Power, cr::Unit::MilliWatt)
                  .source(cr::EvidenceSource::FacilityCapacity)
                  .instance("facility-1")
                  .known(cr::CapacityView::Allocatable, 400)});
  auto run = engine.value().reconcile(
      crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 500));
  CR_REQUIRE_OK(run);
  const cr::ReconciliationCell& cell = run.value().cells().front();
  CR_CHECK_EQ(cell.planned_gap.value(), cr::Amount(100));
  CR_CHECK_EQ(cell.observed_gap.value(), cr::Amount(100));
  CR_CHECK_EQ(cell.derate_gap.value(), cr::Amount(100));
  CR_CHECK_EQ(cell.headroom_gap.value(), cr::Amount(300));
  CR_CHECK_EQ(cell.reservation_pressure.value(), cr::Amount(-600));
  CR_CHECK_EQ(cell.reservation_overhang.value(), cr::Amount(-100));
  CR_CHECK_EQ(cell.derived_allocatable.value(), cr::Amount(400));
  CR_CHECK_EQ(cell.allocatable_skew.value(), cr::Amount(0));
  CR_CHECK(cell.has_class(cr::DiscrepancyClass::PlannedGap));
  CR_CHECK(cell.has_class(cr::DiscrepancyClass::ObservedShortfall));
  CR_CHECK(cell.has_class(cr::DiscrepancyClass::UsableDerated));
}
