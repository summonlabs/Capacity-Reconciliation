// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Independent reference models.
//
// These do not call the runtime's arithmetic, classification or rollup code.
// They recompute the same answers a second, deliberately naive way -- direct
// table lookups and hand-written loops -- so that an error in the runtime's
// implementation cannot hide behind itself. Where the two disagree, the test
// fails and names the cell.

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "summon/capacity_reconciliation/engine.hpp"
#include "test_helpers.hpp"
#include "test_support.hpp"

namespace cr = summon::capacity_reconciliation;

namespace {

/// Reference view vector: a plain array of optional values, one per view.
struct ReferenceViews {
  bool present[6] = {false, false, false, false, false, false};
  long long value[6] = {0, 0, 0, 0, 0, 0};
};

/// Reference classification of a cell, computed by hand from the view vector.
/// Deliberately written as a flat sequence of independent conditions rather
/// than as a table walk, so that it shares no structure with the runtime.
struct ReferenceVerdict {
  bool observed_shortfall = false;
  bool observation_exceeds_installed = false;
  bool planned_gap = false;
  bool unplanned_install = false;
  bool usable_derated = false;
  bool usable_exceeds_observed = false;
  bool allocatable_overstated = false;
  bool allocatable_understated = false;
  bool reservation_exceeds_installed = false;
  bool reservation_exceeds_allocatable = false;
  bool agrees = false;
};

ReferenceVerdict classify_reference(const ReferenceViews& views) {
  ReferenceVerdict verdict;
  const bool has_planned = views.present[0];
  const bool has_reserved = views.present[1];
  const bool has_installed = views.present[2];
  const bool has_observed = views.present[3];
  const bool has_usable = views.present[4];
  const bool has_allocatable = views.present[5];

  if (has_planned && has_installed) {
    if (views.value[0] > views.value[2]) {
      verdict.planned_gap = true;
    }
    if (views.value[2] > views.value[0]) {
      verdict.unplanned_install = true;
    }
  }
  if (has_installed && has_observed) {
    if (views.value[2] > views.value[3]) {
      verdict.observed_shortfall = true;
    }
    if (views.value[3] > views.value[2]) {
      verdict.observation_exceeds_installed = true;
    }
  }
  if (has_observed && has_usable) {
    if (views.value[3] > views.value[4]) {
      verdict.usable_derated = true;
    }
    if (views.value[4] > views.value[3]) {
      verdict.usable_exceeds_observed = true;
    }
  }
  // Declared allocatable must never exceed usable capacity. Holding headroom
  // back is normal when reservations exist, so only the exceeding direction is
  // a discrepancy here.
  if (has_usable && has_allocatable && views.value[5] > views.value[4]) {
    verdict.allocatable_overstated = true;
  }
  // Declared allocatable must equal usable minus reserved, in both directions.
  if (has_usable && has_reserved && has_allocatable) {
    const long long derived = views.value[4] - views.value[1];
    if (views.value[5] > derived) {
      verdict.allocatable_overstated = true;
    }
    if (views.value[5] < derived) {
      verdict.allocatable_understated = true;
    }
  }
  if (has_reserved && has_installed && views.value[1] > views.value[2]) {
    verdict.reservation_exceeds_installed = true;
  }
  if (has_reserved && has_allocatable && views.value[1] > views.value[5]) {
    verdict.reservation_exceeds_allocatable = true;
  }
  verdict.agrees = !verdict.observed_shortfall && !verdict.observation_exceeds_installed &&
                   !verdict.planned_gap && !verdict.unplanned_install &&
                   !verdict.usable_derated && !verdict.usable_exceeds_observed &&
                   !verdict.allocatable_overstated && !verdict.allocatable_understated &&
                   !verdict.reservation_exceeds_installed &&
                   !verdict.reservation_exceeds_allocatable;
  return verdict;
}

ReferenceViews views_of(const cr::ReconciliationCell& cell) {
  ReferenceViews views;
  for (std::size_t i = 0; i < 6; ++i) {
    const cr::Quantity& quantity = cell.view(static_cast<cr::CapacityView>(i));
    if (quantity.is_known()) {
      views.present[i] = true;
      views.value[i] = static_cast<long long>(quantity.value());
    }
  }
  return views;
}

/// Returns an empty string when the runtime agrees with the reference, or the
/// name of the first disagreeing classification.
std::string reference_disagreement(const cr::ReconciliationCell& cell) {
  const ReferenceViews views = views_of(cell);
  const ReferenceVerdict verdict = classify_reference(views);
  bool ok = true;
  const struct {
    bool reference;
    cr::DiscrepancyClass runtime;
  } checks[] = {
      {verdict.observed_shortfall, cr::DiscrepancyClass::ObservedShortfall},
      {verdict.observation_exceeds_installed, cr::DiscrepancyClass::ObservationExceedsInstalled},
      {verdict.planned_gap, cr::DiscrepancyClass::PlannedGap},
      {verdict.unplanned_install, cr::DiscrepancyClass::UnplannedInstall},
      {verdict.usable_derated, cr::DiscrepancyClass::UsableDerated},
      {verdict.usable_exceeds_observed, cr::DiscrepancyClass::UsableExceedsObserved},
      {verdict.allocatable_overstated, cr::DiscrepancyClass::AllocatableOverstated},
      {verdict.allocatable_understated, cr::DiscrepancyClass::AllocatableUnderstated},
      {verdict.reservation_exceeds_installed, cr::DiscrepancyClass::ReservationExceedsInstalled},
      {verdict.reservation_exceeds_allocatable, cr::DiscrepancyClass::ReservationExceedsAllocatable},
  };
  // The reference is computed over *known* views only. The runtime additionally
  // raises cell-level findings (incomplete, stale, conflict, unit split) which
  // are outside the reference's scope, so only the arithmetic classes are
  // compared, and only when the referenced views are all known.
  bool all_known = true;
  for (bool present : views.present) {
    all_known = all_known && present;
  }
  if (!all_known) {
    return std::string();
  }
  for (const auto& check : checks) {
    if (check.reference != cell.has_class(check.runtime)) {
      return std::string("class ") + cr::to_string(check.runtime) + " reference=" +
             (check.reference ? "true" : "false") + " runtime=" +
             (cell.has_class(check.runtime) ? "true" : "false");
    }
  }
  if (verdict.agrees != cell.has_class(cr::DiscrepancyClass::Agrees)) {
    return std::string("agrees reference=") + (verdict.agrees ? "true" : "false") +
           " runtime=" + (cell.has_class(cr::DiscrepancyClass::Agrees) ? "true" : "false") +
           " primary=" + cr::to_string(cell.primary_class());
  }
  (void)ok;
  return std::string();
}

bool check_against_reference(const cr::ReconciliationCell& cell) {
  return reference_disagreement(cell).empty();
}

}  // namespace

