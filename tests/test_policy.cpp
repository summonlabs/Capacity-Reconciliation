// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Policy: precedence, tolerance arithmetic, required views, digesting and the
// refusal to be configured to hide a discrepancy.

#include <algorithm>
#include <string>
#include <vector>

#include "summon/capacity_reconciliation/policy.hpp"
#include "test_helpers.hpp"
#include "test_support.hpp"

namespace cr = summon::capacity_reconciliation;

CR_TEST(policy, default_construction_is_the_standard_policy) {
  const cr::ReconciliationPolicy policy;
  CR_CHECK(!policy.precedence().empty());
  CR_CHECK_EQ(policy.precedence().size(), std::size_t(12));
  CR_CHECK_EQ(std::string(cr::to_string(policy.precedence().front())),
              std::string("observation_stream"));
  CR_CHECK_EQ(std::string(cr::to_string(policy.precedence().back())), std::string("unknown"));
  CR_CHECK(policy.digest() == cr::ReconciliationPolicy::standard().digest());
  CR_CHECK(!policy.digest().is_zero());
}

CR_TEST(policy, precedence_ranks_measurement_above_declaration) {
  const cr::ReconciliationPolicy policy;
  CR_CHECK(policy.precedence_of(cr::EvidenceSource::ObservationStream) <
           policy.precedence_of(cr::EvidenceSource::AssetRegistry));
  CR_CHECK(policy.precedence_of(cr::EvidenceSource::AssetRegistry) <
           policy.precedence_of(cr::EvidenceSource::RackCapacity));
  CR_CHECK(policy.precedence_of(cr::EvidenceSource::RackCapacity) <
           policy.precedence_of(cr::EvidenceSource::OperatorDeclaration));
  CR_CHECK(policy.precedence_of(cr::EvidenceSource::OperatorDeclaration) <
           policy.precedence_of(cr::EvidenceSource::Unknown));
}

CR_TEST(policy, precedence_override_validates_the_permutation) {
  cr::ReconciliationPolicy policy;
  std::vector<cr::EvidenceSource> order = policy.precedence();
  std::reverse(order.begin(), order.end() - 1);  // keep Unknown last
  CR_REQUIRE_OK(policy.set_precedence(order));
  CR_CHECK_EQ(policy.precedence_of(cr::EvidenceSource::OperatorDeclaration), std::size_t(0));

  const cr::ReconciliationPolicy reference;
  CR_CHECK(!(policy.digest() == reference.digest()));

  std::vector<cr::EvidenceSource> too_short = order;
  too_short.pop_back();
  CR_REQUIRE_ERR(policy.set_precedence(too_short), cr::ErrorCode::InvalidArgument);

  std::vector<cr::EvidenceSource> duplicated = order;
  duplicated[0] = duplicated[1];
  CR_REQUIRE_ERR(policy.set_precedence(duplicated), cr::ErrorCode::AlreadyExists);

  std::vector<cr::EvidenceSource> with_unknown = order;
  with_unknown[0] = cr::EvidenceSource::Unknown;
  CR_REQUIRE_ERR(policy.set_precedence(with_unknown), cr::ErrorCode::InvalidArgument);

  // Unknown must stay last: it is the "no producer" sentinel and must never
  // outrank a real source family.
  std::vector<cr::EvidenceSource> unknown_first = order;
  std::swap(unknown_first[0], unknown_first.back());
  CR_REQUIRE_ERR(policy.set_precedence(unknown_first), cr::ErrorCode::InvalidArgument);
}

CR_TEST(policy, tolerance_bound_is_exact_integer_arithmetic) {
  cr::ToleranceBand band;
  band.absolute = 10;
  band.parts_per_million = 0;
  CR_CHECK_EQ(cr::policy_tolerance_bound(band, 1000).value(), cr::Amount(10));

  band.parts_per_million = 50000;  // 5%
  // 1000 * 50000 / 1000000 = 50, plus 10.
  CR_CHECK_EQ(cr::policy_tolerance_bound(band, 1000).value(), cr::Amount(60));
  // Negative references use the magnitude.
  CR_CHECK_EQ(cr::policy_tolerance_bound(band, -1000).value(), cr::Amount(60));
  // A negative absolute tolerance is treated as zero rather than widening it.
  band.absolute = -1000;
  CR_CHECK_EQ(cr::policy_tolerance_bound(band, 1000).value(), cr::Amount(50));

  // Large references must not overflow: 9.2e18 * 999999 ppm is about 9.2e15.
  band.absolute = 0;
  band.parts_per_million = 999999;
  auto large = cr::policy_tolerance_bound(band, cr::amount_max());
  CR_REQUIRE_OK(large);
  CR_CHECK(large.value() > 0);

  // The minimum amount has no representable magnitude and is refused.
  CR_REQUIRE_ERR(cr::policy_tolerance_bound(band, cr::amount_min()), cr::ErrorCode::Overflow);
}

