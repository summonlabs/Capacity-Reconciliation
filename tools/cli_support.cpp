// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "cli_support.hpp"

#include <cstdio>
#include <string>
#include <utility>

#include "summon/capacity_reconciliation/platform.hpp"
#include "summon/capacity_reconciliation/report.hpp"

namespace summon::capacity_reconciliation::tools {
namespace {

constexpr std::size_t kMaxScenarioBytes = 16u * 1024u * 1024u;
constexpr std::size_t kMaxLineBytes = 4096;
constexpr std::size_t kMaxFieldsPerRecord = 24;

std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
    text.remove_suffix(1);
  }
  return text;
}

std::vector<std::string_view> split_ws(std::string_view line) {
  std::vector<std::string_view> out;
  std::size_t index = 0;
  while (index < line.size()) {
    while (index < line.size() && (line[index] == ' ' || line[index] == '\t')) {
      ++index;
    }
    const std::size_t start = index;
    while (index < line.size() && line[index] != ' ' && line[index] != '\t') {
      ++index;
    }
    if (index > start) {
      out.push_back(line.substr(start, index - start));
    }
  }
  return out;
}

Result<std::uint64_t> parse_uint(std::string_view text, const char* what) {
  if (text.empty() || text.size() > kMaxNumericTokenBytes) {
    return make_error(ErrorCode::InvalidArgument,
                      std::string("field '") + what + "' is not a decimal integer", "field");
  }
  std::uint64_t value = 0;
  for (char c : text) {
    if (c < '0' || c > '9') {
      return make_error(ErrorCode::InvalidArgument,
                        std::string("field '") + what + "' is not a decimal integer", "field");
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u) {
      return make_error(ErrorCode::Overflow,
                        std::string("field '") + what + "' overflows a 64-bit counter", "field");
    }
    value = (value * 10u) + digit;
  }
  return value;
}

Result<Amount> parse_amount(std::string_view text, const char* what) {
  bool negative = false;
  if (!text.empty() && (text.front() == '-' || text.front() == '+')) {
    negative = text.front() == '-';
    text.remove_prefix(1);
  }
  auto magnitude = parse_uint(text, what);
  if (!magnitude.ok()) {
    return magnitude.error();
  }
  if (magnitude.value() >
      static_cast<std::uint64_t>(negative ? amount_max() : amount_max()) + (negative ? 1u : 0u)) {
    return make_error(ErrorCode::Overflow,
                      std::string("field '") + what + "' is outside the exact amount domain",
                      "field");
  }
  const Amount value = static_cast<Amount>(magnitude.value());
  return negative ? -value : value;
}

Result<Quantity> parse_quantity(std::string_view text, const char* what) {
  if (text.rfind("unknown:", 0) == 0) {
    auto reason = unknown_reason_from_string(text.substr(8));
    if (!reason.has_value()) {
      return make_error(ErrorCode::InvalidArgument,
                        std::string("field '") + what + "' names an unknown reason that is not "
                        "recognised: '" + std::string(text) + "'",
                        "field");
    }
    return Quantity::unknown(*reason);
  }
  auto amount = parse_amount(text, what);
  if (!amount.ok()) {
    return amount.error();
  }
  return Quantity::known(amount.value());
}

Result<EvidenceStamp> parse_stamp(const Record& record, const ClockDomain& domain) {
  EvidenceStamp stamp;
  stamp.clock_domain = domain;
  auto generation = record.require_uint("gen");
  if (!generation.ok()) {
    return generation.error();
  }
  stamp.generation = Generation{generation.value()};
  auto revision = record.optional_uint("rev", 1);
  stamp.revision = Revision{revision};
  auto epoch = record.optional_uint("epoch", 0);
  stamp.epoch = Epoch{epoch};
  stamp.incarnation = IncarnationId::generate();
  auto observed = record.require_uint("obs");
  if (!observed.ok()) {
    return observed.error();
  }
  stamp.observed_at = Tick{observed.value()};
  const std::uint64_t until = record.optional_uint("until", observed.value());
  stamp.valid_until = Tick{until};
  return stamp;
}

}  // namespace

std::optional<std::string_view> Record::get(std::string_view key) const {
  for (const auto& field : fields) {
    if (field.first == key) {
      return field.second;
    }
  }
  return std::nullopt;
}

Result<std::string> Record::require(std::string_view key) const {
  auto value = get(key);
  if (!value.has_value()) {
    return make_error(ErrorCode::InvalidArgument,
                      "line " + std::to_string(line) + ": missing required field '" +
                          std::string(key) + "'",
                      "field");
  }
  return std::string(*value);
}

