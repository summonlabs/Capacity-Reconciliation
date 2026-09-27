// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Evidence and attribution: construction validation, canonical round trips,
// tamper detection, canonical ordering and insertion-order determinism.

#include <algorithm>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "summon/capacity_reconciliation/attribution.hpp"
#include "summon/capacity_reconciliation/evidence.hpp"
#include "test_helpers.hpp"
#include "test_support.hpp"

namespace cr = summon::capacity_reconciliation;

namespace {

cr::EvidenceStamp stamp_for(std::uint64_t generation, std::uint64_t revision) {
  cr::EvidenceStamp stamp;
  stamp.generation = cr::Generation{generation};
  stamp.revision = cr::Revision{revision};
  stamp.epoch = cr::Epoch{3};
  stamp.incarnation = cr::IncarnationId::generate();
  stamp.observed_at = cr::Tick{100};
  stamp.valid_until = cr::Tick{900};
  stamp.clock_domain = crtest::test_domain();
  return stamp;
}

cr::EvidenceItem item(const std::string& scope, cr::CapacityView view, cr::Amount value,
                      cr::EvidenceSource source, const std::string& instance,
                      std::uint64_t generation = 0, std::uint64_t revision = 1) {
  return cr::EvidenceItem::create(cr::EvidenceId::generate(), source, instance,
                                  cr::ScopeIdentity::parse(scope).value(), crtest::power_key(),
                                  view, cr::Quantity::known(value), cr::EvidenceStatus::Accepted,
                                  stamp_for(generation, revision))
      .value();
}

}  // namespace

CR_TEST(evidence, construction_validates_every_field) {
  const auto scope = cr::ScopeIdentity::parse("site-a/hall-1").value();

  CR_REQUIRE_ERR(cr::EvidenceItem::create(cr::EvidenceId{}, cr::EvidenceSource::FacilityCapacity,
                                          "i", scope, crtest::power_key(),
                                          cr::CapacityView::Planned, cr::Quantity::known(1),
                                          cr::EvidenceStatus::Accepted, stamp_for(0, 1)),
                 cr::ErrorCode::InvalidArgument);

  CR_REQUIRE_ERR(cr::EvidenceItem::create(cr::EvidenceId::generate(), cr::EvidenceSource::Unknown,
                                          "i", scope, crtest::power_key(),
                                          cr::CapacityView::Planned, cr::Quantity::known(1),
                                          cr::EvidenceStatus::Accepted, stamp_for(0, 1)),
                 cr::ErrorCode::InvalidArgument);

  CR_REQUIRE_ERR(cr::EvidenceItem::create(cr::EvidenceId::generate(),
                                          cr::EvidenceSource::FacilityCapacity, "Bad Instance",
                                          scope, crtest::power_key(), cr::CapacityView::Planned,
                                          cr::Quantity::known(1), cr::EvidenceStatus::Accepted,
                                          stamp_for(0, 1)),
                 cr::ErrorCode::InvalidArgument);

  CR_REQUIRE_ERR(cr::EvidenceItem::create(cr::EvidenceId::generate(),
                                          cr::EvidenceSource::FacilityCapacity, "i",
                                          cr::ScopeIdentity{}, crtest::power_key(),
                                          cr::CapacityView::Planned, cr::Quantity::known(1),
                                          cr::EvidenceStatus::Accepted, stamp_for(0, 1)),
                 cr::ErrorCode::InvalidArgument);

  auto no_domain = stamp_for(0, 1);
  no_domain.clock_domain = cr::ClockDomain{};
  CR_REQUIRE_ERR(cr::EvidenceItem::create(cr::EvidenceId::generate(),
                                          cr::EvidenceSource::FacilityCapacity, "i", scope,
                                          crtest::power_key(), cr::CapacityView::Planned,
                                          cr::Quantity::known(1), cr::EvidenceStatus::Accepted,
                                          no_domain),
                 cr::ErrorCode::InvalidArgument);

  auto inverted = stamp_for(0, 1);
  inverted.observed_at = cr::Tick{500};
  inverted.valid_until = cr::Tick{100};
  CR_REQUIRE_ERR(cr::EvidenceItem::create(cr::EvidenceId::generate(),
                                          cr::EvidenceSource::FacilityCapacity, "i", scope,
                                          crtest::power_key(), cr::CapacityView::Planned,
                                          cr::Quantity::known(1), cr::EvidenceStatus::Accepted,
                                          inverted),
                 cr::ErrorCode::InvalidArgument);

  auto invalid_unit = cr::DimensionKey::create(cr::CapacityDimension::Power, cr::Unit::MilliWatt);
  CR_REQUIRE_OK(invalid_unit);
}

