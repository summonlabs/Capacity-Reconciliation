// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/capacity_reconciliation/identity.hpp"

#include <cstdlib>

#include "summon/capacity_reconciliation/limits.hpp"
#include "summon/capacity_reconciliation/platform.hpp"
#include "summon/capacity_reconciliation/quantity.hpp"

namespace summon::capacity_reconciliation {

namespace {
constexpr std::string_view kRunPrefix = "crr_";
constexpr std::string_view kEvidencePrefix = "cre_";
constexpr std::string_view kAttributionPrefix = "cra_";
constexpr std::string_view kConflictPrefix = "crx_";
constexpr std::string_view kNodePrefix = "crn_";
constexpr std::string_view kIncarnationPrefix = "cri_";
}  // namespace

std::string_view id_prefix(ReconciliationRunIdTag) noexcept { return kRunPrefix; }
std::string_view id_prefix(EvidenceIdTag) noexcept { return kEvidencePrefix; }
std::string_view id_prefix(AttributionIdTag) noexcept { return kAttributionPrefix; }
std::string_view id_prefix(ConflictIdTag) noexcept { return kConflictPrefix; }
std::string_view id_prefix(NodeIdTag) noexcept { return kNodePrefix; }
std::string_view id_prefix(IncarnationIdTag) noexcept { return kIncarnationPrefix; }

template <typename Tag>
BasicId<Tag> BasicId<Tag>::generate() {
  std::array<std::uint8_t, 16> bytes{};
  if (!platform::random_bytes(bytes.data(), bytes.size()).ok()) {
    // A CSPRNG failure means identity cannot be generated safely. Exiting is
    // strictly better than silently issuing a predictable or duplicated id.
    std::abort();
  }
  // Clear the version/variant-agnostic layout: every bit is entropy. Only the
  // all-zero case is avoided because it is reserved for "unset".
  bool any = false;
  for (std::uint8_t b : bytes) {
    any = any || (b != 0);
  }
  if (!any) {
    bytes[0] = 1;
  }
  return BasicId{bytes};
}

template class BasicId<ReconciliationRunIdTag>;
template class BasicId<EvidenceIdTag>;
template class BasicId<AttributionIdTag>;
template class BasicId<ConflictIdTag>;
template class BasicId<NodeIdTag>;
template class BasicId<IncarnationIdTag>;

Result<Generation> Generation::next() const {
  auto value = checked_increment(value_);
  if (!value.ok()) {
    return make_error(ErrorCode::Overflow,
                      "store generation exhausted: the counter cannot advance", "generation");
  }
  return Generation{value.value()};
}

Result<AttemptId> AttemptId::next() const {
  auto value = checked_increment(value_);
  if (!value.ok()) {
    return make_error(ErrorCode::Overflow, "attempt counter exhausted", "attempt");
  }
  return AttemptId{value.value()};
}

bool is_valid_token(std::string_view text, std::size_t max_bytes) noexcept {
  if (text.empty() || text.size() > max_bytes) {
    return false;
  }
  for (char raw : text) {
    const auto c = static_cast<unsigned char>(raw);
    const bool alpha = (c >= static_cast<unsigned char>('a')) &&
                       (c <= static_cast<unsigned char>('z'));
    const bool digit = (c >= static_cast<unsigned char>('0')) &&
                       (c <= static_cast<unsigned char>('9'));
    const bool punct = c == static_cast<unsigned char>('_') ||
                       c == static_cast<unsigned char>('.') ||
                       c == static_cast<unsigned char>('-') ||
                       c == static_cast<unsigned char>(':');
    if (!alpha && !digit && !punct) {
      return false;
    }
  }
  // A token may not be `.` or `..`, and may not begin with a separator-like
  // run that a filesystem could interpret.
  if (text == "." || text == "..") {
    return false;
  }
  if (text.front() == '.' || text.front() == '-') {
    return false;
  }
  return true;
}

Result<std::string> make_token(std::string_view text, std::size_t max_bytes, std::string_view what) {
  if (text.empty()) {
    return make_error(ErrorCode::InvalidArgument,
                      std::string(what) + " must not be empty", "token");
  }
  if (text.size() > max_bytes) {
    return make_error(ErrorCode::LimitExceeded,
                      std::string(what) + " exceeds the maximum length of " +
                          std::to_string(max_bytes) + " bytes",
                      "token");
  }
  if (!is_valid_token(text, max_bytes)) {
    return make_error(ErrorCode::InvalidArgument,
                      std::string(what) +
                          " must match [a-z0-9._:-]+ and may not start with '.' or '-'",
                      "token");
  }
  return std::string(text);
}

Result<ClockDomain> ClockDomain::create(std::string_view name) {
  auto token = make_token(name, kMaxClockDomainBytes, "clock domain");
  if (!token.ok()) {
    return token.error();
  }
  return ClockDomain{token.value()};
}

}  // namespace summon::capacity_reconciliation
