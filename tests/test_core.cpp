// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Core value types: digests, identities, units, quantities, checked arithmetic,
// canonical encoding and scope identity.

#include <string>
#include <vector>

#include "summon/capacity_reconciliation/canonical.hpp"
#include "summon/capacity_reconciliation/hash.hpp"
#include "summon/capacity_reconciliation/identity.hpp"
#include "summon/capacity_reconciliation/journal.hpp"
#include "summon/capacity_reconciliation/quantity.hpp"
#include "summon/capacity_reconciliation/scope.hpp"
#include "summon/capacity_reconciliation/status.hpp"
#include "summon/capacity_reconciliation/units.hpp"
#include "summon/capacity_reconciliation/value.hpp"
#include "summon/capacity_reconciliation/version.hpp"
#include "test_support.hpp"

namespace cr = summon::capacity_reconciliation;

namespace {

std::string hash_hex(const std::string& text) {
  return cr::Sha256::hash(text).to_hex();
}

}  // namespace

CR_TEST(core, sha256_matches_fips_vectors) {
  // FIPS 180-4 / NIST published vectors. The digest is used for content
  // addressing and for chaining committed records, so "close enough" would not
  // be good enough.
  CR_CHECK_EQ(hash_hex(""),
              std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  CR_CHECK_EQ(hash_hex("abc"),
              std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  CR_CHECK_EQ(
      hash_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
      std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  CR_CHECK_EQ(
      hash_hex("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
               "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"),
      std::string("cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"));
}

CR_TEST(core, sha256_streaming_equals_oneshot) {
  std::string payload;
  for (int i = 0; i < 1000; ++i) {
    payload.push_back(static_cast<char>('a' + (i % 26)));
  }
  cr::Sha256 streaming;
  for (std::size_t offset = 0; offset < payload.size(); offset += 7) {
    const std::size_t count = (payload.size() - offset) < 7 ? (payload.size() - offset) : 7;
    streaming.update(payload.data() + offset, count);
  }
  CR_CHECK(streaming.finish() == cr::Sha256::hash(payload));
}

CR_TEST(core, digest_hex_is_strict) {
  const cr::Digest digest = cr::Sha256::hash("abc");
  const std::string hex = digest.to_hex();
  CR_CHECK_EQ(hex.size(), std::size_t(64));
  auto parsed = cr::Digest::from_hex(hex);
  CR_REQUIRE(parsed.has_value());
  CR_CHECK(*parsed == digest);

  // Uppercase hex is refused: two spellings of one digest is exactly the kind
  // of ambiguity a canonical format must not permit.
  std::string uppercase = hex;
  for (char& c : uppercase) {
    if (c >= 'a' && c <= 'f') {
      c = static_cast<char>(c - 'a' + 'A');
    }
  }
  CR_CHECK(!cr::Digest::from_hex(uppercase).has_value());
  CR_CHECK(!cr::Digest::from_hex(hex.substr(0, 63)).has_value());
  CR_CHECK(!cr::Digest::from_hex(hex + "0").has_value());
  std::string non_hex = hex;
  non_hex[10] = 'z';
  CR_CHECK(!cr::Digest::from_hex(non_hex).has_value());
  CR_CHECK(cr::Digest{}.is_zero());
  CR_CHECK(!digest.is_zero());
}

CR_TEST(core, identities_are_distinct_types_and_round_trip) {
  const auto run = cr::ReconciliationRunId::generate();
  const auto evidence = cr::EvidenceId::generate();
  CR_CHECK(run.valid());
  CR_CHECK(evidence.valid());
  CR_CHECK(run != cr::ReconciliationRunId{});

  auto parsed = cr::ReconciliationRunId::parse(run.to_string());
  CR_REQUIRE(parsed.has_value());
  CR_CHECK(*parsed == run);

  // A prefix from another family must not parse.
  CR_CHECK(!cr::ReconciliationRunId::parse(evidence.to_string()).has_value());
  CR_CHECK(!cr::EvidenceId::parse(run.to_string()).has_value());
  CR_CHECK(!cr::ReconciliationRunId::parse("crr_").has_value());
  CR_CHECK(!cr::ReconciliationRunId::parse(run.to_string() + "0").has_value());
  CR_CHECK(!cr::ReconciliationRunId::parse(
                "crr_000000000000000000000000000000000")
                .has_value());

  // 2000 generated identities must all be distinct.
  std::vector<cr::EvidenceId> ids;
  for (int i = 0; i < 2000; ++i) {
    ids.push_back(cr::EvidenceId::generate());
  }
  for (std::size_t i = 1; i < ids.size(); ++i) {
    CR_CHECK(ids[i] != ids[i - 1]);
  }
}

CR_TEST(core, generation_and_attempt_refuse_to_wrap) {
  auto next = cr::Generation{41}.next();
  CR_REQUIRE_OK(next);
  CR_CHECK_EQ(next.value().value(), std::uint64_t(42));

  const cr::Generation maximum{std::numeric_limits<std::uint64_t>::max()};
  CR_REQUIRE_ERR(maximum.next(), cr::ErrorCode::Overflow);

  const cr::AttemptId max_attempt{std::numeric_limits<std::uint64_t>::max()};
  CR_REQUIRE_ERR(max_attempt.next(), cr::ErrorCode::Overflow);
}

CR_TEST(core, token_validation_rejects_path_significant_input) {
  CR_CHECK(cr::is_valid_token("power_capacity", 64));
  CR_CHECK(cr::is_valid_token("a.b-c:d", 64));
  CR_CHECK(!cr::is_valid_token("", 64));
  CR_CHECK(!cr::is_valid_token("UPPER", 64));
  CR_CHECK(!cr::is_valid_token("..", 64));
  CR_CHECK(!cr::is_valid_token(".hidden", 64));
  CR_CHECK(!cr::is_valid_token("-leading", 64));
  CR_CHECK(!cr::is_valid_token("has space", 64));
  CR_CHECK(!cr::is_valid_token("has/slash", 64));
  CR_CHECK(!cr::is_valid_token("has\\backslash", 64));
  CR_CHECK(!cr::is_valid_token("c:", 8) == false);  // 'c:' is a legal token
  CR_CHECK(!cr::is_valid_token(std::string(65, 'a'), 64));

  auto token = cr::make_token("ok_token", 64, "field");
  CR_REQUIRE_OK(token);
  CR_CHECK_EQ(token.value(), std::string("ok_token"));
  CR_REQUIRE_ERR(cr::make_token("Bad", 64, "field"), cr::ErrorCode::InvalidArgument);
  CR_REQUIRE_ERR(cr::make_token(std::string(65, 'a'), 64, "field"), cr::ErrorCode::LimitExceeded);
}

CR_TEST(core, dimension_keys_bind_dimension_to_unit) {
  auto power = cr::DimensionKey::create(cr::CapacityDimension::Power, cr::Unit::MilliWatt);
  CR_REQUIRE_OK(power);
  CR_CHECK_EQ(power.value().to_string(), std::string("power:milliwatt"));
  auto parsed = cr::DimensionKey::parse("power:milliwatt");
  CR_REQUIRE(parsed.has_value());
  CR_CHECK(*parsed == power.value());

  // A unit that does not belong to the dimension is refused rather than
  // coerced, so a byte count can never be added to a watt figure.
  CR_REQUIRE_ERR(cr::DimensionKey::create(cr::CapacityDimension::Power, cr::Unit::Byte),
                 cr::ErrorCode::InvalidArgument);
  CR_REQUIRE_ERR(cr::DimensionKey::create(cr::CapacityDimension::Power, cr::Unit::None),
                 cr::ErrorCode::InvalidArgument);
  CR_REQUIRE_ERR(cr::DimensionKey::create(cr::CapacityDimension::None, cr::Unit::MilliWatt),
                 cr::ErrorCode::InvalidArgument);
  CR_CHECK(!cr::DimensionKey::parse("power").has_value());
  CR_CHECK(!cr::DimensionKey::parse("power:byte").has_value());
  CR_CHECK(!cr::DimensionKey::parse("nonsense:milliwatt").has_value());

  const auto space_ru = cr::DimensionKey::create(cr::CapacityDimension::Space, cr::Unit::RackUnit);
  const auto space_mm2 =
      cr::DimensionKey::create(cr::CapacityDimension::Space, cr::Unit::SquareMilliMetre);
  CR_REQUIRE_OK(space_ru);
  CR_REQUIRE_OK(space_mm2);
  CR_CHECK(space_ru.value() != space_mm2.value());
  CR_CHECK(cr::same_dimension_different_unit(space_ru.value(), space_mm2.value()));
  CR_CHECK(!cr::same_dimension_different_unit(space_ru.value(), power.value()));
}

CR_TEST(core, quantity_separates_zero_from_unknown) {
  const cr::Quantity zero = cr::Quantity::known(0);
  const cr::Quantity missing = cr::Quantity::not_reported();
  CR_CHECK(zero.is_known());
  CR_CHECK(missing.is_unknown());
  CR_CHECK(zero != missing);
  CR_CHECK_EQ(zero.value(), cr::Amount(0));
  CR_CHECK_EQ(missing.reason(), cr::UnknownReason::NotReported);
  CR_CHECK_EQ(missing.to_string(), std::string("unknown:not_reported"));

  auto sum_with_zero = cr::add_quantities(zero, cr::Quantity::known(5),
                                          cr::UnknownReason::NotReported);
  CR_REQUIRE_OK(sum_with_zero);
  CR_CHECK_EQ(sum_with_zero.value().value(), cr::Amount(5));

  // Adding an unknown to a value must not silently treat the unknown as zero.
  auto sum_with_unknown =
      cr::add_quantities(cr::Quantity::known(5), missing, cr::UnknownReason::NotReported);
  CR_REQUIRE_OK(sum_with_unknown);
  CR_CHECK(sum_with_unknown.value().is_unknown());

  auto difference = cr::sub_quantities(cr::Quantity::known(5), missing,
                                       cr::UnknownReason::NotReported);
  CR_REQUIRE_OK(difference);
  CR_CHECK(difference.value().is_unknown());

  // Reason precedence: the first non-None reason wins.
  auto combined = cr::add_quantities(cr::Quantity::unknown(cr::UnknownReason::Conflicted), missing,
                                     cr::UnknownReason::NotReported);
  CR_REQUIRE_OK(combined);
  CR_CHECK_EQ(combined.value().reason(), cr::UnknownReason::Conflicted);
}

CR_TEST(core, checked_arithmetic_refuses_to_wrap) {
  CR_REQUIRE_OK(cr::checked_add(1, 2));
  CR_CHECK_EQ(cr::checked_add(1, 2).value(), cr::Amount(3));
  CR_REQUIRE_ERR(cr::checked_add(cr::amount_max(), 1), cr::ErrorCode::Overflow);
  CR_REQUIRE_ERR(cr::checked_add(cr::amount_min(), -1), cr::ErrorCode::Overflow);
  CR_REQUIRE_OK(cr::checked_add(cr::amount_max(), -1));

  CR_REQUIRE_ERR(cr::checked_sub(cr::amount_min(), 1), cr::ErrorCode::Overflow);
  CR_REQUIRE_ERR(cr::checked_sub(cr::amount_max(), -1), cr::ErrorCode::Overflow);
  // amount_min() - (-1) is representable and must succeed.
  CR_REQUIRE_OK(cr::checked_sub(cr::amount_min(), -1));
  CR_CHECK_EQ(cr::checked_sub(0, cr::amount_max()).value(), -cr::amount_max());

  CR_REQUIRE_ERR(cr::checked_neg(cr::amount_min()), cr::ErrorCode::Overflow);
  CR_CHECK_EQ(cr::checked_neg(5).value(), cr::Amount(-5));

  CR_REQUIRE_ERR(cr::checked_mul(cr::amount_max(), 2), cr::ErrorCode::Overflow);
  CR_REQUIRE_ERR(cr::checked_mul(cr::amount_min(), -1), cr::ErrorCode::Overflow);
  CR_REQUIRE_ERR(cr::checked_mul(cr::amount_min(), 2), cr::ErrorCode::Overflow);
  CR_CHECK_EQ(cr::checked_mul(-3, 4).value(), cr::Amount(-12));
  CR_CHECK_EQ(cr::checked_mul(0, cr::amount_min()).value(), cr::Amount(0));
  CR_CHECK_EQ(cr::checked_mul(cr::amount_min(), 1).value(), cr::amount_min());
  CR_CHECK_EQ(cr::checked_mul(1, cr::amount_min()).value(), cr::amount_min());
  // 3037000499^2 fits in a signed 64-bit value; 3037000500^2 does not.
  CR_CHECK(cr::checked_mul(3037000499LL, 3037000499LL).ok());
  CR_CHECK(!cr::checked_mul(3037000500LL, 3037000500LL).ok());

  CR_REQUIRE_ERR(cr::checked_magnitude(cr::amount_min()), cr::ErrorCode::Overflow);
  CR_CHECK_EQ(cr::checked_magnitude(-7).value(), std::uint64_t(7));

  CR_REQUIRE_ERR(cr::checked_increment(std::numeric_limits<std::uint64_t>::max()),
                 cr::ErrorCode::Overflow);
  CR_REQUIRE_ERR(cr::checked_mul_u64(std::numeric_limits<std::uint64_t>::max(), 2),
                 cr::ErrorCode::Overflow);
}

CR_TEST(core, error_codes_have_stable_tokens) {
  CR_CHECK_EQ(std::string(cr::to_string(cr::ErrorCode::StaleGeneration)),
              std::string("stale_generation"));
  CR_CHECK_EQ(std::string(cr::to_string(cr::ErrorCode::LockConflict)),
              std::string("lock_conflict"));
  for (std::uint16_t i = 0; i <= 19; ++i) {
    const auto code = static_cast<cr::ErrorCode>(i);
    const char* token = cr::to_string(code);
    CR_CHECK(std::string(token) != "unrecognised");
    auto parsed = cr::error_code_from_string(token);
    CR_REQUIRE(parsed.has_value());
    CR_CHECK_EQ(static_cast<std::uint16_t>(*parsed), i);
  }
  CR_CHECK(!cr::error_code_from_string("not_a_code").has_value());
}

CR_TEST(core, canonical_encoding_is_little_endian_and_length_prefixed) {
  cr::CanonicalWriter writer;
  writer.u32(0x01020304u);
  writer.u64(0x0102030405060708ull);
  writer.bytes("ab");
  const std::string& bytes = writer.buffer();
  CR_REQUIRE(bytes.size() >= 4 + 8 + 8 + 2);
  CR_CHECK_EQ(static_cast<unsigned char>(bytes[0]), static_cast<unsigned char>(0x04));
  CR_CHECK_EQ(static_cast<unsigned char>(bytes[3]), static_cast<unsigned char>(0x01));
  CR_CHECK_EQ(static_cast<unsigned char>(bytes[4]), static_cast<unsigned char>(0x08));
  CR_CHECK_EQ(static_cast<unsigned char>(bytes[11]), static_cast<unsigned char>(0x01));

  cr::CanonicalReader reader(bytes);
  CR_CHECK_EQ(reader.u32().value(), std::uint32_t(0x01020304u));
  CR_CHECK_EQ(reader.u64().value(), std::uint64_t(0x0102030405060708ull));
  CR_CHECK_EQ(reader.bytes(16, "test").value(), std::string("ab"));
  CR_REQUIRE_OK(reader.exhausted("test"));
}

CR_TEST(core, canonical_reader_refuses_truncation_and_oversize) {
  cr::CanonicalWriter writer;
  writer.bytes("abcdef");
  const std::string bytes = writer.buffer();

  // Truncating anywhere must be detected, never defaulted.
  for (std::size_t cut = 0; cut < bytes.size(); ++cut) {
    cr::CanonicalReader reader(std::string_view(bytes).substr(0, cut));
    auto read = reader.bytes(64, "test");
    CR_CHECK(!read.ok());
  }

  // A declared length beyond the limit is refused before any allocation.
  cr::CanonicalReader oversized(bytes);
  auto refused = oversized.bytes(2, "test");
  CR_REQUIRE_ERR(refused, cr::ErrorCode::LimitExceeded);
}

CR_TEST(core, scope_identity_is_strictly_canonical) {
  auto scope = cr::ScopeIdentity::parse("site-a/hall-1/row-3");
  CR_REQUIRE_OK(scope);
  CR_CHECK_EQ(scope.value().to_string(), std::string("site-a/hall-1/row-3"));
  CR_CHECK_EQ(scope.value().depth(), std::size_t(3));

  const char* rejected[] = {
      "",           "/site",       "site/",         "site//hall",  "site/../hall",
      "site/./hall", "Site/hall",  "site\\hall",    "site/hall/",  "c:/site",
      "site/hall\xC3\xA9", "site/ hall", "site/hall?",
  };
  for (const char* text : rejected) {
    auto parsed = cr::ScopeIdentity::parse(text);
    CR_CHECK_MSG(!parsed.ok(), std::string("should have been rejected: '") + text + "'");
  }
  CR_CHECK(!cr::ScopeIdentity::parse("site/-hall").ok());
  CR_CHECK(!cr::ScopeIdentity::parse(std::string("site/") + std::string(65, 'a')).ok());

  const std::string too_deep = "a/b/c/d/e/f/g/h/i";
  CR_CHECK(!cr::ScopeIdentity::parse(too_deep).ok());

  const auto hall = cr::ScopeIdentity::parse("site-a/hall-1").value();
  const auto row = cr::ScopeIdentity::parse("site-a/hall-1/row-3").value();
  const auto other = cr::ScopeIdentity::parse("site-b/hall-1").value();
  CR_CHECK(hall.is_ancestor_of(row));
  CR_CHECK(hall.is_ancestor_of(hall));
  CR_CHECK(!row.is_ancestor_of(hall));
  CR_CHECK(!hall.is_ancestor_of(other));
  CR_REQUIRE(row.parent().has_value());
  CR_CHECK(*row.parent() == hall);
  CR_CHECK(!hall.parent()->parent().has_value());
}

CR_TEST(core, version_and_limits_are_declared) {
  CR_CHECK_EQ(std::string(cr::version_string()), std::string("1.0.0"));
  CR_CHECK_EQ(cr::kVersionMajor, 1);
  CR_CHECK(cr::default_limits().max_evidence_items > 0);
  CR_CHECK_EQ(cr::kRecordHeaderBytes, std::size_t(136));
}