CR_TEST(evidence, canonical_round_trip_and_tamper_detection) {
  const cr::EvidenceItem original = item("site-a/hall-1", cr::CapacityView::Observed, 1234,
                                         cr::EvidenceSource::ObservationStream, "meter-1", 7, 5);
  const std::string bytes = original.canonical_bytes();

  auto decoded = cr::EvidenceItem::decode(bytes);
  CR_REQUIRE_OK(decoded);
  CR_CHECK(decoded.value().content_digest() == original.content_digest());
  CR_CHECK(decoded.value().id() == original.id());
  CR_CHECK_EQ(decoded.value().value().value(), cr::Amount(1234));
  CR_CHECK(decoded.value().scope() == original.scope());
  CR_CHECK(decoded.value().dimension() == original.dimension());

  // Every single-byte mutation of the stored bytes must be detected, either as
  // a decode failure or as a digest mismatch. None may decode to a different
  // item that still claims the original digest.
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    for (int bit = 0; bit < 8; ++bit) {
      std::string mutated = bytes;
      mutated[i] = static_cast<char>(mutated[i] ^ (1 << bit));
      auto attempt = cr::EvidenceItem::decode(mutated);
      if (attempt.ok()) {
        CR_CHECK(attempt.value().content_digest() == original.content_digest());
      }
    }
  }
}

CR_TEST(evidence, canonical_round_trip_covers_unknown_and_every_view) {
  for (std::uint8_t v = 0; v < cr::kCapacityViewCount; ++v) {
    const auto view = static_cast<cr::CapacityView>(v);
    // UnknownReason::None means "not unknown", so it is not a storable reason.
    for (std::uint8_t r = 1; r < 10; ++r) {
      const auto reason = static_cast<cr::UnknownReason>(r);
      auto built = crtest::EvidenceBuilder("site-a/hall-1", cr::CapacityDimension::Power,
                                           cr::Unit::MilliWatt)
                       .source(cr::EvidenceSource::PowerCapacity)
                       .unknown(view, reason);
      CR_REQUIRE_OK(built);
      auto decoded = cr::EvidenceItem::decode(built.value().canonical_bytes());
      CR_REQUIRE_OK(decoded);
      CR_CHECK(decoded.value().value().is_unknown());
      CR_CHECK_EQ(decoded.value().value().reason(), reason);
      CR_CHECK_EQ(decoded.value().view(), view);
      CR_CHECK(decoded.value().content_digest() == built.value().content_digest());
    }
  }
}

CR_TEST(evidence, every_status_round_trips) {
  for (std::uint8_t s = 0; s < 6; ++s) {
    const auto status = static_cast<cr::EvidenceStatus>(s);
    auto built = crtest::EvidenceBuilder("site-a/hall-1", cr::CapacityDimension::Power,
                                         cr::Unit::MilliWatt)
                     .status(status)
                     .known(cr::CapacityView::Installed, 10);
    CR_REQUIRE_OK(built);
    auto decoded = cr::EvidenceItem::decode(built.value().canonical_bytes());
    CR_REQUIRE_OK(decoded);
    CR_CHECK_EQ(decoded.value().status(), status);
  }
}

CR_TEST(evidence, set_digest_is_independent_of_insertion_order) {
  std::vector<cr::EvidenceItem> items;
  for (int i = 0; i < 40; ++i) {
    items.push_back(item("site-a/hall-1/row-" + std::to_string(i % 5),
                         static_cast<cr::CapacityView>(i % 6), i * 10,
                         static_cast<cr::EvidenceSource>((i % 11) + 1),
                         "instance-" + std::to_string(i % 3), 1, 1));
  }

  auto reference = cr::EvidenceSet::build(items, 1000);
  CR_REQUIRE_OK(reference);

  std::mt19937_64 engine(0xC0FFEEull);
  for (int trial = 0; trial < 60; ++trial) {
    std::vector<cr::EvidenceItem> shuffled = items;
    std::shuffle(shuffled.begin(), shuffled.end(), engine);
    auto rebuilt = cr::EvidenceSet::build(std::move(shuffled), 1000);
    CR_REQUIRE_OK(rebuilt);
    CR_CHECK(rebuilt.value().digest() == reference.value().digest());
    CR_CHECK(rebuilt.value().size() == reference.value().size());
    for (std::size_t i = 0; i < rebuilt.value().size(); ++i) {
      CR_CHECK(rebuilt.value().items()[i].id() == reference.value().items()[i].id());
    }
  }
}

CR_TEST(evidence, duplicate_identity_is_refused) {
  const cr::EvidenceItem first = item("site-a/hall-1", cr::CapacityView::Installed, 10,
                                      cr::EvidenceSource::RackRegistry, "r", 1, 1);
  std::vector<cr::EvidenceItem> items{first, first};
  CR_REQUIRE_ERR(cr::EvidenceSet::build(std::move(items), 10), cr::ErrorCode::AlreadyExists);
}

CR_TEST(evidence, set_bound_is_enforced_before_accepting) {
  std::vector<cr::EvidenceItem> items;
  for (int i = 0; i < 5; ++i) {
    items.push_back(item("site-a/hall-1", cr::CapacityView::Installed, i,
                         cr::EvidenceSource::RackRegistry, "r" + std::to_string(i), 1, 1));
  }
  CR_REQUIRE_ERR(cr::EvidenceSet::build(std::move(items), 4), cr::ErrorCode::LimitExceeded);
}

