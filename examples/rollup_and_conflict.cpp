// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Example 3: rollup and conflict, the two places where a capacity total is most
// easily faked.
//
//   * A subtree rollup sums the descendant scopes that reported. If one
//     descendant did not report a view, the rollup is *unknown*, not a smaller
//     number. The partial sum survives in the explanation graph.
//
//   * Two observation sources report different installed values for the same
//     scope at the same precedence. The policy cannot order them, so the cell
//     is conflicted and both values are retained in the conflict set. Nothing
//     picks a winner.

#include <cstdio>
#include <string>
#include <vector>

#include "summon/capacity_reconciliation/engine.hpp"
#include "summon/capacity_reconciliation/report.hpp"

namespace cr = summon::capacity_reconciliation;

namespace {

cr::Result<cr::EvidenceItem> make(const std::string& scope_text, cr::CapacityView view,
                                  cr::Quantity value, cr::EvidenceSource source,
                                  const std::string& instance, cr::Generation generation,
                                  const cr::ClockDomain& domain) {
  auto scope = cr::ScopeIdentity::parse(scope_text);
  if (!scope.ok()) {
    return scope.error();
  }
  auto dimension =
      cr::DimensionKey::create(cr::CapacityDimension::Power, cr::Unit::MilliWatt);
  if (!dimension.ok()) {
    return dimension.error();
  }
  cr::EvidenceStamp stamp;
  stamp.generation = generation;
  stamp.revision = cr::Revision{1};
  stamp.incarnation = cr::IncarnationId::generate();
  stamp.observed_at = cr::Tick{100};
  stamp.valid_until = cr::Tick{1000};
  stamp.clock_domain = domain;
  return cr::EvidenceItem::create(cr::EvidenceId::generate(), source, instance, scope.value(),
                                  dimension.value(), view, value, cr::EvidenceStatus::Accepted,
                                  stamp);
}

void dump(const cr::ReconciliationRun& run) {
  std::printf("%s\n", cr::render_run_text(run).c_str());
  for (const cr::ReconciliationCell& cell : run.cells()) {
    std::printf("  %s\n", cr::render_cell_text(cell).c_str());
    std::printf("    installed=%s observed=%s (rolled_up=%s contributors=%u missing=%u)\n",
                cell.view(cr::CapacityView::Installed).to_string().c_str(),
                cell.view(cr::CapacityView::Observed).to_string().c_str(),
                cell.rolled_up ? "true" : "false", cell.rollup_contributors, cell.rollup_missing);
    for (const cr::EvidenceConflict& conflict : cell.conflicts) {
      std::printf("    conflict [%s] view=%s resolved=%s values={", conflict.unresolvable_reason.c_str(),
                  cr::to_string(conflict.view), conflict.selected_value.has_value() ? "yes" : "no");
      for (std::size_t i = 0; i < conflict.values.size(); ++i) {
        std::printf("%s%lld", i == 0 ? "" : ",", static_cast<long long>(conflict.values[i]));
      }
      std::printf("} participants=%zu\n", conflict.participants.size());
    }
    for (const cr::ResidualExplanation& explanation : cell.explanation.residuals()) {
      std::printf("    residual %-20s raw=%s attributed=%s unexplained=%s\n",
                  cr::to_string(explanation.target), explanation.raw.to_string().c_str(),
                  explanation.attributed.to_string().c_str(),
                  explanation.unexplained.to_string().c_str());
    }
  }
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
  const cr::Generation generation = engine.value().generation();

  std::vector<cr::EvidenceItem> items;
  const auto push = [&](cr::Result<cr::EvidenceItem> item) -> bool {
    if (!item.ok()) {
      std::fprintf(stderr, "%s\n", item.error().to_string().c_str());
      return false;
    }
    items.push_back(std::move(item.value()));
    return true;
  };

  // Row 1 and row 2 report; row 3 reports installed but not observed. The
  // rollup over the hall must therefore be unknown for the observed view.
  if (!push(make("site-a/hall-1/row-1", cr::CapacityView::Installed, cr::Quantity::known(200000),
                 cr::EvidenceSource::RackCapacity, "rack-1", generation, domain)) ||
      !push(make("site-a/hall-1/row-1", cr::CapacityView::Observed, cr::Quantity::known(200000),
                 cr::EvidenceSource::ObservationStream, "meter-1", generation, domain)) ||
      !push(make("site-a/hall-1/row-2", cr::CapacityView::Installed, cr::Quantity::known(150000),
                 cr::EvidenceSource::RackCapacity, "rack-2", generation, domain)) ||
      !push(make("site-a/hall-1/row-2", cr::CapacityView::Observed, cr::Quantity::known(140000),
                 cr::EvidenceSource::ObservationStream, "meter-2", generation, domain)) ||
      !push(make("site-a/hall-1/row-3", cr::CapacityView::Installed, cr::Quantity::known(100000),
                 cr::EvidenceSource::RackCapacity, "rack-3", generation, domain))) {
    return 1;
  }

  // Two observation streams disagree about row 1 installed capacity at equal
  // policy precedence. Neither wins.
  if (!push(make("site-a/hall-1/row-2", cr::CapacityView::Installed, cr::Quantity::known(160000),
                 cr::EvidenceSource::AssetRegistry, "asset-registry-1", generation, domain))) {
    return 1;
  }

  auto status = engine.value().append_evidence(std::move(items), generation,
                                              engine.value().incarnation());
  if (!status.ok()) {
    std::fprintf(stderr, "%s\n", status.error().to_string().c_str());
    return 1;
  }

  cr::RunRequest request;
  request.scopes.push_back({cr::ScopeIdentity::parse("site-a/hall-1").value(),
                            cr::ScopeSelectionMode::SubtreeRollup});
  request.generation = engine.value().generation();
  request.evaluation_instant = cr::Tick{500};
  request.clock_domain = domain;

  auto run = engine.value().reconcile(request);
  if (!run.ok()) {
    std::fprintf(stderr, "%s\n", run.error().to_string().c_str());
    return 1;
  }
  std::printf("--- subtree rollup over site-a/hall-1 ---\n");
  dump(run.value());
  return 0;
}
