// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Shared helpers for the command line tools: a strict, bounded, line-oriented
// scenario format and small argument utilities.
//
// The scenario format is deliberately simple and fails loudly. It is *not* a
// general configuration language: there is no expression evaluation, no
// includes, no environment substitution and no unbounded field. Every token is
// `key=value`, every key is known, and an unknown or duplicated key is an
// error. That is what makes it safe to feed operator-supplied files to it.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "summon/capacity_reconciliation/engine.hpp"

namespace summon::capacity_reconciliation::tools {

/// One parsed `key=value` line plus its source line number.
struct Record {
  std::string kind;
  std::vector<std::pair<std::string, std::string>> fields;
  std::size_t line = 0;

  [[nodiscard]] std::optional<std::string_view> get(std::string_view key) const;
  [[nodiscard]] Result<std::string> require(std::string_view key) const;
  [[nodiscard]] Result<std::int64_t> require_int(std::string_view key) const;
  [[nodiscard]] Result<std::uint64_t> require_uint(std::string_view key) const;
  [[nodiscard]] std::uint64_t optional_uint(std::string_view key, std::uint64_t fallback) const;
};

/// A whole parsed scenario file.
struct Scenario {
  std::vector<Record> records;
  /// Line number of the first `instant` line, when present.
  std::optional<std::uint64_t> instant;
  std::string clock_domain;
  std::vector<ScopeSelection> scopes;
  std::vector<EvidenceItem> evidence;
  std::vector<AttributionItem> attributions;
  ReconciliationPolicy policy = ReconciliationPolicy::standard();
};

/// Parse a scenario file. `default_domain` is used when the file declares none.
[[nodiscard]] Result<Scenario> parse_scenario(std::string_view text,
                                              std::string_view default_domain);

/// Read a file with a bound. Refuses anything larger than `max_bytes`.
[[nodiscard]] Result<std::string> read_text_file(const std::string& path, std::size_t max_bytes);

/// Minimal argument reader: positional arguments plus `--flag` and
/// `--key value` options, with unknown options rejected.
class Args {
 public:
  Args(int argc, char** argv, std::size_t first);

  [[nodiscard]] bool has(std::string_view flag) const;
  [[nodiscard]] std::optional<std::string> value(std::string_view key) const;
  [[nodiscard]] std::string value_or(std::string_view key, std::string fallback) const;
  [[nodiscard]] const std::vector<std::string>& positional() const { return positional_; }
  [[nodiscard]] const std::vector<std::string>& problems() const { return problems_; }

 private:
  std::vector<std::string> positional_;
  std::vector<std::pair<std::string, std::string>> options_;
  std::vector<std::string> problems_;
};

/// Print a JSON document to stdout followed by a newline.
void emit(const std::string& json);

/// Print an error to stderr in a stable, single-line form.
void emit_error(const Error& error);

}  // namespace summon::capacity_reconciliation::tools