CR_TEST(evidence, ordering_is_total_and_content_determined) {
  const cr::EvidenceItem a = item("site-a/hall-1", cr::CapacityView::Installed, 10,
                                  cr::EvidenceSource::RackRegistry, "r", 1, 1);
  const cr::EvidenceItem b = item("site-a/hall-1", cr::CapacityView::Installed, 10,
                                  cr::EvidenceSource::RackRegistry, "r", 1, 2);
  const cr::EvidenceItem c = item("site-a/hall-1", cr::CapacityView::Observed, 10,
                                  cr::EvidenceSource::ObservationStream, "o", 1, 1);
  // Same source and instance: the newer revision sorts first, so a canonical
  // scan meets the authoritative statement before the superseded one.
  CR_CHECK(cr::evidence_less(b, a));
  CR_CHECK(!cr::evidence_less(a, b));
  CR_CHECK(cr::evidence_less(a, c));
  CR_CHECK(!cr::evidence_less(c, a));
}

CR_TEST(evidence, supersession_within_a_source_instance) {
  // A producer's own newer revision replaces its older one. This is not a
  // disagreement and must not be reported as one.
  std::vector<cr::EvidenceItem> items{
      item("site-a/hall-1", cr::CapacityView::Installed, 100,
           cr::EvidenceSource::RackRegistry, "rack-registry-1", 1, 1),
      item("site-a/hall-1", cr::CapacityView::Installed, 120,
           cr::EvidenceSource::RackRegistry, "rack-registry-1", 1, 2),
  };
  auto set = cr::EvidenceSet::build(std::move(items), 10);
  CR_REQUIRE_OK(set);
  CR_CHECK_EQ(set.value().max_revision("rack-registry-1").value(), std::uint64_t(2));
  CR_CHECK_EQ(set.value().max_generation().value(), std::uint64_t(1));
}

CR_TEST(attribution, construction_and_round_trip) {
  auto built = crtest::make_attribution("site-a/hall-1", cr::CapacityDimension::Power,
                                        cr::Unit::MilliWatt, cr::ResidualKind::ObservedGap, -500,
                                        cr::EvidenceSource::CoolingCapacity, "cooling-1", 2, 4,
                                        100, 900, cr::AttributionReason::EquipmentFailure);
  CR_REQUIRE_OK(built);
  CR_CHECK_EQ(built.value().amount(), cr::Amount(-500));
  CR_CHECK_EQ(built.value().target(), cr::ResidualKind::ObservedGap);

  auto decoded = cr::AttributionItem::decode(built.value().canonical_bytes());
  CR_REQUIRE_OK(decoded);
  CR_CHECK(decoded.value().content_digest() == built.value().content_digest());
  CR_CHECK_EQ(decoded.value().reason(), cr::AttributionReason::EquipmentFailure);
  CR_CHECK_EQ(decoded.value().reason_token(), std::string("equipment_failure"));

  // Negative amounts are legal: an attribution may explain an excess.
  const std::string bytes = built.value().canonical_bytes();
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    std::string mutated = bytes;
    mutated[i] = static_cast<char>(mutated[i] ^ 0x01);
    auto attempt = cr::AttributionItem::decode(mutated);
    if (attempt.ok()) {
      CR_CHECK(attempt.value().content_digest() == built.value().content_digest());
    }
  }
}

CR_TEST(attribution, target_may_not_be_none) {
  CR_REQUIRE_ERR(crtest::make_attribution("site-a/hall-1", cr::CapacityDimension::Power,
                                          cr::Unit::MilliWatt, cr::ResidualKind::None, 1,
                                          cr::EvidenceSource::CoolingCapacity, "c", 1, 1, 1, 2),
                 cr::ErrorCode::InvalidArgument);
}

CR_TEST(attribution, set_digest_is_order_independent) {
  std::vector<cr::AttributionItem> items;
  for (int i = 0; i < 20; ++i) {
    auto built = crtest::make_attribution(
        "site-a/hall-1/row-" + std::to_string(i % 4), cr::CapacityDimension::Power,
        cr::Unit::MilliWatt, static_cast<cr::ResidualKind>((i % 6) + 1), i * 7,
        static_cast<cr::EvidenceSource>((i % 5) + 1), "instance-" + std::to_string(i % 2), 1, 1,
        50, 900);
    CR_REQUIRE_OK(built);
    items.push_back(std::move(built.value()));
  }
  auto reference = cr::AttributionSet::build(items, 100);
  CR_REQUIRE_OK(reference);

  std::mt19937_64 engine(4242);
  for (int trial = 0; trial < 40; ++trial) {
    std::vector<cr::AttributionItem> shuffled = items;
    std::shuffle(shuffled.begin(), shuffled.end(), engine);
    auto rebuilt = cr::AttributionSet::build(std::move(shuffled), 100);
    CR_REQUIRE_OK(rebuilt);
    CR_CHECK(rebuilt.value().digest() == reference.value().digest());
  }
}
