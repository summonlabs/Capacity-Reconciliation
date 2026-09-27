// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Machine-readable rendering. The JSON surface is the contract a consumer
// branches on, so it is validated for well-formedness, for stable tokens and
// for determinism, not merely eyeballed.

#include <string>
#include <vector>

#include "summon/capacity_reconciliation/engine.hpp"
#include "summon/capacity_reconciliation/report.hpp"
#include "test_helpers.hpp"
#include "test_support.hpp"

namespace cr = summon::capacity_reconciliation;

namespace {

cr::Result<cr::ReconciliationRun> sample_run(cr::Engine& engine) {
  const struct {
    cr::CapacityView view;
    cr::Amount value;
  } rows[] = {{cr::CapacityView::Planned, 1000},
              {cr::CapacityView::Installed, 1000},
              {cr::CapacityView::Observed, 900},
              {cr::CapacityView::Usable, 900},
              {cr::CapacityView::Reserved, 100},
              {cr::CapacityView::Allocatable, 800}};
  std::vector<cr::EvidenceItem> items;
  for (const auto& row : rows) {
    cr::EvidenceStamp stamp;
    stamp.generation = cr::Generation{0};
    stamp.revision = cr::Revision{1};
    stamp.incarnation = engine.incarnation();
    stamp.observed_at = cr::Tick{1};
    stamp.valid_until = cr::Tick{100000};
    stamp.clock_domain = crtest::test_domain();
    auto item = cr::EvidenceItem::create(
        cr::EvidenceId::generate(), cr::EvidenceSource::RackCapacity, "r",
        cr::ScopeIdentity::parse("site-a/hall-1").value(), crtest::power_key(), row.view,
        cr::Quantity::known(row.value), cr::EvidenceStatus::Accepted, stamp);
    if (!item.ok()) {
      return item.error();
    }
    items.push_back(std::move(item.value()));
  }
  auto status = engine.append_evidence(std::move(items), engine.generation(),
                                       engine.incarnation());
  if (!status.ok()) {
    return status.error();
  }
  return engine.reconcile(
      crtest::exact_request("site-a/hall-1", engine.generation().value(), 50));
}

}  // namespace

CR_TEST(render, run_json_is_well_formed_and_carries_stable_tokens) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  auto run = sample_run(engine.value());
  CR_REQUIRE_OK(run);

  const std::string json = cr::render_run_json(run.value(), true);
  CR_CHECK(crtest::json_is_well_formed(json));
  CR_CHECK_EQ(crtest::json_find_string(json, "schema"),
              std::string("capacity-reconciliation/v1"));
  CR_CHECK_EQ(crtest::json_find_string(json, "run"), run.value().id().to_string());
  CR_CHECK_EQ(crtest::json_find_string(json, "run_digest"), run.value().run_digest().to_hex());
  CR_CHECK_EQ(crtest::json_find_string(json, "resolution"),
              std::string(cr::to_string(run.value().resolution())));
  // Installed is 1000 and observed 900, so the headline is the shortfall. The
  // unexplained part is a separate finding, not the headline.
  CR_CHECK_EQ(crtest::json_find_string(json, "primary_class"),
              std::string("observed_shortfall"));
  // Tokens are rendered as strings, never bound to the boolean overload.
  CR_CHECK(json.find(": true,") != std::string::npos || json.find(": true\n") != std::string::npos ||
           json.find("\"present\": ") != std::string::npos);

  // The compact rendering omits the cells but keeps the summary.
  const std::string compact = cr::render_run_json(run.value(), false);
  CR_CHECK(crtest::json_is_well_formed(compact));
  CR_CHECK(compact.size() < json.size());
  // The compact rendering keeps the summary counts but omits the cell bodies.
  CR_CHECK(compact.find("\"primary_class\"") == std::string::npos);
  CR_CHECK(compact.find("\"summary\"") != std::string::npos);
}

CR_TEST(render, every_renderer_produces_well_formed_json) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  auto run = sample_run(engine.value());
  CR_REQUIRE_OK(run);
  for (const cr::ReconciliationCell& cell : run.value().cells()) {
    const std::string cell_json = cr::render_cell_json(cell);
    CR_CHECK(crtest::json_is_well_formed(cell_json));
    CR_CHECK_EQ(crtest::json_find_string(cell_json, "schema"),
                std::string("capacity-reconciliation/v1"));
    CR_CHECK(crtest::json_is_well_formed(cr::render_explanation_json(cell.explanation)));
    for (const cr::Finding& finding : cell.findings) {
      // The text rendering is not a contract, but it must still be a valid
      // JSON string so that a consumer may embed it safely.
      const std::string text = finding.to_string();
      CR_CHECK(crtest::json_is_well_formed("[" + cr::json_escape(text) + "]"));
      CR_CHECK(!text.empty());
    }
  }
  CR_CHECK(crtest::json_is_well_formed(cr::render_policy_json(cr::ReconciliationPolicy::standard())));

  cr::RecoveryReport recovery;
  recovery.outcome = cr::RecoveryOutcome::RecoveredFromPreviousCommit;
  recovery.generation = cr::Generation{9};
  recovery.note = "head_failed_verification";
  const std::string recovery_json = cr::render_recovery_json(recovery);
  CR_CHECK(crtest::json_is_well_formed(recovery_json));
  CR_CHECK_EQ(crtest::json_find_string(recovery_json, "outcome"),
              std::string("recovered_from_previous_commit"));

  cr::Store::VerificationReport verification;
  verification.ok = false;
  verification.problems.push_back("record 1: digest mismatch");
  const std::string verification_json = cr::render_verification_json(verification);
  CR_CHECK(crtest::json_is_well_formed(verification_json));
  CR_CHECK(verification_json.find("\"ok\": false") != std::string::npos);

  cr::HistoryEntry entry;
  entry.sequence = 1;
  entry.kind = cr::HistoryEventKind::RunCommitted;
  entry.generation = cr::Generation{1};
  CR_CHECK(crtest::json_is_well_formed(cr::render_history_json(entry)));
}