Result<std::int64_t> Record::require_int(std::string_view key) const {
  auto text = require(key);
  if (!text.ok()) {
    return text.error();
  }
  auto amount = parse_amount(text.value(), std::string(key).c_str());
  if (!amount.ok()) {
    return amount.error();
  }
  return amount.value();
}

Result<std::uint64_t> Record::require_uint(std::string_view key) const {
  auto text = require(key);
  if (!text.ok()) {
    return text.error();
  }
  return parse_uint(text.value(), std::string(key).c_str());
}

std::uint64_t Record::optional_uint(std::string_view key, std::uint64_t fallback) const {
  auto value = get(key);
  if (!value.has_value()) {
    return fallback;
  }
  auto parsed = parse_uint(*value, "value");
  return parsed.ok() ? parsed.value() : fallback;
}

Result<Scenario> parse_scenario(std::string_view text, std::string_view default_domain) {
  Scenario scenario;
  scenario.clock_domain = std::string(default_domain);

  std::size_t line_number = 0;
  std::size_t offset = 0;
  while (offset <= text.size()) {
    const std::size_t newline = text.find('\n', offset);
    const std::string_view raw =
        (newline == std::string_view::npos) ? text.substr(offset) : text.substr(offset, newline - offset);
    offset = (newline == std::string_view::npos) ? text.size() + 1 : newline + 1;
    ++line_number;

    if (raw.size() > kMaxLineBytes) {
      return make_error(ErrorCode::LimitExceeded,
                        "line " + std::to_string(line_number) + " exceeds " +
                            std::to_string(kMaxLineBytes) + " bytes",
                        "line_length");
    }
    std::string_view line = trim(raw);
    if (line.empty() || line.front() == '#') {
      continue;
    }
    if (line.rfind("//", 0) == 0) {
      continue;
    }

    const std::vector<std::string_view> tokens = split_ws(line);
    if (tokens.empty()) {
      continue;
    }
    if (tokens.size() > kMaxFieldsPerRecord + 1) {
      return make_error(ErrorCode::LimitExceeded,
                        "line " + std::to_string(line_number) + " declares too many fields",
                        "field_count");
    }

    Record record;
    record.line = line_number;
    record.kind = std::string(tokens.front());
    for (std::size_t i = 1; i < tokens.size(); ++i) {
      const std::size_t equals = tokens[i].find('=');
      if (equals == std::string_view::npos || equals == 0) {
        return make_error(ErrorCode::Malformed,
                          "line " + std::to_string(line_number) +
                              ": expected key=value, found '" + std::string(tokens[i]) + "'",
                          "field");
      }
      const std::string key(tokens[i].substr(0, equals));
      const std::string value(tokens[i].substr(equals + 1));
      for (const auto& existing : record.fields) {
        if (existing.first == key) {
          return make_error(ErrorCode::AlreadyExists,
                            "line " + std::to_string(line_number) + ": field '" + key +
                                "' appears more than once",
                            "field");
        }
      }
      record.fields.emplace_back(key, value);
    }
    scenario.records.push_back(std::move(record));
  }

  // Second pass: the clock domain must be known before stamps are built.
  for (const Record& record : scenario.records) {
    if (record.kind == "domain") {
      auto name = record.require("name");
      if (!name.ok()) {
        return name.error();
      }
      auto domain = ClockDomain::create(name.value());
      if (!domain.ok()) {
        return domain.error();
      }
      scenario.clock_domain = domain.value().name();
      break;
    }
  }
  if (scenario.clock_domain.empty()) {
    return make_error(ErrorCode::InvalidArgument,
                      "no clock domain: supply one on the command line or with a 'domain "
                      "name=...' line",
                      "clock_domain");
  }
  auto domain = ClockDomain::create(scenario.clock_domain);
  if (!domain.ok()) {
    return domain.error();
  }

  for (const Record& record : scenario.records) {
    const std::string& kind = record.kind;

    if (kind == "domain") {
      continue;
    }
    if (kind == "instant") {
      auto value = record.require_uint("at");
      if (!value.ok()) {
        return value.error();
      }
      scenario.instant = value.value();
      continue;
    }
    if (kind == "scope") {
      auto path = record.require("path");
      if (!path.ok()) {
        return path.error();
      }
      auto scope = ScopeIdentity::parse(path.value());
      if (!scope.ok()) {
        return scope.error();
      }
      ScopeSelection selection;
      selection.scope = std::move(scope.value());
      auto mode = record.get("mode");
      if (mode.has_value()) {
        auto parsed = scope_selection_mode_from_string(*mode);
        if (!parsed.has_value()) {
          return make_error(ErrorCode::InvalidArgument,
                            "line " + std::to_string(record.line) +
                                ": scope mode must be 'exact' or 'subtree_rollup'",
                            "scope_mode");
        }
        selection.mode = *parsed;
      }
      scenario.scopes.push_back(std::move(selection));
      continue;
    }
    if (kind == "policy") {
      auto lag = record.get("max_generation_lag");
      if (lag.has_value()) {
        auto parsed = parse_uint(*lag, "max_generation_lag");
        if (!parsed.ok()) {
          return parsed.error();
        }
        scenario.policy.max_generation_lag = parsed.value();
      }
      auto stale = record.get("stale_evidence");
      if (stale.has_value()) {
        auto parsed = stale_evidence_policy_from_string(*stale);
        if (!parsed.has_value()) {
          return make_error(ErrorCode::InvalidArgument,
                            "line " + std::to_string(record.line) +
                                ": stale_evidence must be 'reject' or 'use_but_mark_stale'",
                            "stale_evidence");
        }
        scenario.policy.stale_evidence = *parsed;
      }
      auto missing = record.get("missing_view");
      if (missing.has_value()) {
        auto parsed = missing_view_policy_from_string(*missing);
        if (!parsed.has_value()) {
          return make_error(ErrorCode::InvalidArgument,
                            "line " + std::to_string(record.line) +
                                ": missing_view must be 'incomplete' or 'proceed_with_present'",
                            "missing_view");
        }
        scenario.policy.missing_view = *parsed;
      }
      auto rollup = record.get("rollup");
      if (rollup.has_value()) {
        auto parsed = rollup_policy_from_string(*rollup);
        if (!parsed.has_value()) {
          return make_error(ErrorCode::InvalidArgument,
                            "line " + std::to_string(record.line) +
                                ": rollup must be 'strict' or 'partial_sum_reported'",
                            "rollup");
        }
        scenario.policy.rollup = *parsed;
      }
      auto tolerance = record.get("tolerance");
      if (tolerance.has_value()) {
        auto dimension = record.require("dim");
        if (!dimension.ok()) {
          return dimension.error();
        }
        auto key = DimensionKey::parse(dimension.value());
        if (!key.has_value()) {
          return make_error(ErrorCode::InvalidArgument,
                            "line " + std::to_string(record.line) +
                                ": 'dim' is not a recognised dimension key",
                            "dimension");
        }
        auto amount = parse_amount(*tolerance, "tolerance");
        if (!amount.ok()) {
          return amount.error();
        }
        ToleranceBand band;
        band.absolute = amount.value();
        scenario.policy.set_tolerance(*key, band);
      }
      continue;
    }

    const bool is_evidence = kind == "evidence";
    const bool is_attribution = kind == "attribution";
    if (!is_evidence && !is_attribution) {
      return make_error(ErrorCode::Unsupported,
                        "line " + std::to_string(record.line) + ": unknown record kind '" + kind +
                            "'",
                        "record_kind");
    }

    auto source_text = record.require("source");
    if (!source_text.ok()) {
      return source_text.error();
    }
    auto source = evidence_source_from_string(source_text.value());
    if (!source.has_value()) {
      return make_error(ErrorCode::InvalidArgument,
                        "line " + std::to_string(record.line) +
                            ": source family is not recognised: '" + source_text.value() + "'",
                        "evidence_source");
    }
    auto instance = record.require("instance");
    if (!instance.ok()) {
      return instance.error();
    }
    auto scope_text = record.require("scope");
    if (!scope_text.ok()) {
      return scope_text.error();
    }
    auto scope = ScopeIdentity::parse(scope_text.value());
    if (!scope.ok()) {
      return scope.error();
    }
    auto dim_text = record.require("dim");
    if (!dim_text.ok()) {
      return dim_text.error();
    }
    auto dimension = DimensionKey::parse(dim_text.value());
    if (!dimension.has_value()) {
      return make_error(ErrorCode::InvalidArgument,
                        "line " + std::to_string(record.line) +
                            ": 'dim' is not a recognised dimension key: '" + dim_text.value() +
                            "'",
                        "dimension");
    }
    auto stamp = parse_stamp(record, domain.value());
    if (!stamp.ok()) {
      return stamp.error();
    }

    if (is_evidence) {
      auto view_text = record.require("view");
      if (!view_text.ok()) {
        return view_text.error();
      }
      auto view = capacity_view_from_string(view_text.value());
      if (!view.has_value()) {
        return make_error(ErrorCode::InvalidArgument,
                          "line " + std::to_string(record.line) +
                              ": view is not recognised: '" + view_text.value() + "'",
                          "capacity_view");
      }
      auto value_text = record.require("value");
      if (!value_text.ok()) {
        return value_text.error();
      }
      auto value = parse_quantity(value_text.value(), "value");
      if (!value.ok()) {
        return value.error();
      }
      EvidenceStatus status = EvidenceStatus::Accepted;
      auto status_text = record.get("status");
      if (status_text.has_value()) {
        auto parsed = evidence_status_from_string(*status_text);
        if (!parsed.has_value()) {
          return make_error(ErrorCode::InvalidArgument,
                            "line " + std::to_string(record.line) +
                                ": status is not recognised: '" + std::string(*status_text) + "'",
                            "evidence_status");
        }
        status = *parsed;
      }
      auto item = EvidenceItem::create(EvidenceId::generate(), *source, instance.value(),
                                       std::move(scope.value()), *dimension, *view,
                                       value.value(), status, stamp.value());
      if (!item.ok()) {
        return item.error();
      }
      scenario.evidence.push_back(std::move(item.value()));
      continue;
    }

    auto target_text = record.require("target");
    if (!target_text.ok()) {
      return target_text.error();
    }
    auto target = residual_kind_from_string(target_text.value());
    if (!target.has_value()) {
      return make_error(ErrorCode::InvalidArgument,
                        "line " + std::to_string(record.line) +
                            ": residual target is not recognised: '" + target_text.value() + "'",
                        "residual_kind");
    }
    auto reason_text = record.require("reason");
    if (!reason_text.ok()) {
      return reason_text.error();
    }
    auto reason = attribution_reason_from_string(reason_text.value());
    if (!reason.has_value()) {
      return make_error(ErrorCode::InvalidArgument,
                        "line " + std::to_string(record.line) +
                            ": attribution reason is not recognised: '" + reason_text.value() +
                            "'",
                        "attribution_reason");
    }
    auto amount = record.require_int("amount");
    if (!amount.ok()) {
      return amount.error();
    }
    std::string reason_token = reason_text.value();
    auto token_text = record.get("token");
    if (token_text.has_value()) {
      reason_token = std::string(*token_text);
    }
    auto item = AttributionItem::create(AttributionId::generate(), *source, instance.value(),
                                        std::move(scope.value()), *dimension, *target, *reason,
                                        reason_token, amount.value(), stamp.value());
    if (!item.ok()) {
      return item.error();
    }
    scenario.attributions.push_back(std::move(item.value()));
  }

  if (scenario.scopes.empty()) {
    return make_error(ErrorCode::InvalidArgument,
                      "the scenario selects no scope: add at least one 'scope path=...' line",
                      "scopes");
  }
  return scenario;
}

