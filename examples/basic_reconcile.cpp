// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Example 1: reconcile one cell in memory and read the explanation.
//
// Scenario (SYNTHETIC): a row of a facility hall is planned for 400 kW, has
// 400 kW installed, delivers 360 kW, and a chiller outage has been declared
// against 30 kW of that shortfall. The remaining 10 kW is unexplained, and the
// point of the example is that it stays unexplained.

#include <cstdio>
#include <string>
#include <vector>

#include "summon/capacity_reconciliation/engine.hpp"
#include "summon/capacity_reconciliation/report.hpp"

namespace cr = summon::capacity_reconciliation;

namespace {

cr::Result<cr::EvidenceItem> make_evidence(const cr::ScopeIdentity& scope,
                                           const cr::DimensionKey& dimension,
                                           cr::CapacityView view, cr::Amount value,
                                           cr::EvidenceSource source,
                                           const std::string& instance, cr::Generation generation,
                                           const cr::ClockDomain& domain) {
  cr::EvidenceStamp stamp;
  stamp.generation = generation;
  stamp.revision = cr::Revision{1};
  stamp.incarnation = cr::IncarnationId::generate();
  stamp.observed_at = cr::Tick{1000};
  stamp.valid_until = cr::Tick{2000};
  stamp.clock_domain = domain;
  return cr::EvidenceItem::create(cr::EvidenceId::generate(), source, instance, scope, dimension,
                                  view, cr::Quantity::known(value), cr::EvidenceStatus::Accepted,
                                  stamp);
}

}  // namespace

int main() {
  const auto domain = cr::ClockDomain::create("facility-monotonic").value();

  cr::EngineOptions options;
  options.clock_domain = domain;
  auto engine = cr::Engine::create(options);
  if (!engine.ok()) {
    std::fprintf(stderr, "%s\n", engine.error().to_string().c_str());
    return 1;
  }

  const auto scope = cr::ScopeIdentity::parse("site-a/hall-1/row-3").value();
  const auto power = cr::DimensionKey::create(cr::CapacityDimension::Power, cr::Unit::MilliWatt).value();

  // Generation 0 is the generation the evidence is produced under; the engine
  // advances to generation 1 once it has been ingested.
  const cr::Generation generation = engine.value().generation();
  std::vector<cr::EvidenceItem> items;
  items.push_back(make_evidence(scope, power, cr::CapacityView::Planned, 400000, 
                                cr::EvidenceSource::FacilityCapacity, "facility-1", generation,
                                domain)
                      .value());
  items.push_back(make_evidence(scope, power, cr::CapacityView::Installed, 400000,
                                cr::EvidenceSource::RackCapacity, "rack-capacity-1", generation,
                                domain)
                      .value());
  items.push_back(make_evidence(scope, power, cr::CapacityView::Observed, 360000,
                                cr::EvidenceSource::ObservationStream, "facility-meters",
                                generation, domain)
                      .value());
  items.push_back(make_evidence(scope, power, cr::CapacityView::Usable, 360000,
                                cr::EvidenceSource::CoolingCapacity, "cooling-1", generation,
                                domain)
                      .value());
  items.push_back(make_evidence(scope, power, cr::CapacityView::Reserved, 100000,
                                cr::EvidenceSource::ReservationRegister, "reservations",
                                generation, domain)
                      .value());
  items.push_back(make_evidence(scope, power, cr::CapacityView::Allocatable, 260000,
                                cr::EvidenceSource::FacilityCapacity, "facility-1", generation,
                                domain)
                      .value());

  auto appended = engine.value().append_evidence(std::move(items), generation,
                                                engine.value().incarnation());
  if (!appended.ok()) {
    std::fprintf(stderr, "%s\n", appended.error().to_string().c_str());
    return 1;
  }

  // The chiller outage explains 30 kW of the 40 kW that installed capacity is
  // not delivering. Nothing explains the other 10 kW, and the runtime will say
  // so rather than balancing the books.
  cr::EvidenceStamp stamp;
  stamp.generation = generation;
  stamp.revision = cr::Revision{1};
  stamp.incarnation = engine.value().incarnation();
  stamp.observed_at = cr::Tick{1000};
  stamp.valid_until = cr::Tick{2000};
  stamp.clock_domain = domain;
  auto attribution = cr::AttributionItem::create(
      cr::AttributionId::generate(), cr::EvidenceSource::CoolingCapacity, "cooling-1", scope,
      power, cr::ResidualKind::ObservedGap, cr::AttributionReason::EquipmentFailure,
      "chiller_2_outage", 30000, stamp);
  if (!attribution.ok()) {
    std::fprintf(stderr, "%s\n", attribution.error().to_string().c_str());
    return 1;
  }
  std::vector<cr::AttributionItem> attributions;
  attributions.push_back(std::move(attribution.value()));
  auto appended_attribution =
      engine.value().append_attributions(std::move(attributions), engine.value().generation(),
                                         engine.value().incarnation());
  if (!appended_attribution.ok()) {
    std::fprintf(stderr, "%s\n", appended_attribution.error().to_string().c_str());
    return 1;
  }

  cr::RunRequest request;
  cr::ScopeSelection selection;
  selection.scope = scope;
  selection.mode = cr::ScopeSelectionMode::Exact;
  request.scopes.push_back(selection);
  request.generation = engine.value().generation();
  request.evaluation_instant = cr::Tick{1500};
  request.clock_domain = domain;

  auto run = engine.value().reconcile(request);
  if (!run.ok()) {
    std::fprintf(stderr, "%s\n", run.error().to_string().c_str());
    return 1;
  }

  std::printf("%s\n\n", cr::render_run_text(run.value()).c_str());
  for (const cr::ReconciliationCell& cell : run.value().cells()) {
    std::printf("%s\n", cr::render_cell_text(cell).c_str());
    for (const cr::ResidualExplanation& explanation : cell.explanation.residuals()) {
      std::printf("  residual %-22s raw=%s attributed=%s unexplained=%s\n",
                  cr::to_string(explanation.target), explanation.raw.to_string().c_str(),
                  explanation.attributed.to_string().c_str(),
                  explanation.unexplained.to_string().c_str());
    }
  }
  return 0;
}
