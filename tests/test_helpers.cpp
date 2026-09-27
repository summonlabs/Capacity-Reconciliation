// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "test_helpers.hpp"

#include <string>
#include <system_error>
#include <utility>

namespace crtest {

ScratchDirectory::ScratchDirectory(const std::string& label) {
  path_ = std::filesystem::temp_directory_path() /
          ("capacity-reconciliation-test-" + label + "-" +
           std::to_string(cr::platform::current_process_id()) + "-" +
           std::to_string(cr::platform::monotonic_nanoseconds()));
  reset();
}

ScratchDirectory::~ScratchDirectory() {
  std::error_code ec;
  std::filesystem::remove_all(path_, ec);
}

void ScratchDirectory::reset() {
  std::error_code ec;
  std::filesystem::remove_all(path_, ec);
  std::filesystem::create_directories(path_, ec);
}

bool ScratchDirectory::exists() const {
  std::error_code ec;
  return std::filesystem::exists(path_, ec);
}

const cr::ClockDomain& test_domain() {
  static const cr::ClockDomain domain = cr::ClockDomain::create("test-monotonic").value();
  return domain;
}

cr::DimensionKey power_key() {
  return cr::DimensionKey::create(cr::CapacityDimension::Power, cr::Unit::MilliWatt).value();
}

cr::DimensionKey space_key() {
  return cr::DimensionKey::create(cr::CapacityDimension::Space, cr::Unit::RackUnit).value();
}

EvidenceBuilder::EvidenceBuilder(std::string scope, cr::CapacityDimension dimension,
                                 cr::Unit unit)
    : scope_text_(std::move(scope)), dimension_(dimension), unit_(unit) {}

cr::Result<cr::EvidenceItem> EvidenceBuilder::build(cr::CapacityView view,
                                                    cr::Quantity quantity) const {
  auto scope = cr::ScopeIdentity::parse(scope_text_);
  if (!scope.ok()) {
    return scope.error();
  }
  auto dimension = cr::DimensionKey::create(dimension_, unit_);
  if (!dimension.ok()) {
    return dimension.error();
  }
  cr::EvidenceStamp stamp;
  stamp.generation = generation_;
  stamp.revision = revision_;
  stamp.epoch = cr::Epoch{0};
  stamp.incarnation = incarnation_;
  stamp.observed_at = observed_at_;
  stamp.valid_until = valid_until_;
  stamp.clock_domain = test_domain();
  return cr::EvidenceItem::create(cr::EvidenceId::generate(), source_, instance_, scope.value(),
                                  dimension.value(), view, quantity, status_, stamp);
}

cr::Result<cr::EvidenceItem> EvidenceBuilder::known(cr::CapacityView view, cr::Amount value) const {
  return build(view, cr::Quantity::known(value));
}

cr::Result<cr::EvidenceItem> EvidenceBuilder::unknown(cr::CapacityView view,
                                                      cr::UnknownReason reason) const {
  return build(view, cr::Quantity::unknown(reason));
}

cr::Result<cr::AttributionItem> make_attribution(
    const std::string& scope, cr::CapacityDimension dimension, cr::Unit unit,
    cr::ResidualKind target, cr::Amount amount, cr::EvidenceSource source,
    const std::string& instance, std::uint64_t generation, std::uint64_t revision,
    std::uint64_t observed_at, std::uint64_t valid_until, cr::AttributionReason reason) {
  auto parsed_scope = cr::ScopeIdentity::parse(scope);
  if (!parsed_scope.ok()) {
    return parsed_scope.error();
  }
  auto key = cr::DimensionKey::create(dimension, unit);
  if (!key.ok()) {
    return key.error();
  }
  cr::EvidenceStamp stamp;
  stamp.generation = cr::Generation{generation};
  stamp.revision = cr::Revision{revision};
  stamp.epoch = cr::Epoch{0};
  stamp.incarnation = cr::IncarnationId::generate();
  stamp.observed_at = cr::Tick{observed_at};
  stamp.valid_until = cr::Tick{valid_until};
  stamp.clock_domain = test_domain();
  return cr::AttributionItem::create(cr::AttributionId::generate(), source, instance,
                                     parsed_scope.value(), key.value(), target, reason,
                                     cr::to_string(reason), amount, stamp);
}

cr::StoreOpenOptions writer_options(bool create_if_missing) {
  cr::StoreOpenOptions options;
  options.create_if_missing = create_if_missing;
  options.writer = true;
  return options;
}

cr::StoreOpenOptions reader_options() {
  cr::StoreOpenOptions options;
  options.create_if_missing = false;
  options.writer = false;
  return options;
}

cr::RunRequest exact_request(const std::string& scope, std::uint64_t generation,
                             std::uint64_t instant) {
  cr::RunRequest request;
  cr::ScopeSelection selection;
  selection.scope = cr::ScopeIdentity::parse(scope).value();
  selection.mode = cr::ScopeSelectionMode::Exact;
  request.scopes.push_back(std::move(selection));
  request.generation = cr::Generation{generation};
  request.evaluation_instant = cr::Tick{instant};
  request.clock_domain = test_domain();
  return request;
}

cr::Result<cr::Engine> make_engine() {
  cr::EngineOptions options;
  options.clock_domain = test_domain();
  return cr::Engine::create(options);
}

namespace {

/// Skip whitespace and return the next index.
std::size_t skip_space(std::string_view text, std::size_t index) {
  while (index < text.size() &&
         (text[index] == ' ' || text[index] == '\t' || text[index] == '\n' ||
          text[index] == '\r')) {
    ++index;
  }
  return index;
}

bool parse_string(std::string_view text, std::size_t* index) {
  if (*index >= text.size() || text[*index] != '"') {
    return false;
  }
  ++(*index);
  while (*index < text.size()) {
    const char c = text[*index];
    if (c == '\\') {
      *index += 2;
      continue;
    }
    if (c == '"') {
      ++(*index);
      return true;
    }
    ++(*index);
  }
  return false;
}

bool parse_value(std::string_view text, std::size_t* index, int depth);

bool parse_object(std::string_view text, std::size_t* index, int depth) {
  if (*index >= text.size() || text[*index] != '{') {
    return false;
  }
  ++(*index);
  *index = skip_space(text, *index);
  if (*index < text.size() && text[*index] == '}') {
    ++(*index);
    return true;
  }
  while (true) {
    *index = skip_space(text, *index);
    if (!parse_string(text, index)) {
      return false;
    }
    *index = skip_space(text, *index);
    if (*index >= text.size() || text[*index] != ':') {
      return false;
    }
    ++(*index);
    *index = skip_space(text, *index);
    if (!parse_value(text, index, depth + 1)) {
      return false;
    }
    *index = skip_space(text, *index);
    if (*index >= text.size()) {
      return false;
    }
    if (text[*index] == ',') {
      ++(*index);
      *index = skip_space(text, *index);
      if (*index < text.size() && text[*index] == '}') {
        return false;  // trailing comma
      }
      continue;
    }
    if (text[*index] == '}') {
      ++(*index);
      return true;
    }
    return false;
  }
}

bool parse_array(std::string_view text, std::size_t* index, int depth) {
  if (*index >= text.size() || text[*index] != '[') {
    return false;
  }
  ++(*index);
  *index = skip_space(text, *index);
  if (*index < text.size() && text[*index] == ']') {
    ++(*index);
    return true;
  }
  while (true) {
    *index = skip_space(text, *index);
    if (!parse_value(text, index, depth + 1)) {
      return false;
    }
    *index = skip_space(text, *index);
    if (*index >= text.size()) {
      return false;
    }
    if (text[*index] == ',') {
      ++(*index);
      continue;
    }
    if (text[*index] == ']') {
      ++(*index);
      return true;
    }
    return false;
  }
}

bool parse_literal(std::string_view text, std::size_t* index) {
  static const char* kLiterals[] = {"true", "false", "null"};
  for (const char* literal : kLiterals) {
    const std::string_view candidate(literal);
    if (text.substr(*index, candidate.size()) == candidate) {
      *index += candidate.size();
      return true;
    }
  }
  if (*index < text.size() && (text[*index] == '-' || (text[*index] >= '0' && text[*index] <= '9'))) {
    ++(*index);
    while (*index < text.size() &&
           ((text[*index] >= '0' && text[*index] <= '9') || text[*index] == '.' ||
            text[*index] == 'e' || text[*index] == 'E' || text[*index] == '+' ||
            text[*index] == '-')) {
      ++(*index);
    }
    return true;
  }
  return false;
}

bool parse_value(std::string_view text, std::size_t* index, int depth) {
  if (depth > 64) {
    return false;
  }
  if (*index >= text.size()) {
    return false;
  }
  const char c = text[*index];
  if (c == '{') {
    return parse_object(text, index, depth);
  }
  if (c == '[') {
    return parse_array(text, index, depth);
  }
  if (c == '"') {
    return parse_string(text, index);
  }
  return parse_literal(text, index);
}

}  // namespace

bool json_is_well_formed(std::string_view text) {
  if (text.empty()) {
    return false;
  }
  std::size_t index = skip_space(text, 0);
  if (!parse_value(text, &index, 0)) {
    return false;
  }
  index = skip_space(text, index);
  return index == text.size();
}

std::string json_find_string(std::string_view text, std::string_view key) {
  const std::string needle = "\"" + std::string(key) + "\"";
  const std::size_t position = text.find(needle);
  if (position == std::string_view::npos) {
    return std::string();
  }
  std::size_t index = position + needle.size();
  index = skip_space(text, index);
  if (index >= text.size() || text[index] != ':') {
    return std::string();
  }
  ++index;
  index = skip_space(text, index);
  if (index >= text.size() || text[index] != '"') {
    return std::string();
  }
  ++index;
  std::string out;
  while (index < text.size() && text[index] != '"') {
    if (text[index] == '\\' && index + 1 < text.size()) {
      ++index;
    }
    out.push_back(text[index]);
    ++index;
  }
  return out;
}

}  // namespace crtest