Result<std::string> read_text_file(const std::string& path, std::size_t max_bytes) {
  auto bytes = platform::read_file_bounded(std::filesystem::path(path), max_bytes);
  if (!bytes.ok()) {
    return bytes.error();
  }
  return bytes.value();
}

Args::Args(int argc, char** argv, std::size_t first) {
  for (std::size_t i = first; i < static_cast<std::size_t>(argc); ++i) {
    const std::string token(argv[i]);
    if (token.rfind("--", 0) == 0) {
      const std::size_t equals = token.find('=');
      if (equals != std::string::npos) {
        options_.emplace_back(token.substr(2, equals - 2), token.substr(equals + 1));
        continue;
      }
      const std::string name = token.substr(2);
      if (i + 1 < static_cast<std::size_t>(argc) && argv[i + 1][0] != '-') {
        options_.emplace_back(name, std::string(argv[i + 1]));
        ++i;
        continue;
      }
      options_.emplace_back(name, std::string());
      continue;
    }
    positional_.push_back(token);
  }
}

bool Args::has(std::string_view flag) const {
  for (const auto& option : options_) {
    if (option.first == flag) {
      return true;
    }
  }
  return false;
}

std::optional<std::string> Args::value(std::string_view key) const {
  for (const auto& option : options_) {
    if (option.first == key) {
      return option.second;
    }
  }
  return std::nullopt;
}

std::string Args::value_or(std::string_view key, std::string fallback) const {
  auto found = value(key);
  if (!found.has_value() || found->empty()) {
    return fallback;
  }
  return *found;
}

void emit(const std::string& json) {
  std::fwrite(json.data(), 1, json.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

void emit_error(const Error& error) {
  const std::string text = error.to_string();
  std::fwrite(text.data(), 1, text.size(), stderr);
  std::fputc('\n', stderr);
  std::fflush(stderr);
}

}  // namespace summon::capacity_reconciliation::tools