CR_TEST(reference, residual_arithmetic_matches_a_hand_computed_table) {
  struct Row {
    long long planned;
    long long installed;
    long long observed;
    long long usable;
    long long reserved;
    long long allocatable;
  };
  const Row rows[] = {
      {100, 100, 100, 100, 0, 100},
      {100, 90, 80, 70, 30, 40},
      {0, 0, 0, 0, 0, 0},
      {1000, 1000, 900, 900, 0, 900},
      {500, 400, 400, 300, 250, 50},
      {-100, -100, -200, -200, -50, -150},
      {7, 7, 7, 7, 7, 0},
  };

  for (const Row& row : rows) {
    auto engine = crtest::make_engine();
    CR_REQUIRE_OK(engine);
    const struct {
      cr::CapacityView view;
      long long value;
    } values[] = {{cr::CapacityView::Planned, row.planned},
                  {cr::CapacityView::Installed, row.installed},
                  {cr::CapacityView::Observed, row.observed},
                  {cr::CapacityView::Usable, row.usable},
                  {cr::CapacityView::Reserved, row.reserved},
                  {cr::CapacityView::Allocatable, row.allocatable}};
    std::vector<cr::EvidenceItem> items;
    for (const auto& entry : values) {
      cr::EvidenceStamp stamp;
      stamp.generation = cr::Generation{0};
      stamp.revision = cr::Revision{1};
      stamp.incarnation = engine.value().incarnation();
      stamp.observed_at = cr::Tick{1};
      stamp.valid_until = cr::Tick{1000};
      stamp.clock_domain = crtest::test_domain();
      auto item = cr::EvidenceItem::create(
          cr::EvidenceId::generate(), cr::EvidenceSource::RackCapacity, "r",
          cr::ScopeIdentity::parse("site-a/hall-1").value(), crtest::power_key(), entry.view,
          cr::Quantity::known(entry.value), cr::EvidenceStatus::Accepted, stamp);
      CR_REQUIRE_OK(item);
      items.push_back(std::move(item.value()));
    }
    CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                                 engine.value().incarnation()));
    auto run = engine.value().reconcile(
        crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 500));
    CR_REQUIRE_OK(run);
    CR_REQUIRE(run.value().cells().size() == 1);
    const cr::ReconciliationCell& cell = run.value().cells().front();

    CR_CHECK_EQ(cell.planned_gap.value(), static_cast<cr::Amount>(row.planned - row.installed));
    CR_CHECK_EQ(cell.observed_gap.value(), static_cast<cr::Amount>(row.installed - row.observed));
    CR_CHECK_EQ(cell.derate_gap.value(), static_cast<cr::Amount>(row.observed - row.usable));
    CR_CHECK_EQ(cell.headroom_gap.value(),
                static_cast<cr::Amount>(row.usable - row.allocatable));
    CR_CHECK_EQ(cell.reservation_pressure.value(),
                static_cast<cr::Amount>(row.reserved - row.installed));
    CR_CHECK_EQ(cell.reservation_overhang.value(),
                static_cast<cr::Amount>(row.reserved - row.allocatable));
    CR_CHECK_EQ(cell.derived_allocatable.value(),
                static_cast<cr::Amount>(row.usable - row.reserved));
    CR_CHECK_EQ(cell.allocatable_skew.value(),
                static_cast<cr::Amount>(row.allocatable - (row.usable - row.reserved)));

    CR_CHECK_MSG(check_against_reference(cell),
                 reference_disagreement(cell) + " [planned=" + std::to_string(row.planned) +
                     " installed=" + std::to_string(row.installed) +
                     " observed=" + std::to_string(row.observed) +
                     " usable=" + std::to_string(row.usable) +
                     " reserved=" + std::to_string(row.reserved) +
                     " allocatable=" + std::to_string(row.allocatable) + "]");
  }
}

