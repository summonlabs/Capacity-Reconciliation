// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Seeded randomized property tests.
//
// Every test here generates its input from a fixed seed, so a failure is
// reproducible, and asserts a property that must hold for *every* input, not
// just the one the author happened to think of.

#include <algorithm>
#include <random>
#include <string>
#include <vector>

#include "summon/capacity_reconciliation/engine.hpp"
#include "summon/capacity_reconciliation/report.hpp"
#include "test_helpers.hpp"
#include "test_support.hpp"

namespace cr = summon::capacity_reconciliation;

namespace {

class Rng {
 public:
  explicit Rng(std::uint64_t seed) : engine_(seed) {}

  std::uint64_t next() { return engine_(); }
  std::int64_t range(std::int64_t low, std::int64_t high) {
    return low + static_cast<std::int64_t>(engine_() % static_cast<std::uint64_t>(high - low + 1));
  }
  bool coin() { return (engine_() & 1u) != 0; }

 private:
  std::mt19937_64 engine_;
};

std::vector<cr::EvidenceItem> random_evidence(const std::string& scope, std::uint64_t generation,
                                              Rng* rng, const cr::IncarnationId& incarnation,
                                              bool allow_unknown) {
  std::vector<cr::EvidenceItem> items;
  const cr::CapacityView views[] = {cr::CapacityView::Planned,  cr::CapacityView::Reserved,
                                    cr::CapacityView::Installed, cr::CapacityView::Observed,
                                    cr::CapacityView::Usable,   cr::CapacityView::Allocatable};
  const cr::EvidenceSource sources[] = {
      cr::EvidenceSource::FacilityCapacity,   cr::EvidenceSource::RackCapacity,
      cr::EvidenceSource::SpaceCapacity,      cr::EvidenceSource::PowerCapacity,
      cr::EvidenceSource::CoolingCapacity,    cr::EvidenceSource::ReservationRegister,
      cr::EvidenceSource::AssetRegistry,      cr::EvidenceSource::RackRegistry,
      cr::EvidenceSource::LocationRegistry,   cr::EvidenceSource::ObservationStream,
      cr::EvidenceSource::OperatorDeclaration};
  for (cr::CapacityView view : views) {
    const int statements = static_cast<int>(rng->range(1, 4));
    for (int i = 0; i < statements; ++i) {
      cr::EvidenceStamp stamp;
      stamp.generation = cr::Generation{generation};
      stamp.revision = cr::Revision{static_cast<std::uint64_t>(rng->range(1, 3))};
      stamp.epoch = cr::Epoch{0};
      stamp.incarnation = incarnation;
      stamp.observed_at = cr::Tick{static_cast<std::uint64_t>(rng->range(1, 100))};
      stamp.valid_until = cr::Tick{static_cast<std::uint64_t>(rng->range(500, 1000))};
      stamp.clock_domain = crtest::test_domain();

      cr::Quantity value = cr::Quantity::known(rng->range(-1000, 1000));
      if (allow_unknown && rng->range(0, 9) == 0) {
        value = cr::Quantity::unknown(cr::UnknownReason::NotReported);
      }
      const auto source = sources[static_cast<std::size_t>(
          rng->range(0, static_cast<std::int64_t>(std::size(sources)) - 1))];
      auto item = cr::EvidenceItem::create(
          cr::EvidenceId::generate(), source, "instance-" + std::to_string(i),
          cr::ScopeIdentity::parse(scope).value(), crtest::power_key(), view, value,
          cr::EvidenceStatus::Accepted, stamp);
      if (item.ok()) {
        items.push_back(std::move(item.value()));
      }
    }
  }
  return items;
}

bool is_present(const std::vector<cr::EvidenceItem>& items, const cr::EvidenceId& id) {
  for (const cr::EvidenceItem& item : items) {
    if (item.id() == id) {
      return true;
    }
  }
  return false;
}

}  // namespace

