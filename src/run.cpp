// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/capacity_reconciliation/run.hpp"

#include <algorithm>

#include "summon/capacity_reconciliation/version.hpp"

#include "reconcile.hpp"

namespace summon::capacity_reconciliation {
namespace {

constexpr std::string_view kRunDomain = "capacity-reconciliation/run/v1";

/// Number of residuals computed for every cell. Fixed by the canonical schema.
constexpr std::uint64_t kResidualCount = 8;

struct HistoryToken {
  HistoryEventKind kind;
  const char* token;
};

constexpr std::array<HistoryToken, 10> kHistoryTokens{{
    {HistoryEventKind::RunComputed, "run_computed"},
    {HistoryEventKind::RunCommitted, "run_committed"},
    {HistoryEventKind::RunSuperseded, "run_superseded"},
    {HistoryEventKind::RunClosed, "run_closed"},
    {HistoryEventKind::RunReopened, "run_reopened"},
    {HistoryEventKind::EvidenceAppended, "evidence_appended"},
    {HistoryEventKind::AttributionAppended, "attribution_appended"},
    {HistoryEventKind::PolicyChanged, "policy_changed"},
    {HistoryEventKind::StoreRecovered, "store_recovered"},
    {HistoryEventKind::RecordsRetired, "records_retired"},
}};

enum Tag : std::uint8_t {
  kTagSchema = 1,
  kTagScopeCount = 2,
  kTagScopeMode = 3,
  kTagScopePath = 4,
  kTagGeneration = 5,
  kTagEvaluationInstant = 6,
  kTagClockDomain = 7,
  kTagPolicyDigest = 8,
  kTagMinEvidenceGeneration = 9,
  kTagHasParent = 10,
  kTagParent = 11,
  kTagFromRecovered = 12,
  kTagLabel = 13,
  kTagEvidenceDigest = 14,
  kTagAttributionDigest = 15,
  kTagCellCount = 16,
  kTagCell = 17,
  kTagCellScope = 18,
  kTagCellDimension = 19,
  kTagViewPresent = 20,
  kTagViewValue = 21,
  kTagViewSelected = 22,
  kTagViewSource = 23,
  kTagViewCandidates = 24,
  kTagViewPartial = 25,
  kTagResiduals = 26,
  kTagFindingCount = 27,
  kTagFinding = 28,
  kTagConflictCount = 29,
  kTagConflict = 30,
  kTagSiblingUnitCount = 31,
  kTagSiblingUnit = 32,
  kTagEvidenceSeen = 33,
  kTagAttributionsApplied = 34,
  kTagRolledUp = 35,
  kTagRollupContributors = 36,
  kTagRollupMissing = 37,
  kTagEnd = 0x7F,
};

enum EnvelopeTag : std::uint8_t {
  kEnvId = 1,
  kEnvContent = 2,
  kEnvResolution = 3,
  kEnvAttempt = 4,
  kEnvHasCommitted = 5,
  kEnvCommitted = 6,
  kEnvHasSupersededBy = 7,
  kEnvSupersededBy = 8,
  kEnvEnd = 0x7F,
};

void write_quantity_field(CanonicalWriter& writer, std::uint8_t tag, const Quantity& quantity) {
  writer.field(tag);
  writer.boolean(quantity.is_known());
  if (quantity.is_known()) {
    writer.i64(quantity.value());
  } else {
    writer.token(to_string(quantity.reason()));
  }
}

Result<Quantity> read_quantity_field(CanonicalReader& reader, std::uint8_t tag, const char* what) {
  auto status = reader.field(tag, what);
  if (!status.ok()) {
    return status.error();
  }
  auto known = reader.boolean();
  if (!known.ok()) {
    return known.error();
  }
  if (known.value()) {
    auto value = reader.i64();
    if (!value.ok()) {
      return value.error();
    }
    return Quantity::known(value.value());
  }
  auto reason = reader.token(48, what);
  if (!reason.ok()) {
    return reason.error();
  }
  auto parsed = unknown_reason_from_string(reason.value());
  if (!parsed.has_value()) {
    return make_error(ErrorCode::Malformed,
                      std::string("stored unknown reason is not recognised for ") + what,
                      "unknown_reason");
  }
  return Quantity::unknown(*parsed);
}

}  // namespace

const char* to_string(HistoryEventKind kind) noexcept {
  for (const HistoryToken& entry : kHistoryTokens) {
    if (entry.kind == kind) {
      return entry.token;
    }
  }
  return "unrecognised";
}

std::optional<HistoryEventKind> history_event_kind_from_string(std::string_view token) noexcept {
  for (const HistoryToken& entry : kHistoryTokens) {
    if (token == entry.token) {
      return entry.kind;
    }
  }
  return std::nullopt;
}

void encode_request(CanonicalWriter& writer, const RunRequest& request) {
  writer.field(kTagScopeCount);
  writer.u64(static_cast<std::uint64_t>(request.scopes.size()));
  for (const ScopeSelection& selection : request.scopes) {
    writer.field(kTagScopeMode);
    writer.token(to_string(selection.mode));
    writer.field(kTagScopePath);
    writer.bytes(selection.scope.to_string());
  }
  writer.field(kTagGeneration);
  writer.u64(request.generation.value());
  writer.field(kTagEvaluationInstant);
  writer.u64(request.evaluation_instant.value());
  writer.field(kTagClockDomain);
  writer.token(request.clock_domain.name());
  writer.field(kTagPolicyDigest);
  writer.bytes(request.policy.digest().to_hex());
  writer.field(kTagMinEvidenceGeneration);
  writer.u64(request.min_evidence_generation.value());
  writer.field(kTagHasParent);
  writer.boolean(request.parent_run.has_value());
  if (request.parent_run.has_value()) {
    writer.field(kTagParent);
    writer.bytes(request.parent_run->to_string());
  }
  writer.field(kTagFromRecovered);
  writer.boolean(request.from_recovered_state);
  writer.field(kTagLabel);
  writer.token(request.label.empty() ? std::string_view("run") : std::string_view(request.label));
}

void encode_cell(CanonicalWriter& writer, const ReconciliationCell& cell) {
  writer.field(kTagCellScope);
  writer.bytes(cell.scope.to_string());
  writer.field(kTagCellDimension);
  writer.token(cell.dimension.to_string());

  for (std::size_t i = 0; i < kCapacityViewCount; ++i) {
    const auto view = static_cast<CapacityView>(i);
    writer.field(kTagViewPresent);
    writer.boolean(cell.views.selected[i].has_value());
    if (cell.views.selected[i].has_value()) {
      writer.field(kTagViewSelected);
      writer.bytes(cell.views.selected[i]->to_string());
      writer.field(kTagViewSource);
      writer.token(to_string(cell.views.selected_source[i]));
    }
    writer.field(kTagViewCandidates);
    writer.u32(cell.views.candidate_count[i]);
    writer.field(kTagViewPartial);
    writer.boolean(cell.views.partial[i]);
    write_quantity_field(writer, kTagViewValue, cell.views.views[i]);
    (void)view;
  }

  writer.field(kTagResiduals);
  const Quantity* residuals[kResidualCount] = {
      &cell.planned_gap,       &cell.observed_gap,          &cell.derate_gap,
      &cell.headroom_gap,      &cell.reservation_pressure,  &cell.reservation_overhang,
      &cell.derived_allocatable, &cell.allocatable_skew,
  };
  writer.u64(static_cast<std::uint64_t>(kResidualCount));
  for (const Quantity* quantity : residuals) {
    write_quantity_field(writer, kTagResiduals, *quantity);
  }

  writer.field(kTagFindingCount);
  writer.u64(static_cast<std::uint64_t>(cell.findings.size()));
  for (const Finding& finding : cell.findings) {
    writer.field(kTagFinding);
    writer.token(to_string(finding.classification));
    writer.token(to_string(finding.target));
    writer.boolean(finding.magnitude.has_value());
    if (finding.magnitude.has_value()) {
      writer.i64(*finding.magnitude);
    }
    writer.i64(finding.tolerance);
    writer.boolean(finding.exceeds_tolerance);
  }

  writer.field(kTagConflictCount);
  writer.u64(static_cast<std::uint64_t>(cell.conflicts.size()));
  for (const EvidenceConflict& conflict : cell.conflicts) {
    writer.field(kTagConflict);
    writer.bytes(conflict.id.to_string());
    writer.bytes(conflict.scope.to_string());
    writer.token(conflict.dimension.to_string());
    writer.token(to_string(conflict.view));
    writer.u64(static_cast<std::uint64_t>(conflict.participants.size()));
    for (std::size_t i = 0; i < conflict.participants.size(); ++i) {
      writer.bytes(conflict.participants[i].to_string());
      writer.bytes(conflict.participant_digests[i].to_hex());
    }
    writer.u64(static_cast<std::uint64_t>(conflict.values.size()));
    for (std::size_t i = 0; i < conflict.values.size(); ++i) {
      writer.i64(conflict.values[i]);
      writer.token(to_string(conflict.asserting_sources[i]));
    }
    writer.token(conflict.unresolvable_reason);
    writer.boolean(conflict.selected_value.has_value());
    if (conflict.selected_value.has_value()) {
      writer.i64(*conflict.selected_value);
    }
  }

  writer.field(kTagSiblingUnitCount);
  writer.u64(static_cast<std::uint64_t>(cell.sibling_units.size()));
  for (Unit unit : cell.sibling_units) {
    writer.field(kTagSiblingUnit);
    writer.token(to_string(unit));
  }

  writer.field(kTagEvidenceSeen);
  writer.u64(cell.evidence_seen);
  writer.field(kTagAttributionsApplied);
  writer.u64(cell.attributions_applied);
  writer.field(kTagRolledUp);
  writer.boolean(cell.rolled_up);
  writer.field(kTagRollupContributors);
  writer.u32(cell.rollup_contributors);
  writer.field(kTagRollupMissing);
  writer.u32(cell.rollup_missing);

  cell.explanation.encode(writer);
}

Result<ReconciliationCell> decode_cell(CanonicalReader& reader, const Limits& limits) {
  ReconciliationCell cell;

  auto status = reader.field(kTagCellScope, "cell scope");
  if (!status.ok()) {
    return status.error();
  }
  auto scope_text = reader.bytes(kMaxScopeDepth * (kMaxScopeSegmentBytes + 1u), "cell scope");
  if (!scope_text.ok()) {
    return scope_text.error();
  }
  auto scope = ScopeIdentity::parse(scope_text.value());
  if (!scope.ok()) {
    return scope.error();
  }
  if (scope.value().to_string() != scope_text.value()) {
    return make_error(ErrorCode::Malformed, "stored cell scope is not canonical", "scope");
  }
  cell.scope = scope.value();

  status = reader.field(kTagCellDimension, "cell dimension");
  if (!status.ok()) {
    return status.error();
  }
  auto dimension_text = reader.bytes(64, "cell dimension");
  if (!dimension_text.ok()) {
    return dimension_text.error();
  }
  auto dimension = DimensionKey::parse(dimension_text.value());
  if (!dimension.has_value()) {
    return make_error(ErrorCode::Malformed, "stored cell dimension is not recognised",
                      "dimension");
  }
  cell.dimension = *dimension;

  for (std::size_t i = 0; i < kCapacityViewCount; ++i) {
    status = reader.field(kTagViewPresent, "cell view presence");
    if (!status.ok()) {
      return status.error();
    }
    auto present = reader.boolean();
    if (!present.ok()) {
      return present.error();
    }
    if (present.value()) {
      status = reader.field(kTagViewSelected, "cell view selection");
      if (!status.ok()) {
        return status.error();
      }
      auto selected_text = reader.bytes(64, "cell view selection");
      if (!selected_text.ok()) {
        return selected_text.error();
      }
      auto selected = EvidenceId::parse(selected_text.value());
      if (!selected.has_value()) {
        return make_error(ErrorCode::Malformed, "stored selected evidence identity is malformed",
                          "evidence_id");
      }
      cell.views.selected[i] = *selected;

      status = reader.field(kTagViewSource, "cell view source");
      if (!status.ok()) {
        return status.error();
      }
      auto source_text = reader.token(48, "cell view source");
      if (!source_text.ok()) {
        return source_text.error();
      }
      auto source = evidence_source_from_string(source_text.value());
      if (!source.has_value()) {
        return make_error(ErrorCode::Malformed, "stored view source is not recognised",
                          "evidence_source");
      }
      cell.views.selected_source[i] = *source;
    }

    status = reader.field(kTagViewCandidates, "cell view candidate count");
    if (!status.ok()) {
      return status.error();
    }
    auto candidates = reader.u32();
    if (!candidates.ok()) {
      return candidates.error();
    }
    if (candidates.value() > limits.max_evidence_per_cell) {
      return make_error(ErrorCode::LimitExceeded,
                        "stored candidate count exceeds the configured per-cell evidence limit",
                        "candidate_count");
    }
    cell.views.candidate_count[i] = candidates.value();

    status = reader.field(kTagViewPartial, "cell view partial flag");
    if (!status.ok()) {
      return status.error();
    }
    auto partial = reader.boolean();
    if (!partial.ok()) {
      return partial.error();
    }
    cell.views.partial[i] = partial.value();

    auto value = read_quantity_field(reader, kTagViewValue, "cell view value");
    if (!value.ok()) {
      return value.error();
    }
    cell.views.views[i] = value.value();
  }

  status = reader.field(kTagResiduals, "cell residuals");
  if (!status.ok()) {
    return status.error();
  }
  auto residual_count = reader.u64();
  if (!residual_count.ok()) {
    return residual_count.error();
  }
  if (residual_count.value() != kResidualCount) {
    return make_error(ErrorCode::Malformed,
                      "stored cell declares " + std::to_string(residual_count.value()) +
                          " residuals; this format version has exactly " +
                          std::to_string(kResidualCount),
                      "residual_count");
  }
  Quantity* residuals[kResidualCount] = {
      &cell.planned_gap,       &cell.observed_gap,          &cell.derate_gap,
      &cell.headroom_gap,      &cell.reservation_pressure,  &cell.reservation_overhang,
      &cell.derived_allocatable, &cell.allocatable_skew,
  };
  for (Quantity* quantity : residuals) {
    auto value = read_quantity_field(reader, kTagResiduals, "cell residual");
    if (!value.ok()) {
      return value.error();
    }
    *quantity = value.value();
  }

  status = reader.field(kTagFindingCount, "cell finding count");
  if (!status.ok()) {
    return status.error();
  }
  auto finding_count = reader.u64();
  if (!finding_count.ok()) {
    return finding_count.error();
  }
  if (finding_count.value() > 64) {
    return make_error(ErrorCode::LimitExceeded, "stored finding count is out of range",
                      "finding_count");
  }
  for (std::uint64_t i = 0; i < finding_count.value(); ++i) {
    status = reader.field(kTagFinding, "cell finding");
    if (!status.ok()) {
      return status.error();
    }
    auto class_text = reader.token(48, "finding class");
    if (!class_text.ok()) {
      return class_text.error();
    }
    auto classification = discrepancy_class_from_string(class_text.value());
    if (!classification.has_value()) {
      return make_error(ErrorCode::Malformed, "stored finding class is not recognised",
                        "discrepancy_class");
    }
    auto target_text = reader.token(48, "finding target");
    if (!target_text.ok()) {
      return target_text.error();
    }
    Finding finding;
    finding.classification = *classification;
    bool target_matched = false;
    for (std::uint8_t candidate = 0; candidate <= 8 && !target_matched; ++candidate) {
      const auto target = static_cast<ReasonTarget>(candidate);
      if (target_text.value() == to_string(target)) {
        finding.target = target;
        target_matched = true;
      }
    }
    if (!target_matched) {
      return make_error(ErrorCode::Malformed, "stored finding target is not recognised",
                        "reason_target");
    }
    auto has_magnitude = reader.boolean();
    if (!has_magnitude.ok()) {
      return has_magnitude.error();
    }
    if (has_magnitude.value()) {
      auto magnitude = reader.i64();
      if (!magnitude.ok()) {
        return magnitude.error();
      }
      finding.magnitude = magnitude.value();
    }
    auto tolerance = reader.i64();
    if (!tolerance.ok()) {
      return tolerance.error();
    }
    finding.tolerance = tolerance.value();
    auto exceeds = reader.boolean();
    if (!exceeds.ok()) {
      return exceeds.error();
    }
    finding.exceeds_tolerance = exceeds.value();
    cell.findings.push_back(std::move(finding));
  }

  status = reader.field(kTagConflictCount, "cell conflict count");
  if (!status.ok()) {
    return status.error();
  }
  auto conflict_count = reader.u64();
  if (!conflict_count.ok()) {
    return conflict_count.error();
  }
  if (conflict_count.value() > limits.max_conflicts_per_cell) {
    return make_error(ErrorCode::LimitExceeded, "stored conflict count is out of range",
                      "conflict_count");
  }
  for (std::uint64_t i = 0; i < conflict_count.value(); ++i) {
    status = reader.field(kTagConflict, "cell conflict");
    if (!status.ok()) {
      return status.error();
    }
    EvidenceConflict conflict;

    auto conflict_id_text = reader.bytes(64, "conflict id");
    if (!conflict_id_text.ok()) {
      return conflict_id_text.error();
    }
    auto conflict_id = ConflictId::parse(conflict_id_text.value());
    if (!conflict_id.has_value()) {
      return make_error(ErrorCode::Malformed, "stored conflict identity is malformed",
                        "conflict_id");
    }
    conflict.id = *conflict_id;

    auto conflict_scope_text = reader.bytes(kMaxScopeDepth * (kMaxScopeSegmentBytes + 1u),
                                            "conflict scope");
    if (!conflict_scope_text.ok()) {
      return conflict_scope_text.error();
    }
    auto conflict_scope = ScopeIdentity::parse(conflict_scope_text.value());
    if (!conflict_scope.ok()) {
      return conflict_scope.error();
    }
    conflict.scope = conflict_scope.value();

    auto conflict_dimension_text = reader.bytes(64, "conflict dimension");
    if (!conflict_dimension_text.ok()) {
      return conflict_dimension_text.error();
    }
    auto conflict_dimension = DimensionKey::parse(conflict_dimension_text.value());
    if (!conflict_dimension.has_value()) {
      return make_error(ErrorCode::Malformed, "stored conflict dimension is not recognised",
                        "dimension");
    }
    conflict.dimension = *conflict_dimension;

    auto view_text = reader.token(32, "conflict view");
    if (!view_text.ok()) {
      return view_text.error();
    }
    auto view = capacity_view_from_string(view_text.value());
    if (!view.has_value()) {
      return make_error(ErrorCode::Malformed, "stored conflict view is not recognised",
                        "capacity_view");
    }
    conflict.view = *view;

    auto participant_count = reader.u64();
    if (!participant_count.ok()) {
      return participant_count.error();
    }
    if (participant_count.value() > limits.max_evidence_per_cell) {
      return make_error(ErrorCode::LimitExceeded, "stored conflict participant count is out of "
                                                 "range",
                        "participant_count");
    }
    for (std::uint64_t p = 0; p < participant_count.value(); ++p) {
      auto participant_text = reader.bytes(64, "participant id");
      if (!participant_text.ok()) {
        return participant_text.error();
      }
      auto participant = EvidenceId::parse(participant_text.value());
      if (!participant.has_value()) {
        return make_error(ErrorCode::Malformed, "stored participant identity is malformed",
                          "evidence_id");
      }
      auto digest_text = reader.bytes(64, "participant digest");
      if (!digest_text.ok()) {
        return digest_text.error();
      }
      auto digest = Digest::from_hex(digest_text.value());
      if (!digest.has_value()) {
        return make_error(ErrorCode::Malformed, "stored participant digest is malformed", "digest");
      }
      conflict.participants.push_back(*participant);
      conflict.participant_digests.push_back(*digest);
    }

    auto value_count = reader.u64();
    if (!value_count.ok()) {
      return value_count.error();
    }
    if (value_count.value() > limits.max_evidence_per_cell) {
      return make_error(ErrorCode::LimitExceeded, "stored conflict value count is out of range",
                        "value_count");
    }
    for (std::uint64_t v = 0; v < value_count.value(); ++v) {
      auto value = reader.i64();
      if (!value.ok()) {
        return value.error();
      }
      auto source_text = reader.token(48, "conflict asserting source");
      if (!source_text.ok()) {
        return source_text.error();
      }
      auto source = evidence_source_from_string(source_text.value());
      if (!source.has_value()) {
        return make_error(ErrorCode::Malformed, "stored asserting source is not recognised",
                          "evidence_source");
      }
      conflict.values.push_back(value.value());
      conflict.asserting_sources.push_back(*source);
    }

    auto reason = reader.token(48, "conflict reason");
    if (!reason.ok()) {
      return reason.error();
    }
    conflict.unresolvable_reason = reason.value();

    auto has_selected = reader.boolean();
    if (!has_selected.ok()) {
      return has_selected.error();
    }
    if (has_selected.value()) {
      auto selected = reader.i64();
      if (!selected.ok()) {
        return selected.error();
      }
      conflict.selected_value = selected.value();
    }

    cell.conflicts.push_back(std::move(conflict));
  }

  status = reader.field(kTagSiblingUnitCount, "sibling unit count");
  if (!status.ok()) {
    return status.error();
  }
  auto sibling_count = reader.u64();
  if (!sibling_count.ok()) {
    return sibling_count.error();
  }
  if (sibling_count.value() > 16) {
    return make_error(ErrorCode::LimitExceeded, "stored sibling unit count is out of range",
                      "sibling_units");
  }
  for (std::uint64_t i = 0; i < sibling_count.value(); ++i) {
    status = reader.field(kTagSiblingUnit, "sibling unit");
    if (!status.ok()) {
      return status.error();
    }
    auto unit_text = reader.token(32, "sibling unit");
    if (!unit_text.ok()) {
      return unit_text.error();
    }
    auto unit = unit_from_string(unit_text.value());
    if (!unit.has_value()) {
      return make_error(ErrorCode::Malformed, "stored sibling unit is not recognised", "unit");
    }
    cell.sibling_units.push_back(*unit);
  }

  status = reader.field(kTagEvidenceSeen, "cell evidence seen");
  if (!status.ok()) {
    return status.error();
  }
  auto evidence_seen = reader.u64();
  if (!evidence_seen.ok()) {
    return evidence_seen.error();
  }
  cell.evidence_seen = evidence_seen.value();

  status = reader.field(kTagAttributionsApplied, "cell attributions applied");
  if (!status.ok()) {
    return status.error();
  }
  auto attributions_applied = reader.u64();
  if (!attributions_applied.ok()) {
    return attributions_applied.error();
  }
  cell.attributions_applied = attributions_applied.value();

  status = reader.field(kTagRolledUp, "cell rolled-up flag");
  if (!status.ok()) {
    return status.error();
  }
  auto rolled_up = reader.boolean();
  if (!rolled_up.ok()) {
    return rolled_up.error();
  }
  cell.rolled_up = rolled_up.value();

  status = reader.field(kTagRollupContributors, "cell rollup contributors");
  if (!status.ok()) {
    return status.error();
  }
  auto contributors = reader.u32();
  if (!contributors.ok()) {
    return contributors.error();
  }
  cell.rollup_contributors = contributors.value();

  status = reader.field(kTagRollupMissing, "cell rollup missing");
  if (!status.ok()) {
    return status.error();
  }
  auto missing = reader.u32();
  if (!missing.ok()) {
    return missing.error();
  }
  cell.rollup_missing = missing.value();

  auto explanation = decode_explanation(reader, limits.max_explanation_nodes_per_cell,
                                        limits.max_explanation_residuals_per_cell);
  if (!explanation.ok()) {
    return explanation.error();
  }
  cell.explanation = std::move(explanation.value());

  return cell;
}

bool cell_less(const ReconciliationCell& a, const ReconciliationCell& b) noexcept {
  if (!(a.scope == b.scope)) {
    return a.scope < b.scope;
  }
  return a.dimension < b.dimension;
}

bool ReconciliationCell::has_class(DiscrepancyClass value) const noexcept {
  for (const Finding& finding : findings) {
    if (finding.classification == value) {
      return true;
    }
  }
  return false;
}

DiscrepancyClass ReconciliationCell::primary_class() const noexcept {
  if (findings.empty()) {
    return DiscrepancyClass::NoEvidence;
  }
  for (const Finding& finding : findings) {
    if (finding.exceeds_tolerance || !finding.magnitude.has_value()) {
      return finding.classification;
    }
  }
  // Every finding was inside its tolerance band, so the cell agrees. The
  // findings remain, with exceeds_tolerance false, so nothing is hidden.
  return DiscrepancyClass::Agrees;
}

Result<ReconciliationRun> ReconciliationRun::create(ReconciliationRunId id, RunRequest request,
                                                    Digest evidence_digest,
                                                    Digest attribution_digest,
                                                    std::vector<ReconciliationCell> cells) {
  if (!id.valid()) {
    return make_error(ErrorCode::InvalidArgument, "a run requires a generated identity",
                      "run_id");
  }
  if (request.scopes.empty()) {
    return make_error(ErrorCode::InvalidArgument,
                      "a run requires at least one scope selection", "scopes");
  }
  if (request.clock_domain.empty()) {
    return make_error(ErrorCode::InvalidArgument, "a run requires a declared clock domain",
                      "clock_domain");
  }
  ReconciliationRun run;
  run.id_ = id;
  run.request_ = std::move(request);
  run.evidence_digest_ = evidence_digest;
  run.attribution_digest_ = attribution_digest;
  run.policy_digest_ = run.request_.policy.digest();
  run.cells_ = std::move(cells);
  run.resolution_ = run.derive_resolution();
  run.run_digest_ = digest_with_domain(kRunDomain, run.canonical_bytes());
  return run;
}

std::string ReconciliationRun::canonical_bytes() const {
  CanonicalWriter writer;
  writer.field(kTagSchema);
  writer.u32(kCanonicalSchemaVersion);
  encode_request(writer, request_);
  writer.field(kTagEvidenceDigest);
  writer.bytes(evidence_digest_.to_hex());
  writer.field(kTagAttributionDigest);
  writer.bytes(attribution_digest_.to_hex());
  writer.field(kTagCellCount);
  writer.u64(static_cast<std::uint64_t>(cells_.size()));
  for (const ReconciliationCell& cell : cells_) {
    writer.field(kTagCell);
    encode_cell(writer, cell);
  }
  writer.field(kTagEnd);
  return std::move(writer).take();
}

Status ReconciliationRun::reseal() {
  policy_digest_ = request_.policy.digest();
  run_digest_ = digest_with_domain(kRunDomain, canonical_bytes());
  return Status::success();
}

void encode_run_envelope(CanonicalWriter& writer, const ReconciliationRun& run) {
  writer.field(kEnvId);
  writer.bytes(run.id().to_string());
  writer.field(kEnvContent);
  writer.bytes(run.canonical_bytes());
  writer.field(kEnvResolution);
  writer.token(to_string(run.resolution()));
  writer.field(kEnvAttempt);
  writer.u64(run.commit_attempt().value());
  writer.field(kEnvHasCommitted);
  writer.boolean(run.committed_generation().has_value());
  if (run.committed_generation().has_value()) {
    writer.field(kEnvCommitted);
    writer.u64(run.committed_generation()->value());
  }
  writer.field(kEnvHasSupersededBy);
  writer.boolean(run.superseded_by().has_value());
  if (run.superseded_by().has_value()) {
    writer.field(kEnvSupersededBy);
    writer.bytes(run.superseded_by()->to_string());
  }
  writer.field(kEnvEnd);
}

Result<ReconciliationRun> ReconciliationRun::decode_envelope(CanonicalReader& reader,
                                                             const Limits& limits) {
  auto status = reader.field(kEnvId, "run envelope id");
  if (!status.ok()) {
    return status.error();
  }
  auto id_text = reader.bytes(64, "run id");
  if (!id_text.ok()) {
    return id_text.error();
  }
  auto id = ReconciliationRunId::parse(id_text.value());
  if (!id.has_value()) {
    return make_error(ErrorCode::Malformed, "stored run identity is malformed", "run_id");
  }

  status = reader.field(kEnvContent, "run content");
  if (!status.ok()) {
    return status.error();
  }
  auto content = reader.bytes(limits.max_read_bytes, "run content");
  if (!content.ok()) {
    return content.error();
  }

  auto decoded = decode_content(content.value(), limits);
  if (!decoded.ok()) {
    return decoded.error();
  }
  ReconciliationRun run = std::move(decoded.value());
  // The identity is deliberately not part of the content: a run's content
  // digest must not depend on which random identity was issued for it. The
  // envelope is therefore the sole authority for the identity, and it is
  // adopted here.
  run.id_ = *id;

  status = reader.field(kEnvResolution, "run resolution");
  if (!status.ok()) {
    return status.error();
  }
  auto resolution_text = reader.token(32, "run resolution");
  if (!resolution_text.ok()) {
    return resolution_text.error();
  }
  auto resolution = resolution_state_from_string(resolution_text.value());
  if (!resolution.has_value()) {
    return make_error(ErrorCode::Malformed, "stored resolution state is not recognised",
                      "resolution_state");
  }
  run.resolution_ = *resolution;

  status = reader.field(kEnvAttempt, "run attempt");
  if (!status.ok()) {
    return status.error();
  }
  auto attempt = reader.u64();
  if (!attempt.ok()) {
    return attempt.error();
  }
  run.commit_attempt_ = AttemptId{attempt.value()};

  status = reader.field(kEnvHasCommitted, "committed generation presence");
  if (!status.ok()) {
    return status.error();
  }
  auto has_committed = reader.boolean();
  if (!has_committed.ok()) {
    return has_committed.error();
  }
  if (has_committed.value()) {
    status = reader.field(kEnvCommitted, "committed generation");
    if (!status.ok()) {
      return status.error();
    }
    auto committed = reader.u64();
    if (!committed.ok()) {
      return committed.error();
    }
    run.committed_generation_ = Generation{committed.value()};
  }

  status = reader.field(kEnvHasSupersededBy, "supersession presence");
  if (!status.ok()) {
    return status.error();
  }
  auto has_superseded = reader.boolean();
  if (!has_superseded.ok()) {
    return has_superseded.error();
  }
  if (has_superseded.value()) {
    status = reader.field(kEnvSupersededBy, "superseding run id");
    if (!status.ok()) {
      return status.error();
    }
    auto superseded_text = reader.bytes(64, "superseding run id");
    if (!superseded_text.ok()) {
      return superseded_text.error();
    }
    auto superseded = ReconciliationRunId::parse(superseded_text.value());
    if (!superseded.has_value()) {
      return make_error(ErrorCode::Malformed, "stored superseding identity is malformed",
                        "run_id");
    }
    run.superseded_by_ = *superseded;
  }

  status = reader.field(kEnvEnd, "run envelope end marker");
  if (!status.ok()) {
    return status.error();
  }
  return run;
}

Result<ReconciliationRun> ReconciliationRun::decode_content(std::string_view content,
                                                             const Limits& limits) {
  CanonicalReader reader(content);
  auto status = reader.field(kTagSchema, "run schema version");
  if (!status.ok()) {
    return status.error();
  }
  auto schema = reader.u32();
  if (!schema.ok()) {
    return schema.error();
  }
  if (schema.value() != kCanonicalSchemaVersion) {
    return make_error(ErrorCode::IncompatibleVersion,
                      "stored run uses canonical schema version " +
                          std::to_string(schema.value()) + ", this build writes and reads " +
                          std::to_string(kCanonicalSchemaVersion),
                      "schema_version");
  }

  RunRequest request;
  status = reader.field(kTagScopeCount, "scope count");
  if (!status.ok()) {
    return status.error();
  }
  auto scope_count = reader.u64();
  if (!scope_count.ok()) {
    return scope_count.error();
  }
  if (scope_count.value() == 0 || scope_count.value() > limits.max_scope_selections) {
    return make_error(ErrorCode::LimitExceeded, "stored scope count is out of range",
                      "scope_count");
  }
  for (std::uint64_t i = 0; i < scope_count.value(); ++i) {
    status = reader.field(kTagScopeMode, "scope mode");
    if (!status.ok()) {
      return status.error();
    }
    auto mode_text = reader.token(32, "scope mode");
    if (!mode_text.ok()) {
      return mode_text.error();
    }
    auto mode = scope_selection_mode_from_string(mode_text.value());
    if (!mode.has_value()) {
      return make_error(ErrorCode::Malformed, "stored scope mode is not recognised", "scope_mode");
    }
    status = reader.field(kTagScopePath, "scope path");
    if (!status.ok()) {
      return status.error();
    }
    auto path = reader.bytes(kMaxScopeDepth * (kMaxScopeSegmentBytes + 1u), "scope path");
    if (!path.ok()) {
      return path.error();
    }
    auto parsed = ScopeIdentity::parse(path.value());
    if (!parsed.ok()) {
      return parsed.error();
    }
    if (parsed.value().to_string() != path.value()) {
      return make_error(ErrorCode::Malformed, "stored scope path is not canonical", "scope");
    }
    ScopeSelection selection;
    selection.scope = std::move(parsed.value());
    selection.mode = *mode;
    request.scopes.push_back(std::move(selection));
  }

  status = reader.field(kTagGeneration, "run generation");
  if (!status.ok()) {
    return status.error();
  }
  auto generation = reader.u64();
  if (!generation.ok()) {
    return generation.error();
  }
  request.generation = Generation{generation.value()};

  status = reader.field(kTagEvaluationInstant, "evaluation instant");
  if (!status.ok()) {
    return status.error();
  }
  auto instant = reader.u64();
  if (!instant.ok()) {
    return instant.error();
  }
  request.evaluation_instant = Tick{instant.value()};

  status = reader.field(kTagClockDomain, "clock domain");
  if (!status.ok()) {
    return status.error();
  }
  auto domain = reader.token(kMaxClockDomainBytes, "clock domain");
  if (!domain.ok()) {
    return domain.error();
  }
  auto parsed_domain = ClockDomain::create(domain.value());
  if (!parsed_domain.ok()) {
    return parsed_domain.error();
  }
  request.clock_domain = parsed_domain.value();

  status = reader.field(kTagPolicyDigest, "policy digest");
  if (!status.ok()) {
    return status.error();
  }
  auto policy_digest_text = reader.bytes(64, "policy digest");
  if (!policy_digest_text.ok()) {
    return policy_digest_text.error();
  }
  auto policy_digest = Digest::from_hex(policy_digest_text.value());
  if (!policy_digest.has_value()) {
    return make_error(ErrorCode::Malformed, "stored policy digest is malformed", "digest");
  }

  status = reader.field(kTagMinEvidenceGeneration, "minimum evidence generation");
  if (!status.ok()) {
    return status.error();
  }
  auto min_generation = reader.u64();
  if (!min_generation.ok()) {
    return min_generation.error();
  }
  request.min_evidence_generation = Generation{min_generation.value()};

  status = reader.field(kTagHasParent, "parent presence");
  if (!status.ok()) {
    return status.error();
  }
  auto has_parent = reader.boolean();
  if (!has_parent.ok()) {
    return has_parent.error();
  }
  if (has_parent.value()) {
    status = reader.field(kTagParent, "parent run id");
    if (!status.ok()) {
      return status.error();
    }
    auto parent_text = reader.bytes(64, "parent run id");
    if (!parent_text.ok()) {
      return parent_text.error();
    }
    auto parent = ReconciliationRunId::parse(parent_text.value());
    if (!parent.has_value()) {
      return make_error(ErrorCode::Malformed, "stored parent identity is malformed", "run_id");
    }
    request.parent_run = *parent;
  }

  status = reader.field(kTagFromRecovered, "from-recovered flag");
  if (!status.ok()) {
    return status.error();
  }
  auto recovered = reader.boolean();
  if (!recovered.ok()) {
    return recovered.error();
  }
  request.from_recovered_state = recovered.value();

  status = reader.field(kTagLabel, "run label");
  if (!status.ok()) {
    return status.error();
  }
  auto label = reader.token(64, "run label");
  if (!label.ok()) {
    return label.error();
  }
  request.label = label.value();

  status = reader.field(kTagEvidenceDigest, "evidence digest");
  if (!status.ok()) {
    return status.error();
  }
  auto evidence_digest_text = reader.bytes(64, "evidence digest");
  if (!evidence_digest_text.ok()) {
    return evidence_digest_text.error();
  }
  auto evidence_digest = Digest::from_hex(evidence_digest_text.value());
  if (!evidence_digest.has_value()) {
    return make_error(ErrorCode::Malformed, "stored evidence digest is malformed", "digest");
  }

  status = reader.field(kTagAttributionDigest, "attribution digest");
  if (!status.ok()) {
    return status.error();
  }
  auto attribution_digest_text = reader.bytes(64, "attribution digest");
  if (!attribution_digest_text.ok()) {
    return attribution_digest_text.error();
  }
  auto attribution_digest = Digest::from_hex(attribution_digest_text.value());
  if (!attribution_digest.has_value()) {
    return make_error(ErrorCode::Malformed, "stored attribution digest is malformed", "digest");
  }

  status = reader.field(kTagCellCount, "cell count");
  if (!status.ok()) {
    return status.error();
  }
  auto cell_count = reader.u64();
  if (!cell_count.ok()) {
    return cell_count.error();
  }
  if (cell_count.value() > limits.max_cells_per_run) {
    return make_error(ErrorCode::LimitExceeded,
                      "stored run declares " + std::to_string(cell_count.value()) +
                          " cells, above the limit of " + std::to_string(limits.max_cells_per_run),
                      "cell_count");
  }

  std::vector<ReconciliationCell> cells;
  cells.reserve(static_cast<std::size_t>(cell_count.value()));
  for (std::uint64_t i = 0; i < cell_count.value(); ++i) {
    status = reader.field(kTagCell, "cell");
    if (!status.ok()) {
      return status.error();
    }
    auto cell = decode_cell(reader, limits);
    if (!cell.ok()) {
      return cell.error();
    }
    if (!cells.empty() && !cell_less(cells.back(), cell.value())) {
      return make_error(ErrorCode::Malformed,
                        "stored run cells are not in canonical order or contain a duplicate",
                        "cell_order");
    }
    cells.push_back(std::move(cell.value()));
  }

  status = reader.field(kTagEnd, "run end marker");
  if (!status.ok()) {
    return status.error();
  }
  status = reader.exhausted("run content");
  if (!status.ok()) {
    return status.error();
  }

  // The identity lives in the envelope, not in the content: a run's content
  // digest must be independent of which random identity happened to be issued
  // for it. A placeholder is generated here and replaced by decode_envelope.
  auto run = ReconciliationRun::create(ReconciliationRunId::generate(), std::move(request),
                                       *evidence_digest, *attribution_digest, std::move(cells));
  if (!run.ok()) {
    return run.error();
  }
  // The policy *object* is not persisted with the run: only its digest is, and
  // the digest is part of the content. The decoded run therefore carries a
  // recorded policy digest that is authoritative for comparison even though the
  // full policy is not reconstructed here. A caller who needs the policy must
  // supply it and check its digest against this one.
  ReconciliationRun decoded_run = std::move(run.value());
  decoded_run.policy_digest_ = *policy_digest;
  return decoded_run;
}

std::size_t ReconciliationRun::count_class(DiscrepancyClass value) const noexcept {
  std::size_t count = 0;
  for (const ReconciliationCell& cell : cells_) {
    if (cell.has_class(value)) {
      ++count;
    }
  }
  return count;
}

std::size_t ReconciliationRun::unexplained_cell_count() const noexcept {
  std::size_t count = 0;
  for (const ReconciliationCell& cell : cells_) {
    if (cell.explanation.has_unexplained()) {
      ++count;
    }
  }
  return count;
}

std::size_t ReconciliationRun::conflict_count() const noexcept {
  std::size_t count = 0;
  for (const ReconciliationCell& cell : cells_) {
    count += cell.conflicts.size();
  }
  return count;
}

ResolutionState ReconciliationRun::derive_resolution() const noexcept {
  // Precedence, tested explicitly by the suite:
  //   Conflicted > Stale > Incomplete > PartiallyExplained > Explained
  if (cells_.empty()) {
    return ResolutionState::Explained;
  }
  for (const ReconciliationCell& cell : cells_) {
    if (cell.has_class(DiscrepancyClass::EvidenceConflict) ||
        cell.has_class(DiscrepancyClass::ArithmeticOverflow)) {
      return ResolutionState::Conflicted;
    }
  }
  for (const ReconciliationCell& cell : cells_) {
    if (cell.has_class(DiscrepancyClass::Stale) ||
        cell.has_class(DiscrepancyClass::GenerationRegression)) {
      return ResolutionState::Stale;
    }
  }
  for (const ReconciliationCell& cell : cells_) {
    if (cell.has_class(DiscrepancyClass::Incomplete) ||
        cell.has_class(DiscrepancyClass::NoEvidence)) {
      return ResolutionState::Incomplete;
    }
  }
  for (const ReconciliationCell& cell : cells_) {
    if (cell.explanation.has_unexplained()) {
      return ResolutionState::PartiallyExplained;
    }
  }
  return ResolutionState::Explained;
}

Status ReconciliationRun::validate(std::size_t limits_max_cells) const {
  if (cells_.size() > limits_max_cells) {
    return make_error(ErrorCode::LimitExceeded, "run holds more cells than the limit permits",
                      "cell_count");
  }
  for (std::size_t i = 0; i < cells_.size(); ++i) {
    if (i > 0 && !cell_less(cells_[i - 1], cells_[i])) {
      return make_error(ErrorCode::InvariantViolation,
                        "run cells are not strictly ordered by (scope, dimension)",
                        "cell_order");
    }
    const ReconciliationCell& cell = cells_[i];
    if (cell.scope.empty()) {
      return make_error(ErrorCode::InvariantViolation, "run contains a cell with an empty scope",
                        "scope");
    }
    if (cell.findings.size() > 64) {
      return make_error(ErrorCode::LimitExceeded, "cell holds more findings than the limit permits",
                        "finding_count");
    }
    for (const EvidenceConflict& conflict : cell.conflicts) {
      if (conflict.participants.size() != conflict.participant_digests.size()) {
        return make_error(ErrorCode::InvariantViolation,
                          "conflict participant and digest lists differ in length",
                          "conflict_participants");
      }
      if (conflict.values.size() != conflict.asserting_sources.size()) {
        return make_error(ErrorCode::InvariantViolation,
                          "conflict value and source lists differ in length", "conflict_values");
      }
      if (conflict.unresolvable_reason.empty()) {
        return make_error(ErrorCode::InvariantViolation,
                          "conflict was recorded without a reason token", "conflict_reason");
      }
    }
    for (const ResidualExplanation& explanation : cell.explanation.residuals()) {
      if (explanation.unexplained_node != 0 &&
          static_cast<std::size_t>(explanation.unexplained_node) >= cell.explanation.size()) {
        return make_error(ErrorCode::InvariantViolation,
                          "residual explanation points outside its graph", "unexplained_node");
      }
    }
  }
  return Status::success();
}

}  // namespace summon::capacity_reconciliation