CR_TEST(reference, rollup_matches_an_independent_sum) {
  // Reference: read the per-scope cells back from a separate exact run and add
  // them up by hand.
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  const long long values[] = {11, 22, 33, 44, 55};
  std::vector<std::string> scopes;
  for (int i = 0; i < 5; ++i) {
    const std::string scope = "site-a/hall-1/row-" + std::to_string(i + 1);
    scopes.push_back(scope);
    cr::EvidenceStamp stamp;
    stamp.generation = cr::Generation{0};
    stamp.revision = cr::Revision{1};
    stamp.incarnation = engine.value().incarnation();
    stamp.observed_at = cr::Tick{1};
    stamp.valid_until = cr::Tick{1000};
    stamp.clock_domain = crtest::test_domain();
    std::vector<cr::EvidenceItem> items;
    for (const auto view : {cr::CapacityView::Installed, cr::CapacityView::Observed}) {
      auto item = cr::EvidenceItem::create(
          cr::EvidenceId::generate(), cr::EvidenceSource::RackCapacity, "r",
          cr::ScopeIdentity::parse(scope).value(), crtest::power_key(), view,
          cr::Quantity::known(values[i]), cr::EvidenceStatus::Accepted, stamp);
      CR_REQUIRE_OK(item);
      items.push_back(std::move(item.value()));
    }
    CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                                 engine.value().incarnation()));
  }

  // Exact run, to read the per-scope values independently.
  cr::RunRequest exact;
  for (const std::string& scope : scopes) {
    exact.scopes.push_back({cr::ScopeIdentity::parse(scope).value(), cr::ScopeSelectionMode::Exact});
  }
  exact.generation = engine.value().generation();
  exact.evaluation_instant = cr::Tick{500};
  exact.clock_domain = crtest::test_domain();
  auto exact_run = engine.value().reconcile(exact);
  CR_REQUIRE_OK(exact_run);
  long long expected_installed = 0;
  long long expected_observed = 0;
  for (const cr::ReconciliationCell& cell : exact_run.value().cells()) {
    expected_installed += cell.view(cr::CapacityView::Installed).value();
    expected_observed += cell.view(cr::CapacityView::Observed).value();
  }

  cr::RunRequest rolled;
  rolled.scopes.push_back({cr::ScopeIdentity::parse("site-a/hall-1").value(),
                           cr::ScopeSelectionMode::SubtreeRollup});
  rolled.generation = engine.value().generation();
  rolled.evaluation_instant = cr::Tick{500};
  rolled.clock_domain = crtest::test_domain();
  auto rolled_run = engine.value().reconcile(rolled);
  CR_REQUIRE_OK(rolled_run);
  CR_REQUIRE(rolled_run.value().cells().size() == 1);
  const cr::ReconciliationCell& cell = rolled_run.value().cells().front();
  CR_CHECK_EQ(cell.view(cr::CapacityView::Installed).value(),
              static_cast<cr::Amount>(expected_installed));
  CR_CHECK_EQ(cell.view(cr::CapacityView::Observed).value(),
              static_cast<cr::Amount>(expected_observed));
  CR_CHECK_EQ(cell.rollup_contributors, std::uint32_t(5));
}