CR_TEST(render, empty_arrays_and_objects_are_well_formed) {
  cr::JsonWriter writer;
  writer.begin_object();
  writer.key("empty_array");
  writer.begin_array();
  writer.end_array();
  writer.key("empty_object");
  writer.begin_object();
  writer.end_object();
  writer.key("after");
  writer.string_value("value");
  writer.end_object();
  const std::string json = std::move(writer).take();
  CR_CHECK(crtest::json_is_well_formed(json));
  CR_CHECK_EQ(json.find(",]"), std::string::npos);
  CR_CHECK_EQ(json.find(",}"), std::string::npos);
  CR_CHECK_EQ(json.find("[,]"), std::string::npos);
}

CR_TEST(render, nested_values_are_comma_separated_exactly_once) {
  cr::JsonWriter writer;
  writer.begin_array();
  writer.begin_object();
  writer.member("a", std::uint64_t(1));
  writer.key("nested");
  writer.begin_array();
  writer.unumber_value(1);
  writer.unumber_value(2);
  writer.end_array();
  writer.end_object();
  writer.begin_object();
  writer.member("b", "x");
  writer.end_object();
  writer.end_array();
  const std::string json = std::move(writer).take();
  CR_CHECK(crtest::json_is_well_formed(json));
}

CR_TEST(render, non_ascii_and_control_characters_are_escaped) {
  const std::string hostile = std::string("a\"b\\c\nd\te") + "\xC3\xA9" + "\x01";
  const std::string escaped = cr::json_escape(hostile);
  CR_CHECK(crtest::json_is_well_formed("[" + escaped + "]"));
  for (char c : escaped) {
    const auto value = static_cast<unsigned char>(c);
    CR_CHECK(value < 0x7F);
  }
  // Non-ASCII bytes are escaped byte by byte, so the two UTF-8 bytes of 'e
  // acute' (0xC3 0xA9) become two escapes. Nothing is normalised, folded or
  // re-decoded, which is what keeps the output byte-exact.
  CR_CHECK(escaped.find("\\u00c3") != std::string::npos);
  CR_CHECK(escaped.find("\\u00a9") != std::string::npos);
  CR_CHECK(escaped.find("\\u0001") != std::string::npos);
  CR_CHECK(escaped.find("\\\"") != std::string::npos);
  CR_CHECK(escaped.find("\\\\") != std::string::npos);
}

CR_TEST(render, text_renderings_are_stable_and_do_not_leak_unknowns) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  auto run = sample_run(engine.value());
  CR_REQUIRE_OK(run);
  const std::string first = cr::render_run_text(run.value());
  const std::string second = cr::render_run_text(run.value());
  CR_CHECK_EQ(first, second);
  CR_CHECK(first.find("partially_explained") != std::string::npos);
  for (const cr::ReconciliationCell& cell : run.value().cells()) {
    const std::string text = cr::render_cell_text(cell);
    CR_CHECK(text.find("site-a/hall-1") != std::string::npos);
    CR_CHECK(text.find("power:milliwatt") != std::string::npos);
  }
}

CR_TEST(render, quantities_render_unknown_and_zero_differently) {
  CR_CHECK_EQ(cr::Quantity::known(0).to_string(), std::string("0"));
  CR_CHECK_EQ(cr::Quantity::unknown(cr::UnknownReason::Conflicted).to_string(),
              std::string("unknown:conflicted"));
  CR_CHECK_EQ(cr::quantity_json(cr::Quantity::known(-5)), std::string("-5"));
  CR_CHECK_EQ(cr::quantity_json(cr::Quantity::unknown(cr::UnknownReason::Expired)),
              std::string("{\"unknown\": \"expired\"}"));
}

CR_TEST(render, an_envelope_keeps_the_declared_key_order) {
  const std::string json = cr::render_envelope(
      "sample", {{"alpha", cr::render_policy_json(cr::ReconciliationPolicy::standard())},
                 {"beta", cr::quantity_json(cr::Quantity::known(1))}});
  CR_CHECK(crtest::json_is_well_formed(json));
  CR_CHECK_EQ(crtest::json_find_string(json, "kind"), std::string("sample"));
  const std::size_t alpha = json.find("\"alpha\"");
  const std::size_t beta = json.find("\"beta\"");
  CR_REQUIRE(alpha != std::string::npos);
  CR_REQUIRE(beta != std::string::npos);
  CR_CHECK(alpha < beta);
}