CR_TEST(property, run_digest_ignores_evidence_insertion_order) {
  Rng rng(0x5EED0001ull);
  for (int trial = 0; trial < 12; ++trial) {
    auto engine = crtest::make_engine();
    CR_REQUIRE_OK(engine);
    std::vector<cr::EvidenceItem> items;
    for (int scope = 1; scope <= 4; ++scope) {
      auto generated = random_evidence("site-a/hall-1/row-" + std::to_string(scope), 0, &rng,
                                       engine.value().incarnation(), true);
      items.insert(items.end(), generated.begin(), generated.end());
    }
    // Insertion order is defined to be the order the caller happened to supply.
    CR_REQUIRE_OK(engine.value().append_evidence(items, engine.value().generation(),
                                                 engine.value().incarnation()));

    cr::RunRequest request;
    for (int scope = 1; scope <= 4; ++scope) {
      request.scopes.push_back(
          {cr::ScopeIdentity::parse("site-a/hall-1/row-" + std::to_string(scope)).value(),
           cr::ScopeSelectionMode::Exact});
    }
    request.generation = engine.value().generation();
    request.evaluation_instant = cr::Tick{50};
    request.clock_domain = crtest::test_domain();

    auto first = engine.value().reconcile(request);
    CR_REQUIRE_OK(first);

    // A second engine fed the same items in a shuffled order must produce the
    // same digest, cell for cell.
    std::shuffle(items.begin(), items.end(), std::mt19937_64(static_cast<std::uint64_t>(trial)));
    auto engine2 = crtest::make_engine();
    CR_REQUIRE_OK(engine2);
    CR_REQUIRE_OK(engine2.value().append_evidence(items, engine2.value().generation(),
                                                  engine2.value().incarnation()));
    auto second = engine2.value().reconcile(request);
    CR_REQUIRE_OK(second);
    CR_CHECK(first.value().run_digest() == second.value().run_digest());
    CR_CHECK_EQ(first.value().cells().size(), second.value().cells().size());
  }
}

CR_TEST(property, every_cell_satisfies_the_residual_identities) {
  Rng rng(0x5EED0002ull);
  for (int trial = 0; trial < 24; ++trial) {
    auto engine = crtest::make_engine();
    CR_REQUIRE_OK(engine);
    auto items = random_evidence("site-a/hall-1", 0, &rng, engine.value().incarnation(), true);
    CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                                 engine.value().incarnation()));
    auto run = engine.value().reconcile(
        crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 50));
    CR_REQUIRE_OK(run);
    for (const cr::ReconciliationCell& cell : run.value().cells()) {
      // raw = attributed + unexplained, exactly, whenever both are known.
      for (const cr::ResidualExplanation& explanation : cell.explanation.residuals()) {
        if (explanation.raw.is_known() && explanation.attributed.is_known() &&
            explanation.unexplained.is_known()) {
          CR_CHECK_EQ(explanation.raw.value(),
                      explanation.attributed.value() + explanation.unexplained.value());
        }
      }
      // derived_allocatable = usable - reserved.
      const cr::Quantity& usable = cell.view(cr::CapacityView::Usable);
      const cr::Quantity& reserved = cell.view(cr::CapacityView::Reserved);
      if (usable.is_known() && reserved.is_known() && cell.derived_allocatable.is_known()) {
        CR_CHECK_EQ(cell.derived_allocatable.value(), usable.value() - reserved.value());
      }
      // allocatable_skew = allocatable - derived.
      const cr::Quantity& allocatable = cell.view(cr::CapacityView::Allocatable);
      if (allocatable.is_known() && cell.derived_allocatable.is_known() &&
          cell.allocatable_skew.is_known()) {
        CR_CHECK_EQ(cell.allocatable_skew.value(),
                    allocatable.value() - cell.derived_allocatable.value());
      }
      // A residual involving an unknown view is itself unknown.
      if (cell.view(cr::CapacityView::Installed).is_unknown() ||
          cell.view(cr::CapacityView::Observed).is_unknown()) {
        CR_CHECK(cell.observed_gap.is_unknown());
      }
    }
  }
}

CR_TEST(property, unknown_never_becomes_zero) {
  Rng rng(0x5EED0003ull);
  for (int trial = 0; trial < 20; ++trial) {
    auto engine = crtest::make_engine();
    CR_REQUIRE_OK(engine);
    cr::EvidenceStamp stamp;
    stamp.generation = cr::Generation{0};
    stamp.revision = cr::Revision{1};
    stamp.incarnation = engine.value().incarnation();
    stamp.observed_at = cr::Tick{1};
    stamp.valid_until = cr::Tick{1000};
    stamp.clock_domain = crtest::test_domain();

    // Installed is known; observed is unknown for a randomly chosen reason.
    const auto reasons = {cr::UnknownReason::NotReported, cr::UnknownReason::SourceUnavailable,
                          cr::UnknownReason::Unsupported, cr::UnknownReason::Withheld,
                          cr::UnknownReason::Conflicted, cr::UnknownReason::PartialRollup};
    const auto reason = *std::next(reasons.begin(), rng.range(0, 5));

    std::vector<cr::EvidenceItem> items;
    auto installed = cr::EvidenceItem::create(
        cr::EvidenceId::generate(), cr::EvidenceSource::RackCapacity, "r",
        cr::ScopeIdentity::parse("site-a/hall-1").value(), crtest::power_key(),
        cr::CapacityView::Installed, cr::Quantity::known(500), cr::EvidenceStatus::Accepted,
        stamp);
    CR_REQUIRE_OK(installed);
    items.push_back(std::move(installed.value()));
    auto observed = cr::EvidenceItem::create(
        cr::EvidenceId::generate(), cr::EvidenceSource::ObservationStream, "m",
        cr::ScopeIdentity::parse("site-a/hall-1").value(), crtest::power_key(),
        cr::CapacityView::Observed, cr::Quantity::unknown(reason),
        cr::EvidenceStatus::Accepted, stamp);
    CR_REQUIRE_OK(observed);
    items.push_back(std::move(observed.value()));

    CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                                 engine.value().incarnation()));
    auto run = engine.value().reconcile(
        crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 50));
    CR_REQUIRE_OK(run);
    const cr::ReconciliationCell& cell = run.value().cells().front();
    // The residual must be unknown, and must carry the reason the value was
    // unknown. A zero here would be a silent overcommit.
    CR_CHECK(cell.observed_gap.is_unknown());
    CR_CHECK_EQ(cell.observed_gap.reason(), reason);
  }
}