CR_TEST(reference, attribution_arithmetic_matches_an_independent_subtraction) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  cr::EvidenceStamp stamp;
  stamp.generation = cr::Generation{0};
  stamp.revision = cr::Revision{1};
  stamp.incarnation = engine.value().incarnation();
  stamp.observed_at = cr::Tick{1};
  stamp.valid_until = cr::Tick{1000};
  stamp.clock_domain = crtest::test_domain();

  std::vector<cr::EvidenceItem> items;
  for (const auto pair : {std::make_pair(cr::CapacityView::Installed, 1000LL),
                          std::make_pair(cr::CapacityView::Observed, 800LL)}) {
    auto item = cr::EvidenceItem::create(
        cr::EvidenceId::generate(), cr::EvidenceSource::RackCapacity, "r",
        cr::ScopeIdentity::parse("site-a/hall-1").value(), crtest::power_key(), pair.first,
        cr::Quantity::known(pair.second), cr::EvidenceStatus::Accepted, stamp);
    CR_REQUIRE_OK(item);
    items.push_back(std::move(item.value()));
  }
  CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                               engine.value().incarnation()));

  const long long amounts[] = {50, 25, 100, -30};
  long long expected_attributed = 0;
  std::vector<cr::AttributionItem> attributions;
  for (long long amount : amounts) {
    auto built = crtest::make_attribution("site-a/hall-1", cr::CapacityDimension::Power,
                                          cr::Unit::MilliWatt, cr::ResidualKind::ObservedGap,
                                          amount, cr::EvidenceSource::CoolingCapacity, "c", 0, 1,
                                          1, 1000);
    CR_REQUIRE_OK(built);
    attributions.push_back(std::move(built.value()));
    expected_attributed += amount;
  }
  CR_REQUIRE_OK(engine.value().append_attributions(std::move(attributions),
                                                   engine.value().generation(),
                                                   engine.value().incarnation()));

  auto run = engine.value().reconcile(
      crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 500));
  CR_REQUIRE_OK(run);
  const cr::ReconciliationCell& cell = run.value().cells().front();
  const cr::ResidualExplanation* explanation =
      cell.explanation.find_residual(cr::ReasonTarget::ObservedGap);
  CR_REQUIRE(explanation != nullptr);
  CR_CHECK_EQ(explanation->raw.value(), cr::Amount(200));
  CR_CHECK_EQ(explanation->attributed.value(), static_cast<cr::Amount>(expected_attributed));
  CR_CHECK_EQ(explanation->unexplained.value(),
              static_cast<cr::Amount>(200 - expected_attributed));
}