CR_TEST(policy, tolerances_are_per_dimension) {
  cr::ReconciliationPolicy policy;
  cr::ToleranceBand band;
  band.absolute = 25;
  policy.set_tolerance(crtest::power_key(), band);
  CR_CHECK_EQ(policy.tolerance_for(crtest::power_key()).absolute, cr::Amount(25));
  CR_CHECK_EQ(policy.tolerance_for(crtest::space_key()).absolute, cr::Amount(0));

  band.absolute = 40;
  policy.set_tolerance(crtest::power_key(), band);
  CR_CHECK_EQ(policy.tolerance_for(crtest::power_key()).absolute, cr::Amount(40));
  CR_CHECK_EQ(policy.dimensions().size(), std::size_t(1));
}

CR_TEST(policy, required_views_default_and_override) {
  cr::ReconciliationPolicy policy;
  const auto& defaults = policy.required_views_for(crtest::power_key());
  CR_CHECK_EQ(defaults.size(), std::size_t(2));
  CR_CHECK_EQ(defaults[0], cr::CapacityView::Installed);
  CR_CHECK_EQ(defaults[1], cr::CapacityView::Observed);

  policy.set_required_views(crtest::power_key(),
                            {cr::CapacityView::Usable, cr::CapacityView::Installed,
                             cr::CapacityView::Usable});
  const auto& custom = policy.required_views_for(crtest::power_key());
  CR_CHECK_EQ(custom.size(), std::size_t(2));
  CR_CHECK_EQ(custom[0], cr::CapacityView::Installed);
  CR_CHECK_EQ(custom[1], cr::CapacityView::Usable);
  CR_CHECK_EQ(policy.required_views_for(crtest::space_key()).size(), std::size_t(2));
}

CR_TEST(policy, every_setting_changes_the_digest) {
  const cr::ReconciliationPolicy base;
  const std::string base_bytes = base.canonical_bytes();

  cr::ReconciliationPolicy a = base;
  a.max_generation_lag = 5;
  CR_CHECK(a.digest() != base.digest());
  CR_CHECK(a.canonical_bytes() != base_bytes);

  cr::ReconciliationPolicy b = base;
  b.stale_evidence = cr::StaleEvidencePolicy::UseButMarkStale;
  CR_CHECK(b.digest() != base.digest());

  cr::ReconciliationPolicy c = base;
  c.missing_view = cr::MissingViewPolicy::ProceedWithPresent;
  CR_CHECK(c.digest() != base.digest());

  cr::ReconciliationPolicy d = base;
  d.rollup = cr::RollupPolicy::PartialSumReported;
  CR_CHECK(d.digest() != base.digest());

  cr::ReconciliationPolicy e = base;
  cr::ToleranceBand band;
  band.absolute = 1;
  e.set_tolerance(crtest::power_key(), band);
  CR_CHECK(e.digest() != base.digest());

  // Identical settings produce identical bytes and identical digests.
  cr::ReconciliationPolicy f = base;
  CR_CHECK_EQ(f.canonical_bytes(), base_bytes);
  CR_CHECK(f.digest() == base.digest());
}

CR_TEST(policy, policy_tokens_round_trip) {
  CR_CHECK_EQ(std::string(cr::to_string(cr::StaleEvidencePolicy::Reject)), std::string("reject"));
  CR_CHECK(cr::stale_evidence_policy_from_string("use_but_mark_stale").has_value());
  CR_CHECK(!cr::stale_evidence_policy_from_string("nonsense").has_value());
  CR_CHECK(cr::missing_view_policy_from_string("proceed_with_present").has_value());
  CR_CHECK(!cr::missing_view_policy_from_string("maybe").has_value());
  CR_CHECK(cr::rollup_policy_from_string("partial_sum_reported").has_value());
  CR_CHECK(!cr::rollup_policy_from_string("lax").has_value());
}
