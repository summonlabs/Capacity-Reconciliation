// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// capacity_reconciliation_cli -- inspection and administration.
//
// Every command is a thin, strict wrapper over the library: it takes the same
// locks, applies the same bounds and reports the same typed outcomes. Nothing
// here can reach past a precondition that the library enforces, which is why
// the inspection surface is exactly as strict as the operational one.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "cli_support.hpp"
#include "summon/capacity_reconciliation/engine.hpp"
#include "summon/capacity_reconciliation/report.hpp"
#include "summon/capacity_reconciliation/store.hpp"
#include "summon/capacity_reconciliation/version.hpp"

namespace cr = summon::capacity_reconciliation;
namespace tools = summon::capacity_reconciliation::tools;

namespace {

constexpr int kExitOk = 0;
constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;

const char* kUsage =
    "capacity_reconciliation_cli <command> [options]\n"
    "\n"
    "  version\n"
    "  policy [--json]\n"
    "  apply       --store DIR --input FILE [--json]\n"
    "  reconcile   --store DIR --input FILE [--commit] [--json]\n"
    "  inspect     --store DIR [--json]\n"
    "  history     --store DIR [--json]\n"
    "  show        --store DIR --run ID [--cells] [--json]\n"
    "  diff        --store DIR --before ID --after ID [--json]\n"
    "  revalidate  --store DIR --run ID --instant N [--json]\n"
    "  reopen      --store DIR --run ID --instant N [--json]\n"
    "  close       --store DIR --run ID --instant N\n"
    "  verify      --store DIR [--json]\n";

int fail(const cr::Error& error) {
  tools::emit_error(error);
  return kExitFailure;
}

int fail(const std::string& message) {
  std::fputs(message.c_str(), stderr);
  std::fputc('\n', stderr);
  return kExitUsage;
}

cr::Result<cr::Store> open_store(const tools::Args& args, bool writer) {
  auto path = args.value("store");
  if (!path.has_value() || path->empty()) {
    return cr::make_error(cr::ErrorCode::InvalidArgument, "--store DIR is required", "argument");
  }
  cr::StoreOpenOptions options;
  options.create_if_missing = writer;
  options.writer = writer;
  return cr::Store::open(std::filesystem::path(*path), options);
}

std::string require_id(const tools::Args& args, const char* key) {
  return args.value_or(key, std::string());
}

cr::Result<cr::ReconciliationRunId> parse_run_id(const std::string& text) {
  auto id = cr::ReconciliationRunId::parse(text);
  if (!id.has_value()) {
    return cr::make_error(cr::ErrorCode::InvalidArgument,
                          "'" + text + "' is not a reconciliation run identity", "run_id");
  }
  return *id;
}

int command_version() {
  cr::JsonWriter writer;
  writer.begin_object();
  writer.member("runtime", "capacity-reconciliation");
  writer.member("version", cr::version_string());
  writer.member("store_format_version",
                static_cast<std::uint64_t>(cr::kStoreFormatVersion));
  writer.member("record_format_version",
                static_cast<std::uint64_t>(cr::kRecordFormatVersion));
  writer.member("canonical_schema_version",
                static_cast<std::uint64_t>(cr::kCanonicalSchemaVersion));
  writer.end_object();
  tools::emit(std::move(writer).take());
  return kExitOk;
}

int command_policy(const tools::Args& args) {
  const auto policy = cr::ReconciliationPolicy::standard();
  if (args.has("json")) {
    tools::emit(cr::render_policy_json(policy));
    return kExitOk;
  }
  std::printf("policy digest %s\n", policy.digest().to_hex().c_str());
  std::printf("source precedence (most authoritative first):\n");
  std::size_t rank = 0;
  for (cr::EvidenceSource source : policy.precedence()) {
    std::printf("  %2zu  %s\n", rank++, cr::to_string(source));
  }
  std::printf("max_generation_lag     %llu\n",
              static_cast<unsigned long long>(policy.max_generation_lag));
  std::printf("stale_evidence         %s\n", cr::to_string(policy.stale_evidence));
  std::printf("missing_view           %s\n", cr::to_string(policy.missing_view));
  std::printf("rollup                 %s\n", cr::to_string(policy.rollup));
  return kExitOk;
}

int command_apply(const tools::Args& args) {
  auto store = open_store(args, true);
  if (!store.ok()) {
    return fail(store.error());
  }
  auto input = args.value("input");
  if (!input.has_value() || input->empty()) {
    return fail(std::string("apply requires --input FILE"));
  }
  auto text = tools::read_text_file(*input, 16u * 1024u * 1024u);
  if (!text.ok()) {
    return fail(text.error());
  }
  auto scenario = tools::parse_scenario(text.value(), "facility-default");
  if (!scenario.ok()) {
    return fail(scenario.error());
  }

  cr::EngineOptions options;
  options.clock_domain = cr::ClockDomain{scenario.value().clock_domain};
  auto engine = cr::Engine::attach(std::move(store.value()), options);
  if (!engine.ok()) {
    return fail(engine.error());
  }
  const auto incarnation = engine.value().incarnation();
  const auto generation = engine.value().generation();

  if (!scenario.value().evidence.empty()) {
    auto status = engine.value().append_evidence(scenario.value().evidence, generation,
                                                incarnation);
    if (!status.ok()) {
      return fail(status.error());
    }
  }
  if (!scenario.value().attributions.empty()) {
    auto status = engine.value().append_attributions(scenario.value().attributions,
                                                     engine.value().generation(), incarnation);
    if (!status.ok()) {
      return fail(status.error());
    }
  }

  cr::JsonWriter writer;
  writer.begin_object();
  writer.member("kind", "applied");
  writer.member("generation", engine.value().generation().value());
  writer.member("evidence", static_cast<std::uint64_t>(engine.value().evidence_snapshot()->size()));
  writer.member("attributions",
                static_cast<std::uint64_t>(engine.value().attribution_snapshot()->size()));
  writer.end_object();
  tools::emit(std::move(writer).take());
  return kExitOk;
}

int command_reconcile(const tools::Args& args) {
  auto store = open_store(args, true);
  if (!store.ok()) {
    return fail(store.error());
  }
  auto input = args.value("input");
  if (!input.has_value() || input->empty()) {
    return fail(std::string("reconcile requires --input FILE"));
  }
  auto text = tools::read_text_file(*input, 16u * 1024u * 1024u);
  if (!text.ok()) {
    return fail(text.error());
  }
  auto scenario = tools::parse_scenario(text.value(), "facility-default");
  if (!scenario.ok()) {
    return fail(scenario.error());
  }
  if (!scenario.value().instant.has_value()) {
    return fail(std::string("reconcile requires an 'instant at=<ticks>' line"));
  }

  cr::EngineOptions options;
  options.clock_domain = cr::ClockDomain{scenario.value().clock_domain};
  options.policy = scenario.value().policy;
  auto engine = cr::Engine::attach(std::move(store.value()), options);
  if (!engine.ok()) {
    return fail(engine.error());
  }
  const auto incarnation = engine.value().incarnation();
  const auto generation = engine.value().generation();

  if (!scenario.value().evidence.empty()) {
    auto status =
        engine.value().append_evidence(scenario.value().evidence, generation, incarnation);
    if (!status.ok()) {
      return fail(status.error());
    }
  }
  if (!scenario.value().attributions.empty()) {
    auto status = engine.value().append_attributions(scenario.value().attributions,
                                                     engine.value().generation(), incarnation);
    if (!status.ok()) {
      return fail(status.error());
    }
  }

  cr::RunRequest request;
  request.scopes = scenario.value().scopes;
  request.generation = engine.value().generation();
  request.evaluation_instant = cr::Tick{*scenario.value().instant};
  request.clock_domain = cr::ClockDomain{scenario.value().clock_domain};
  request.policy = scenario.value().policy;

  const bool commit = args.has("commit");
  auto run = commit ? engine.value().reconcile_and_commit(request, request.generation,
                                                          engine.value().incarnation())
                    : engine.value().reconcile(request);
  if (!run.ok()) {
    return fail(run.error());
  }

  if (args.has("json")) {
    tools::emit(cr::render_run_json(run.value(), true));
  } else {
    std::printf("%s\n", cr::render_run_text(run.value()).c_str());
    for (const cr::ReconciliationCell& cell : run.value().cells()) {
      std::printf("  %s\n", cr::render_cell_text(cell).c_str());
    }
  }
  return kExitOk;
}

int command_inspect(const tools::Args& args) {
  auto store = open_store(args, false);
  if (!store.ok()) {
    return fail(store.error());
  }
  const auto& image = store.value().image();
  if (args.has("json")) {
    cr::JsonWriter writer;
    writer.begin_object();
    writer.member("schema", "capacity-reconciliation/v1");
    writer.member("kind", "store");
    writer.member("runtime_version", cr::version_string());
    writer.key("recovery");
    writer.raw_value(cr::render_recovery_json(store.value().recovery()));
    writer.member("store_identity", image.store_identity.to_hex());
    writer.member("generation", image.generation.value());
    writer.member("attempt", image.attempt.value());
    writer.member("incarnation", image.incarnation.to_string());
    writer.member("epoch", image.epoch.value());
    writer.member("clock_domain", image.clock_domain.name());
    writer.member("evidence", static_cast<std::uint64_t>(image.evidence.size()));
    writer.member("attributions", static_cast<std::uint64_t>(image.attributions.size()));
    writer.member("runs", static_cast<std::uint64_t>(image.runs.size()));
    writer.member("history", static_cast<std::uint64_t>(image.history.size()));
    writer.key("run_summaries");
    writer.begin_array();
    for (const cr::ReconciliationRun& run : image.runs) {
      writer.raw_value(cr::render_run_json(run, false));
    }
    writer.end_array();
    writer.end_object();
    tools::emit(std::move(writer).take());
    return kExitOk;
  }
  std::printf("store            %s\n", store.value().root().string().c_str());
  std::printf("identity         %s\n", image.store_identity.to_hex().c_str());
  std::printf("generation       %llu\n",
              static_cast<unsigned long long>(image.generation.value()));
  std::printf("attempt          %llu\n",
              static_cast<unsigned long long>(image.attempt.value()));
  std::printf("epoch            %llu\n",
              static_cast<unsigned long long>(image.epoch.value()));
  std::printf("clock domain     %s\n", image.clock_domain.name().c_str());
  std::printf("recovery         %s\n", cr::to_string(store.value().recovery().outcome));
  std::printf("evidence         %zu\n", image.evidence.size());
  std::printf("attributions     %zu\n", image.attributions.size());
  std::printf("runs             %zu\n", image.runs.size());
  std::printf("history          %zu\n", image.history.size());
  return kExitOk;
}

int command_history(const tools::Args& args) {
  auto store = open_store(args, false);
  if (!store.ok()) {
    return fail(store.error());
  }
  const auto& history = store.value().image().history;
  if (args.has("json")) {
    cr::JsonWriter writer;
    writer.begin_object();
    writer.member("schema", "capacity-reconciliation/v1");
    writer.member("kind", "history");
    writer.member("count", static_cast<std::uint64_t>(history.size()));
    writer.key("entries");
    writer.begin_array();
    for (const cr::HistoryEntry& entry : history) {
      writer.raw_value(cr::render_history_json(entry));
    }
    writer.end_array();
    writer.end_object();
    tools::emit(std::move(writer).take());
    return kExitOk;
  }
  for (const cr::HistoryEntry& entry : history) {
    std::printf("%llu  gen=%llu  %-22s %s %s\n",
                static_cast<unsigned long long>(entry.sequence),
                static_cast<unsigned long long>(entry.generation.value()),
                cr::to_string(entry.kind),
                entry.run.has_value() ? entry.run->to_string().c_str() : "-",
                entry.detail.c_str());
  }
  return kExitOk;
}

int command_show(const tools::Args& args) {
  auto store = open_store(args, false);
  if (!store.ok()) {
    return fail(store.error());
  }
  auto id = parse_run_id(require_id(args, "run"));
  if (!id.ok()) {
    return fail(id.error());
  }
  cr::EngineOptions options;
  options.clock_domain = store.value().image().clock_domain;
  auto engine = cr::Engine::attach(std::move(store.value()), options);
  if (!engine.ok()) {
    return fail(engine.error());
  }
  auto run = engine.value().find_run(id.value());
  if (!run.ok()) {
    return fail(run.error());
  }
  if (args.has("json")) {
    tools::emit(cr::render_run_json(run.value(), args.has("cells")));
    return kExitOk;
  }
  std::printf("%s\n", cr::render_run_text(run.value()).c_str());
  if (args.has("cells")) {
    for (const cr::ReconciliationCell& cell : run.value().cells()) {
      std::printf("  %s\n", cr::render_cell_text(cell).c_str());
    }
  }
  return kExitOk;
}

int command_diff(const tools::Args& args) {
  auto store = open_store(args, false);
  if (!store.ok()) {
    return fail(store.error());
  }
  auto before = parse_run_id(require_id(args, "before"));
  if (!before.ok()) {
    return fail(before.error());
  }
  auto after = parse_run_id(require_id(args, "after"));
  if (!after.ok()) {
    return fail(after.error());
  }
  cr::EngineOptions options;
  options.clock_domain = store.value().image().clock_domain;
  auto engine = cr::Engine::attach(std::move(store.value()), options);
  if (!engine.ok()) {
    return fail(engine.error());
  }
  auto diff = engine.value().diff(before.value(), after.value());
  if (!diff.ok()) {
    return fail(diff.error());
  }
  if (args.has("json")) {
    tools::emit(cr::render_diff_json(diff.value()));
    return kExitOk;
  }
  std::printf("before %s\n", diff.value().before().to_string().c_str());
  std::printf("after  %s\n", diff.value().after().to_string().c_str());
  std::printf("digest %s\n", diff.value().digest().to_hex().c_str());
  for (std::uint8_t i = 0; i <= 5; ++i) {
    const auto transition = static_cast<cr::CellTransition>(i);
    std::printf("  %-16s %zu\n", cr::to_string(transition), diff.value().count(transition));
  }
  for (const cr::DiffEntry& entry : diff.value().entries()) {
    if (entry.transition == cr::CellTransition::Unchanged) {
      continue;
    }
    std::printf("  %s %s -> %s\n", entry.scope.to_string().c_str(),
                entry.dimension.to_string().c_str(), cr::to_string(entry.transition));
  }
  return kExitOk;
}

int command_revalidate(const tools::Args& args) {
  auto store = open_store(args, true);
  if (!store.ok()) {
    return fail(store.error());
  }
  auto id = parse_run_id(require_id(args, "run"));
  if (!id.ok()) {
    return fail(id.error());
  }
  auto instant = args.value("instant");
  if (!instant.has_value()) {
    return fail(std::string("revalidate requires --instant N"));
  }
  cr::EngineOptions options;
  options.clock_domain = store.value().image().clock_domain;
  auto engine = cr::Engine::attach(std::move(store.value()), options);
  if (!engine.ok()) {
    return fail(engine.error());
  }
  auto run = engine.value().revalidate(id.value(), cr::Tick{std::strtoull(instant->c_str(), nullptr, 10)},
                                       engine.value().generation(), engine.value().incarnation());
  if (!run.ok()) {
    return fail(run.error());
  }
  if (args.has("json")) {
    tools::emit(cr::render_run_json(run.value(), true));
  } else {
    std::printf("%s\n", cr::render_run_text(run.value()).c_str());
  }
  return kExitOk;
}

int command_reopen(const tools::Args& args) {
  auto store = open_store(args, true);
  if (!store.ok()) {
    return fail(store.error());
  }
  auto id = parse_run_id(require_id(args, "run"));
  if (!id.ok()) {
    return fail(id.error());
  }
  auto instant = args.value("instant");
  if (!instant.has_value()) {
    return fail(std::string("reopen requires --instant N"));
  }
  cr::EngineOptions options;
  options.clock_domain = store.value().image().clock_domain;
  auto engine = cr::Engine::attach(std::move(store.value()), options);
  if (!engine.ok()) {
    return fail(engine.error());
  }
  auto ticket =
      engine.value().reopen(id.value(), cr::Tick{std::strtoull(instant->c_str(), nullptr, 10)},
                            engine.value().generation(), engine.value().incarnation());
  if (!ticket.ok()) {
    return fail(ticket.error());
  }
  cr::JsonWriter writer;
  writer.begin_object();
  writer.member("kind", "reopened");
  writer.member("run", ticket.value().reopened_run.to_string());
  writer.member("issued_at_generation", ticket.value().issued_at.value());
  writer.member("incarnation", ticket.value().incarnation.to_string());
  writer.end_object();
  tools::emit(std::move(writer).take());
  return kExitOk;
}

int command_close(const tools::Args& args) {
  auto store = open_store(args, true);
  if (!store.ok()) {
    return fail(store.error());
  }
  auto id = parse_run_id(require_id(args, "run"));
  if (!id.ok()) {
    return fail(id.error());
  }
  auto instant = args.value("instant");
  if (!instant.has_value()) {
    return fail(std::string("close requires --instant N"));
  }
  cr::EngineOptions options;
  options.clock_domain = store.value().image().clock_domain;
  auto engine = cr::Engine::attach(std::move(store.value()), options);
  if (!engine.ok()) {
    return fail(engine.error());
  }
  auto status = engine.value().close_run(id.value(),
                                         cr::Tick{std::strtoull(instant->c_str(), nullptr, 10)},
                                         engine.value().generation(),
                                         engine.value().incarnation());
  if (!status.ok()) {
    return fail(status.error());
  }
  cr::JsonWriter writer;
  writer.begin_object();
  writer.member("kind", "closed");
  writer.member("run", id.value().to_string());
  writer.end_object();
  tools::emit(std::move(writer).take());
  return kExitOk;
}

int command_verify(const tools::Args& args) {
  auto path = args.value("store");
  if (!path.has_value() || path->empty()) {
    return fail(std::string("verify requires --store DIR"));
  }
  const auto report = cr::Store::verify(std::filesystem::path(*path), cr::default_limits());
  if (args.has("json")) {
    tools::emit(cr::render_verification_json(report));
  } else {
    std::printf("ok                %s\n", report.ok ? "true" : "false");
    std::printf("generation        %llu\n",
                static_cast<unsigned long long>(report.generation.value()));
    std::printf("records present   %llu\n",
                static_cast<unsigned long long>(report.records_present));
    std::printf("records referenced %llu\n",
                static_cast<unsigned long long>(report.records_referenced));
    for (const std::string& problem : report.problems) {
      std::printf("problem           %s\n", problem.c_str());
    }
  }
  return report.ok ? kExitOk : kExitFailure;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fputs(kUsage, stderr);
    return kExitUsage;
  }
  const std::string command(argv[1]);
  const tools::Args args(argc, argv, 2);

  if (command == "version") {
    return command_version();
  }
  if (command == "policy") {
    return command_policy(args);
  }
  if (command == "apply") {
    return command_apply(args);
  }
  if (command == "reconcile") {
    return command_reconcile(args);
  }
  if (command == "inspect") {
    return command_inspect(args);
  }
  if (command == "history") {
    return command_history(args);
  }
  if (command == "show") {
    return command_show(args);
  }
  if (command == "diff") {
    return command_diff(args);
  }
  if (command == "revalidate") {
    return command_revalidate(args);
  }
  if (command == "reopen") {
    return command_reopen(args);
  }
  if (command == "close") {
    return command_close(args);
  }
  if (command == "verify") {
    return command_verify(args);
  }
  if (command == "--help" || command == "-h" || command == "help") {
    std::fputs(kUsage, stdout);
    return kExitOk;
  }

  std::fprintf(stderr, "unknown command '%s'\n", command.c_str());
  std::fputs(kUsage, stderr);
  return kExitUsage;
}