CR_TEST(reference, precedence_selection_matches_an_independent_minimum_scan) {
  // Reference: scan the statements by hand, remember the smallest precedence
  // index observed, then check that only statements at that index decided the
  // value.
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  const cr::ReconciliationPolicy policy = cr::ReconciliationPolicy::standard();

  const struct {
    cr::EvidenceSource source;
    long long value;
  } statements[] = {
      {cr::EvidenceSource::CoolingCapacity, 10},
      {cr::EvidenceSource::FacilityCapacity, 20},
      {cr::EvidenceSource::ObservationStream, 30},
      {cr::EvidenceSource::OperatorDeclaration, 40},
  };
  std::size_t expected_index = policy.precedence().size();
  for (std::size_t i = 0; i < 4; ++i) {
    expected_index = std::min(expected_index, policy.precedence_of(statements[i].source));
  }
  long long expected_value = 0;
  for (const auto& statement : statements) {
    if (policy.precedence_of(statement.source) == expected_index) {
      expected_value = statement.value;
      break;
    }
  }

  cr::EvidenceStamp stamp;
  stamp.generation = cr::Generation{0};
  stamp.revision = cr::Revision{1};
  stamp.incarnation = engine.value().incarnation();
  stamp.observed_at = cr::Tick{1};
  stamp.valid_until = cr::Tick{1000};
  stamp.clock_domain = crtest::test_domain();
  std::vector<cr::EvidenceItem> items;
  for (const auto& statement : statements) {
    auto item = cr::EvidenceItem::create(
        cr::EvidenceId::generate(), statement.source, "instance",
        cr::ScopeIdentity::parse("site-a/hall-1").value(), crtest::power_key(),
        cr::CapacityView::Installed, cr::Quantity::known(statement.value),
        cr::EvidenceStatus::Accepted, stamp);
    CR_REQUIRE_OK(item);
    items.push_back(std::move(item.value()));
  }
  CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                               engine.value().incarnation()));

  auto run = engine.value().reconcile(
      crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 500));
  CR_REQUIRE_OK(run);
  CR_CHECK_EQ(run.value().cells().front().view(cr::CapacityView::Installed).value(),
              static_cast<cr::Amount>(expected_value));
  CR_REQUIRE(run.value().cells().front().conflicts.size() == 1);
  CR_CHECK(run.value().cells().front().conflicts.front().unresolvable_reason ==
           std::string("outranked_by_precedence"));
}

CR_TEST(reference, every_cell_agrees_with_the_reference_model_on_classification) {
  // A sweep across combinations, checked cell by cell against the reference.
  const long long candidates[] = {0, 50, 100};
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  std::size_t cell_index = 0;
  for (long long planned : candidates) {
    for (long long installed : candidates) {
      for (long long observed : candidates) {
        for (long long usable : candidates) {
          for (long long reserved : candidates) {
            for (long long allocatable : candidates) {
              const std::string scope = "site-a/row-" + std::to_string(cell_index);
              ++cell_index;
              cr::EvidenceStamp stamp;
              stamp.generation = cr::Generation{0};
              stamp.revision = cr::Revision{1};
              stamp.incarnation = engine.value().incarnation();
              stamp.observed_at = cr::Tick{1};
              stamp.valid_until = cr::Tick{1000};
              stamp.clock_domain = crtest::test_domain();
              const struct {
                cr::CapacityView view;
                long long value;
              } values[] = {{cr::CapacityView::Planned, planned},
                            {cr::CapacityView::Installed, installed},
                            {cr::CapacityView::Observed, observed},
                            {cr::CapacityView::Usable, usable},
                            {cr::CapacityView::Reserved, reserved},
                            {cr::CapacityView::Allocatable, allocatable}};
              std::vector<cr::EvidenceItem> items;
              for (const auto& entry : values) {
                auto item = cr::EvidenceItem::create(
                    cr::EvidenceId::generate(), cr::EvidenceSource::RackCapacity, "r",
                    cr::ScopeIdentity::parse(scope).value(), crtest::power_key(), entry.view,
                    cr::Quantity::known(entry.value), cr::EvidenceStatus::Accepted, stamp);
                CR_REQUIRE_OK(item);
                items.push_back(std::move(item.value()));
              }
              CR_REQUIRE_OK(engine.value().append_evidence(
                  std::move(items), engine.value().generation(), engine.value().incarnation()));
            }
          }
        }
      }
    }
  }

  cr::RunRequest request;
  for (std::size_t i = 0; i < cell_index; ++i) {
    request.scopes.push_back({cr::ScopeIdentity::parse("site-a/row-" + std::to_string(i)).value(),
                              cr::ScopeSelectionMode::Exact});
  }
  request.generation = engine.value().generation();
  request.evaluation_instant = cr::Tick{500};
  request.clock_domain = crtest::test_domain();
  auto run = engine.value().reconcile(request);
  CR_REQUIRE_OK(run);
  CR_CHECK_EQ(run.value().cells().size(), cell_index);
  for (const cr::ReconciliationCell& cell : run.value().cells()) {
    CR_CHECK_MSG(check_against_reference(cell),
                 "reference disagreement at " + cell.scope.to_string());
  }
}