CR_TEST(property, encode_decode_encode_is_a_fixed_point) {
  Rng rng(0x5EED0004ull);
  for (int trial = 0; trial < 16; ++trial) {
    auto engine = crtest::make_engine();
    CR_REQUIRE_OK(engine);
    std::vector<cr::EvidenceItem> items;
    for (int scope = 1; scope <= 3; ++scope) {
      auto generated = random_evidence("site-a/hall-1/row-" + std::to_string(scope), 0, &rng,
                                       engine.value().incarnation(), true);
      items.insert(items.end(), generated.begin(), generated.end());
    }
    CR_REQUIRE_OK(engine.value().append_evidence(items, engine.value().generation(),
                                                 engine.value().incarnation()));
    cr::RunRequest request;
    for (int scope = 1; scope <= 3; ++scope) {
      request.scopes.push_back(
          {cr::ScopeIdentity::parse("site-a/hall-1/row-" + std::to_string(scope)).value(),
           cr::ScopeSelectionMode::Exact});
    }
    request.generation = engine.value().generation();
    request.evaluation_instant = cr::Tick{50};
    request.clock_domain = crtest::test_domain();
    auto run = engine.value().reconcile(request);
    CR_REQUIRE_OK(run);

    const std::string first = run.value().canonical_bytes();
    auto decoded = cr::ReconciliationRun::decode_content(first, cr::default_limits());
    CR_REQUIRE_OK(decoded);
    const std::string second = decoded.value().canonical_bytes();
    CR_CHECK_EQ(first, second);
    CR_CHECK(decoded.value().run_digest() == run.value().run_digest());

    auto decoded_again = cr::ReconciliationRun::decode_content(second, cr::default_limits());
    CR_REQUIRE_OK(decoded_again);
    CR_CHECK_EQ(decoded_again.value().canonical_bytes(), first);
  }
}

CR_TEST(property, classification_is_the_first_exceeded_finding_in_precedence_order) {
  Rng rng(0x5EED0005ull);
  for (int trial = 0; trial < 40; ++trial) {
    auto engine = crtest::make_engine();
    CR_REQUIRE_OK(engine);
    auto items = random_evidence("site-a/hall-1", 0, &rng, engine.value().incarnation(), true);
    CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                                 engine.value().incarnation()));
    auto run = engine.value().reconcile(
        crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 50));
    CR_REQUIRE_OK(run);
    for (const cr::ReconciliationCell& cell : run.value().cells()) {
      CR_REQUIRE(!cell.findings.empty());
      // Findings are sorted by (class ordinal, target, magnitude, detail).
      for (std::size_t i = 1; i < cell.findings.size(); ++i) {
        CR_CHECK(!cr::finding_less(cell.findings[i], cell.findings[i - 1]));
      }
      // The headline is the first qualifying finding, or Agrees.
      bool found_qualifying = false;
      for (const cr::Finding& finding : cell.findings) {
        if (finding.exceeds_tolerance || !finding.magnitude.has_value()) {
          CR_CHECK_EQ(cell.primary_class(), finding.classification);
          found_qualifying = true;
          break;
        }
      }
      if (!found_qualifying) {
        CR_CHECK_EQ(cell.primary_class(), cr::DiscrepancyClass::Agrees);
      }
    }
  }
}

