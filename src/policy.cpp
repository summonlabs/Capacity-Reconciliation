// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/capacity_reconciliation/policy.hpp"

#include <algorithm>
#include <array>

namespace summon::capacity_reconciliation {
namespace {

struct MissingToken {
  MissingViewPolicy policy;
  const char* token;
};

constexpr std::array<MissingToken, 2> kMissingTokens{{
    {MissingViewPolicy::Incomplete, "incomplete"},
    {MissingViewPolicy::ProceedWithPresent, "proceed_with_present"},
}};

struct StaleToken {
  StaleEvidencePolicy policy;
  const char* token;
};

constexpr std::array<StaleToken, 2> kStaleTokens{{
    {StaleEvidencePolicy::Reject, "reject"},
    {StaleEvidencePolicy::UseButMarkStale, "use_but_mark_stale"},
}};

struct RollupToken {
  RollupPolicy policy;
  const char* token;
};

constexpr std::array<RollupToken, 2> kRollupTokens{{
    {RollupPolicy::Strict, "strict"},
    {RollupPolicy::PartialSumReported, "partial_sum_reported"},
}};

constexpr std::array<EvidenceSource, 12> kCanonicalPrecedence{{
    EvidenceSource::ObservationStream,
    EvidenceSource::ReservationRegister,
    EvidenceSource::AssetRegistry,
    EvidenceSource::RackRegistry,
    EvidenceSource::LocationRegistry,
    EvidenceSource::FacilityCapacity,
    EvidenceSource::RackCapacity,
    EvidenceSource::SpaceCapacity,
    EvidenceSource::PowerCapacity,
    EvidenceSource::CoolingCapacity,
    EvidenceSource::OperatorDeclaration,
    EvidenceSource::Unknown,
}};

constexpr std::string_view kPolicyDomain = "capacity-reconciliation/policy/v1";

enum Tag : std::uint8_t {
  kTagPrecedenceCount = 1,
  kTagPrecedenceEntry = 2,
  kTagMaxGenerationLag = 3,
  kTagMissingView = 4,
  kTagStaleEvidence = 5,
  kTagRollup = 6,
  kTagRecordConflicts = 7,
  kTagReportUnexplained = 8,
  kTagDimensionCount = 9,
  kTagDimensionKey = 10,
  kTagToleranceAbsolute = 11,
  kTagTolerancePpm = 12,
  kTagRequiredViewCount = 13,
  kTagRequiredView = 14,
  kTagDefaultRequiredCount = 15,
  kTagDefaultRequired = 16,
  kTagEnd = 0x7F,
};

/// Integer magnitude of a value, refused for the minimum value.
Result<std::uint64_t> magnitude(Amount value) {
  if (value == amount_min()) {
    return make_error(ErrorCode::Overflow,
                      "tolerance arithmetic cannot represent the negation of the minimum amount",
                      "tolerance");
  }
  return static_cast<std::uint64_t>(value < 0 ? -value : value);
}

Result<std::uint64_t> scale(std::uint64_t reference, std::uint32_t parts_per_million) {
  // reference * ppm / 1'000'000 evaluated with 128-bit intermediate precision
  // by splitting the multiplication, so a large capacity cannot overflow.
  constexpr std::uint64_t kMillion = 1000000u;
  const std::uint64_t whole = reference / kMillion;
  const std::uint64_t remainder = reference % kMillion;

  auto whole_part = checked_mul_u64(whole, parts_per_million);
  if (!whole_part.ok()) {
    return whole_part.error();
  }
  // remainder < 1e6 and ppm <= 4.29e9, so remainder * ppm < 4.3e15 which fits.
  const std::uint64_t remainder_part =
      (remainder * static_cast<std::uint64_t>(parts_per_million)) / kMillion;
  return checked_add_u64(whole_part.value(), remainder_part);
}

}  // namespace

const char* to_string(MissingViewPolicy policy) noexcept {
  for (const MissingToken& entry : kMissingTokens) {
    if (entry.policy == policy) {
      return entry.token;
    }
  }
  return "unrecognised";
}

std::optional<MissingViewPolicy> missing_view_policy_from_string(std::string_view token) noexcept {
  for (const MissingToken& entry : kMissingTokens) {
    if (token == entry.token) {
      return entry.policy;
    }
  }
  return std::nullopt;
}

const char* to_string(StaleEvidencePolicy policy) noexcept {
  for (const StaleToken& entry : kStaleTokens) {
    if (entry.policy == policy) {
      return entry.token;
    }
  }
  return "unrecognised";
}

std::optional<StaleEvidencePolicy> stale_evidence_policy_from_string(
    std::string_view token) noexcept {
  for (const StaleToken& entry : kStaleTokens) {
    if (token == entry.token) {
      return entry.policy;
    }
  }
  return std::nullopt;
}

const char* to_string(RollupPolicy policy) noexcept {
  for (const RollupToken& entry : kRollupTokens) {
    if (entry.policy == policy) {
      return entry.token;
    }
  }
  return "unrecognised";
}

std::optional<RollupPolicy> rollup_policy_from_string(std::string_view token) noexcept {
  for (const RollupToken& entry : kRollupTokens) {
    if (token == entry.token) {
      return entry.policy;
    }
  }
  return std::nullopt;
}

bool operator==(const DimensionPolicy& a, const DimensionPolicy& b) noexcept {
  return a.dimension == b.dimension && a.tolerance == b.tolerance &&
         a.required_views == b.required_views;
}

ReconciliationPolicy::ReconciliationPolicy()
    : precedence_(kCanonicalPrecedence.begin(), kCanonicalPrecedence.end()),
      default_required_views_{CapacityView::Installed, CapacityView::Observed} {
  // A default-constructed policy is the standard policy, never an empty one. An
  // empty precedence list would rank every source family equally and turn
  // ordinary multi-source agreement into an unresolvable conflict, which is a
  // silent behavioural change no caller asked for.
}

ReconciliationPolicy ReconciliationPolicy::standard() { return ReconciliationPolicy{}; }

std::size_t ReconciliationPolicy::precedence_of(EvidenceSource source) const noexcept {
  for (std::size_t i = 0; i < precedence_.size(); ++i) {
    if (precedence_[i] == source) {
      return i;
    }
  }
  return precedence_.size();
}

Status ReconciliationPolicy::set_precedence(std::vector<EvidenceSource> order) {
  // The list must be the complete set of source families, with Unknown last:
  // Unknown is the "no producer" sentinel and must never outrank a real source.
  if (order.size() != kCanonicalPrecedence.size()) {
    return make_error(ErrorCode::InvalidArgument,
                      "precedence must list every evidence source family exactly once; got " +
                          std::to_string(order.size()) + " entries, expected " +
                          std::to_string(kCanonicalPrecedence.size()),
                      "precedence");
  }
  if (order.back() != EvidenceSource::Unknown) {
    return make_error(ErrorCode::InvalidArgument,
                      "precedence must end with the Unknown source family so that it can never "
                      "outrank a real source",
                      "precedence");
  }
  std::array<bool, 12> seen{};
  for (std::size_t i = 0; i + 1 < order.size(); ++i) {
    const EvidenceSource source = order[i];
    const auto index = static_cast<std::size_t>(source);
    if (source == EvidenceSource::Unknown || index >= seen.size()) {
      return make_error(ErrorCode::InvalidArgument,
                        "precedence lists the Unknown source family before the end", "precedence");
    }
    if (seen[index]) {
      return make_error(ErrorCode::AlreadyExists,
                        std::string("precedence lists '") + to_string(source) + "' more than once",
                        "precedence");
    }
    seen[index] = true;
  }
  for (std::size_t i = 0; i < seen.size(); ++i) {
    if (!seen[i] && static_cast<EvidenceSource>(i) != EvidenceSource::Unknown) {
      return make_error(ErrorCode::NotFound,
                        std::string("precedence omits source family '") +
                            to_string(static_cast<EvidenceSource>(i)) + "'",
                        "precedence");
    }
  }
  precedence_ = std::move(order);
  return Status::success();
}

void ReconciliationPolicy::set_tolerance(const DimensionKey& dimension, ToleranceBand band) {
  for (DimensionPolicy& entry : dimensions_) {
    if (entry.dimension == dimension) {
      entry.tolerance = band;
      return;
    }
  }
  DimensionPolicy entry;
  entry.dimension = dimension;
  entry.tolerance = band;
  entry.required_views = default_required_views_;
  dimensions_.push_back(std::move(entry));
  std::sort(dimensions_.begin(), dimensions_.end(),
            [](const DimensionPolicy& a, const DimensionPolicy& b) {
              return a.dimension < b.dimension;
            });
}

ToleranceBand ReconciliationPolicy::tolerance_for(const DimensionKey& dimension) const noexcept {
  for (const DimensionPolicy& entry : dimensions_) {
    if (entry.dimension == dimension) {
      return entry.tolerance;
    }
  }
  return ToleranceBand{};
}

void ReconciliationPolicy::set_required_views(const DimensionKey& dimension,
                                              std::vector<CapacityView> views) {
  std::sort(views.begin(), views.end(), [](CapacityView a, CapacityView b) {
    return view_index(a) < view_index(b);
  });
  views.erase(std::unique(views.begin(), views.end()), views.end());

  for (DimensionPolicy& entry : dimensions_) {
    if (entry.dimension == dimension) {
      entry.required_views = std::move(views);
      return;
    }
  }
  DimensionPolicy entry;
  entry.dimension = dimension;
  entry.required_views = std::move(views);
  dimensions_.push_back(std::move(entry));
  std::sort(dimensions_.begin(), dimensions_.end(),
            [](const DimensionPolicy& a, const DimensionPolicy& b) {
              return a.dimension < b.dimension;
            });
}

const std::vector<CapacityView>& ReconciliationPolicy::required_views_for(
    const DimensionKey& dimension) const noexcept {
  for (const DimensionPolicy& entry : dimensions_) {
    if (entry.dimension == dimension) {
      if (!entry.required_views.empty()) {
        return entry.required_views;
      }
      break;
    }
  }
  return default_required_views_;
}

Digest ReconciliationPolicy::digest() const {
  return digest_with_domain(kPolicyDomain, canonical_bytes());
}

std::string ReconciliationPolicy::canonical_bytes() const {
  CanonicalWriter writer;
  writer.field(kTagPrecedenceCount);
  writer.u64(static_cast<std::uint64_t>(precedence_.size()));
  for (EvidenceSource source : precedence_) {
    writer.field(kTagPrecedenceEntry);
    writer.token(to_string(source));
  }
  writer.field(kTagMaxGenerationLag);
  writer.u64(max_generation_lag);
  writer.field(kTagMissingView);
  writer.token(to_string(missing_view));
  writer.field(kTagStaleEvidence);
  writer.token(to_string(stale_evidence));
  writer.field(kTagRollup);
  writer.token(to_string(rollup));
  writer.field(kTagRecordConflicts);
  writer.boolean(record_conflict_sets);
  writer.field(kTagReportUnexplained);
  writer.boolean(report_unexplained_residuals);
  writer.field(kTagDefaultRequiredCount);
  writer.u64(static_cast<std::uint64_t>(default_required_views_.size()));
  for (CapacityView view : default_required_views_) {
    writer.field(kTagDefaultRequired);
    writer.token(to_string(view));
  }
  writer.field(kTagDimensionCount);
  writer.u64(static_cast<std::uint64_t>(dimensions_.size()));
  for (const DimensionPolicy& entry : dimensions_) {
    writer.field(kTagDimensionKey);
    writer.token(entry.dimension.to_string());
    writer.field(kTagToleranceAbsolute);
    writer.i64(entry.tolerance.absolute);
    writer.field(kTagTolerancePpm);
    writer.u32(entry.tolerance.parts_per_million);
    writer.field(kTagRequiredViewCount);
    writer.u64(static_cast<std::uint64_t>(entry.required_views.size()));
    for (CapacityView view : entry.required_views) {
      writer.field(kTagRequiredView);
      writer.token(to_string(view));
    }
  }
  writer.field(kTagEnd);
  return std::move(writer).take();
}

Result<Amount> policy_tolerance_bound(const ToleranceBand& band, Amount reference) {
  std::uint64_t bound = 0;
  if (band.parts_per_million != 0) {
    // The magnitude is only needed for the proportional term. Computing it
    // unconditionally would make a purely absolute (or zero) tolerance fail for
    // the single reference value whose magnitude is not representable, which
    // would turn a perfectly ordinary policy into a run-time failure.
    auto magnitude_value = magnitude(reference);
    if (!magnitude_value.ok()) {
      return magnitude_value.error();
    }
    auto scaled = scale(magnitude_value.value(), band.parts_per_million);
    if (!scaled.ok()) {
      return scaled.error();
    }
    bound = scaled.value();
  }
  const std::uint64_t absolute =
      band.absolute < 0 ? 0u : static_cast<std::uint64_t>(band.absolute);
  auto total = checked_add_u64(bound, absolute);
  if (!total.ok()) {
    return total.error();
  }
  return to_amount(total.value());
}

}  // namespace summon::capacity_reconciliation