CR_TEST(property, a_run_never_claims_a_view_it_did_not_receive) {
  Rng rng(0x5EED0006ull);
  for (int trial = 0; trial < 30; ++trial) {
    auto engine = crtest::make_engine();
    CR_REQUIRE_OK(engine);
    const std::vector<cr::CapacityView> all = {
        cr::CapacityView::Planned,  cr::CapacityView::Reserved, cr::CapacityView::Installed,
        cr::CapacityView::Observed, cr::CapacityView::Usable,   cr::CapacityView::Allocatable};
    // Supply a random subset of views.
    std::vector<cr::EvidenceItem> items;
    std::vector<cr::CapacityView> supplied;
    cr::EvidenceStamp stamp;
    stamp.generation = cr::Generation{0};
    stamp.revision = cr::Revision{1};
    stamp.incarnation = engine.value().incarnation();
    stamp.observed_at = cr::Tick{1};
    stamp.valid_until = cr::Tick{1000};
    stamp.clock_domain = crtest::test_domain();
    for (cr::CapacityView view : all) {
      if (!rng.coin()) {
        continue;
      }
      supplied.push_back(view);
      auto item = cr::EvidenceItem::create(
          cr::EvidenceId::generate(), cr::EvidenceSource::RackCapacity, "r",
          cr::ScopeIdentity::parse("site-a/hall-1").value(), crtest::power_key(), view,
          cr::Quantity::known(rng.range(0, 100)), cr::EvidenceStatus::Accepted, stamp);
      CR_REQUIRE_OK(item);
      items.push_back(std::move(item.value()));
    }
    if (items.empty()) {
      continue;
    }
    CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                                 engine.value().incarnation()));
    auto run = engine.value().reconcile(
        crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 50));
    CR_REQUIRE_OK(run);
    const cr::ReconciliationCell& cell = run.value().cells().front();
    for (cr::CapacityView view : all) {
      const bool was_supplied =
          std::find(supplied.begin(), supplied.end(), view) != supplied.end();
      if (was_supplied) {
        CR_CHECK(cell.view(view).is_known());
        CR_CHECK(cell.views.selected[cr::view_index(view)].has_value());
      } else {
        CR_CHECK(cell.view(view).is_unknown());
        CR_CHECK(!cell.views.selected[cr::view_index(view)].has_value());
      }
    }
  }
}

CR_TEST(property, appending_evidence_never_changes_an_existing_item) {
  Rng rng(0x5EED0007ull);
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  std::vector<cr::EvidenceItem> accumulated;
  for (int batch = 0; batch < 8; ++batch) {
    auto items = random_evidence("site-a/hall-1/row-" + std::to_string(batch), 0, &rng,
                                 engine.value().incarnation(), true);
    for (const cr::EvidenceItem& item : items) {
      accumulated.push_back(item);
    }
    CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                                 engine.value().incarnation()));
    auto snapshot = engine.value().evidence_snapshot();
    CR_CHECK_EQ(snapshot->size(), accumulated.size());
    // Every item that was there before is still there, byte for byte.
    for (const cr::EvidenceItem& original : accumulated) {
      CR_CHECK(is_present(snapshot->items(), original.id()));
    }
  }
}

CR_TEST(property, json_rendering_is_well_formed_and_deterministic) {
  Rng rng(0x5EED0008ull);
  for (int trial = 0; trial < 8; ++trial) {
    auto engine = crtest::make_engine();
    CR_REQUIRE_OK(engine);
    auto items = random_evidence("site-a/hall-1", 0, &rng, engine.value().incarnation(), true);
    CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                                 engine.value().incarnation()));
    auto run = engine.value().reconcile(
        crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 50));
    CR_REQUIRE_OK(run);
    const std::string first = cr::render_run_json(run.value(), true);
    const std::string second = cr::render_run_json(run.value(), true);
    CR_CHECK_EQ(first, second);
    CR_CHECK(crtest::json_is_well_formed(first));
    for (const cr::ReconciliationCell& cell : run.value().cells()) {
      CR_CHECK(crtest::json_is_well_formed(cr::render_cell_json(cell)));
    }
  }
}

CR_TEST(property, a_second_identical_reconciliation_is_byte_identical) {
  Rng rng(0x5EED0009ull);
  for (int trial = 0; trial < 12; ++trial) {
    auto engine = crtest::make_engine();
    CR_REQUIRE_OK(engine);
    auto items = random_evidence("site-a/hall-1", 0, &rng, engine.value().incarnation(), true);
    CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                                 engine.value().incarnation()));
    const auto request =
        crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 50);
    auto first = engine.value().reconcile(request);
    auto second = engine.value().reconcile(request);
    CR_REQUIRE_OK(first);
    CR_REQUIRE_OK(second);
    CR_CHECK_EQ(first.value().canonical_bytes(), second.value().canonical_bytes());
  }
}
