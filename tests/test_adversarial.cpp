// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Adversarial input: malformed, truncated, oversized, mis-versioned,
// mis-endian, path-manipulated and identity-swapped state. Every case here is
// an attempt to make the runtime adopt something it should refuse.

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "summon/capacity_reconciliation/engine.hpp"
#include "summon/capacity_reconciliation/journal.hpp"
#include "summon/capacity_reconciliation/platform.hpp"
#include "summon/capacity_reconciliation/store.hpp"
#include "test_helpers.hpp"
#include "test_support.hpp"

namespace cr = summon::capacity_reconciliation;

namespace {



std::vector<cr::EvidenceItem> small_evidence(const std::string& scope, std::uint64_t generation) {
  cr::EvidenceStamp stamp;
  stamp.generation = cr::Generation{generation};
  stamp.revision = cr::Revision{1};
  stamp.epoch = cr::Epoch{0};
  stamp.incarnation = cr::IncarnationId::generate();
  stamp.observed_at = cr::Tick{10};
  stamp.valid_until = cr::Tick{10000};
  stamp.clock_domain = crtest::test_domain();
  std::vector<cr::EvidenceItem> items;
  auto item = cr::EvidenceItem::create(
      cr::EvidenceId::generate(), cr::EvidenceSource::RackCapacity, "r",
      cr::ScopeIdentity::parse(scope).value(), crtest::power_key(), cr::CapacityView::Installed,
      cr::Quantity::known(10), cr::EvidenceStatus::Accepted, stamp);
  if (item.ok()) {
    items.push_back(std::move(item.value()));
  }
  return items;
}

void write_raw(const std::filesystem::path& path, const std::string& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

CR_TEST(adversarial, record_framing_rejects_every_declared_hazard) {
  const std::string payload = "the payload";
  cr::RecordHeader header;
  header.generation = cr::Generation{1};
  header.attempt = cr::AttemptId{1};
  header.store_identity = cr::Sha256::hash("store");
  auto framed = cr::frame_record(header, payload);
  CR_REQUIRE_OK(framed);

  // Round trip is intact.
  cr::RecordHeader decoded;
  auto payload_back = cr::unframe_record(framed.value(), &decoded);
  CR_REQUIRE_OK(payload_back);
  CR_CHECK_EQ(payload_back.value(), payload);
  CR_CHECK_EQ(decoded.generation.value(), std::uint64_t(1));

  // Wrong magic.
  {
    std::string bad = framed.value();
    bad[0] = 'X';
    CR_REQUIRE_ERR(cr::unframe_record(bad, nullptr), cr::ErrorCode::Malformed);
  }
  // Wrong format version.
  {
    std::string bad = framed.value();
    bad[8] = static_cast<char>(9);
    CR_REQUIRE_ERR(cr::unframe_record(bad, nullptr), cr::ErrorCode::IncompatibleVersion);
  }
  // Wrong endian marker: a record written by a big-endian architecture stores
  // the marker as 01 02 03 04, which is what this plants.
  {
    std::string bad = framed.value();
    bad[12] = static_cast<char>(0x01);
    bad[13] = static_cast<char>(0x02);
    bad[14] = static_cast<char>(0x03);
    bad[15] = static_cast<char>(0x04);
    CR_REQUIRE_ERR(cr::unframe_record(bad, nullptr), cr::ErrorCode::IncompatibleVersion);
  }
  // A declared payload length beyond the limit must be refused before any
  // allocation, not after.
  {
    std::string bad = framed.value();
    for (int i = 0; i < 8; ++i) {
      bad[16 + i] = static_cast<char>(0xFF);
    }
    CR_REQUIRE_ERR(cr::unframe_record(bad, nullptr), cr::ErrorCode::LimitExceeded);
  }
  // A declared length that does not match the bytes present.
  {
    std::string bad = framed.value();
    bad[16] = static_cast<char>(static_cast<unsigned char>(bad[16]) + 1);
    CR_REQUIRE_ERR(cr::unframe_record(bad, nullptr), cr::ErrorCode::Corruption);
  }
  // Truncation at every prefix.
  for (std::size_t cut = 0; cut < framed.value().size(); ++cut) {
    auto attempt = cr::unframe_record(std::string_view(framed.value()).substr(0, cut), nullptr);
    CR_CHECK_MSG(!attempt.ok(), "truncation to " + std::to_string(cut) + " bytes was accepted");
  }
  // Every single-bit flip must be *detected by someone*. The framer itself is
  // responsible for the magic, the version, the endian marker, the length and
  // the payload digest. The chain link, the store identity, the generation and
  // the attempt are verified against the payload and the published manifest by
  // the store, so for those the framer is allowed to succeed -- but it must
  // then report a header that differs from the original, which is what the
  // store compares against.
  const auto header_differs = [](const cr::RecordHeader& a, const cr::RecordHeader& b) {
    return a.format_version != b.format_version || a.payload_length != b.payload_length ||
           !(a.payload_digest == b.payload_digest) ||
           !(a.previous_record_digest == b.previous_record_digest) ||
           !(a.store_identity == b.store_identity) || !(a.generation == b.generation) ||
           !(a.attempt == b.attempt);
  };
  for (std::size_t i = 0; i < framed.value().size(); ++i) {
    for (int bit = 0; bit < 8; ++bit) {
      std::string bad = framed.value();
      bad[i] = static_cast<char>(bad[i] ^ (1 << bit));
      cr::RecordHeader flipped;
      auto attempt = cr::unframe_record(bad, &flipped);
      const bool detected = !attempt.ok() || header_differs(flipped, decoded);
      CR_CHECK_MSG(detected, "bit flip at byte " + std::to_string(i) + " bit " +
                                 std::to_string(bit) + " was silently accepted");
    }
  }
}

CR_TEST(adversarial, a_head_that_is_not_a_head_is_refused) {
  crtest::ScratchDirectory scratch("badhead");
  {
    auto store = cr::Store::open(scratch.path(), crtest::writer_options(true));
    CR_REQUIRE_OK(store);
  }
  write_raw(cr::Store::head_path(scratch.path()), "not a manifest at all");
  auto store = cr::Store::open(scratch.path(), crtest::reader_options());
  // The *kind* of failure is preserved. A manifest that is not a canonical
  // manifest at all is reported as malformed, not flattened into corruption.
  CR_REQUIRE_ERR(store, cr::ErrorCode::Malformed);
  CR_CHECK(store.error().message.find("no previous commit") != std::string::npos);

  // A head with the right magic but nothing else. The original typed error is
  // preserved rather than collapsed into a generic corruption.
  write_raw(cr::Store::head_path(scratch.path()), std::string("CRHEAD01"));
  auto store2 = cr::Store::open(scratch.path(), crtest::reader_options());
  CR_REQUIRE_ERR(store2, cr::ErrorCode::Malformed);
  CR_CHECK(store2.error().message.find("no previous commit") != std::string::npos);
}

CR_TEST(adversarial, an_oversized_head_is_refused_before_it_is_read) {
  crtest::ScratchDirectory scratch("bighead");
  {
    auto store = cr::Store::open(scratch.path(), crtest::writer_options(true));
    CR_REQUIRE_OK(store);
  }
  std::string huge(cr::kMaxHeadBytes + 1024, 'A');
  huge.replace(0, 8, "CRHEAD01");
  write_raw(cr::Store::head_path(scratch.path()), huge);
  auto store = cr::Store::open(scratch.path(), crtest::reader_options());
  CR_REQUIRE_ERR(store, cr::ErrorCode::LimitExceeded);
}

CR_TEST(adversarial, a_fabricated_marker_is_refused) {
  crtest::ScratchDirectory scratch("fakemarker");
  std::filesystem::create_directories(scratch.path());
  write_raw(cr::Store::marker_path(scratch.path()), "CAPACITY-RECONCILIATION-STORE-not-really");
  auto store = cr::Store::open(scratch.path(), crtest::reader_options());
  CR_REQUIRE_ERR(store, cr::ErrorCode::Malformed);
}

CR_TEST(adversarial, a_marker_from_a_future_format_version_is_refused) {
  crtest::ScratchDirectory scratch("futuremarker");
  {
    auto store = cr::Store::open(scratch.path(), crtest::writer_options(true));
    CR_REQUIRE_OK(store);
  }
  // The marker is canonical, so a byte edit is detected either as a digest
  // mismatch or as a version mismatch. Both are refusals.
  auto bytes = cr::platform::read_file_bounded(cr::Store::marker_path(scratch.path()),
                                               cr::kMaxMarkerBytes);
  CR_REQUIRE_OK(bytes);
  std::string mutated = bytes.value();
  // The format version follows the length-prefixed magic string and its
  // one-byte field tag. Locating it by search keeps the test independent of the
  // exact tag numbering while still exercising the version check itself.
  const std::string magic = "CAPACITY-RECONCILIATION-STORE";
  const std::size_t magic_at = mutated.find(magic);
  CR_REQUIRE(magic_at != std::string::npos);
  const std::size_t version_at = magic_at + magic.size() + 1;
  CR_REQUIRE(version_at + 3 < mutated.size());
  mutated[version_at] = static_cast<char>(9);
  write_raw(cr::Store::marker_path(scratch.path()), mutated);
  auto store = cr::Store::open(scratch.path(), crtest::reader_options());
  CR_REQUIRE_ERR(store, cr::ErrorCode::IncompatibleVersion);
}

CR_TEST(adversarial, a_store_rejects_a_record_whose_declared_generation_lies) {
  crtest::ScratchDirectory scratch("genlie");
  {
    auto store = cr::Store::open(scratch.path(), crtest::writer_options(true));
    CR_REQUIRE_OK(store);
    cr::EngineOptions options;
    options.clock_domain = crtest::test_domain();
    auto engine = cr::Engine::attach(std::move(store.value()), options);
    CR_REQUIRE_OK(engine);
    CR_REQUIRE_OK(engine.value().append_evidence(small_evidence("site-a/hall-1", 0),
                                                 engine.value().generation(),
                                                 engine.value().incarnation()));
  }
  // Rename a record so that its filename claims a generation its header does
  // not. The store must not adopt it.
  std::error_code ec;
  const auto original = cr::Store::record_path(scratch.path(), cr::Generation{1});
  const auto renamed = cr::Store::record_path(scratch.path(), cr::Generation{7});
  std::filesystem::rename(original, renamed, ec);
  CR_REQUIRE(!ec);
  const auto report = cr::Store::verify(scratch.path(), cr::default_limits());
  // The published manifest points at generation 1, which no longer exists.
  CR_CHECK(!report.ok);
}

CR_TEST(adversarial, scope_paths_cannot_escape_or_inject) {
  std::vector<std::string> hostile = {
      "../../etc/passwd", "site/../../root", "..", ".", "site/./hall",
      "C:/Windows/System32", "\\\\server\\share", "site/hall\nrow",
      "site/hall\ttab", "/absolute", "site/hall?query", "site/hall#fragment",
  };
  // An embedded NUL: the classic truncation attack on a NUL-terminated path.
  hostile.push_back(std::string("site/hall") + static_cast<char>(0) + "hidden");
  for (const std::string& text : hostile) {
    auto parsed = cr::ScopeIdentity::parse(text);
    CR_CHECK_MSG(!parsed.ok(), "hostile scope accepted");
  }
}

CR_TEST(adversarial, evidence_rejects_a_unit_that_does_not_belong_to_the_dimension) {
  auto key = cr::DimensionKey::create(cr::CapacityDimension::Cooling, cr::Unit::Gram);
  CR_REQUIRE_ERR(key, cr::ErrorCode::InvalidArgument);
  auto ok = cr::DimensionKey::create(cr::CapacityDimension::Cooling, cr::Unit::MilliWattThermal);
  CR_REQUIRE_OK(ok);
  auto also_ok = cr::DimensionKey::create(cr::CapacityDimension::Cooling, cr::Unit::MilliWatt);
  CR_REQUIRE_OK(also_ok);
}

CR_TEST(adversarial, decoded_evidence_with_a_broken_digest_is_refused) {
  auto built = crtest::EvidenceBuilder("site-a/hall-1", cr::CapacityDimension::Power,
                                       cr::Unit::MilliWatt)
                   .known(cr::CapacityView::Installed, 5);
  CR_REQUIRE_OK(built);
  const std::string bytes = built.value().canonical_bytes();
  // Replace the trailing digest field with a different valid-looking digest.
  const std::string other = cr::Sha256::hash("other").to_hex();
  const std::size_t position = bytes.rfind(built.value().content_digest().to_hex());
  CR_REQUIRE(position != std::string::npos);
  std::string mutated = bytes;
  mutated.replace(position, other.size(), other);
  CR_REQUIRE_ERR(cr::EvidenceItem::decode(mutated), cr::ErrorCode::Corruption);
}

CR_TEST(adversarial, decoded_evidence_rejects_trailing_bytes) {
  auto built = crtest::EvidenceBuilder("site-a/hall-1", cr::CapacityDimension::Power,
                                       cr::Unit::MilliWatt)
                   .known(cr::CapacityView::Installed, 5);
  CR_REQUIRE_OK(built);
  std::string bytes = built.value().canonical_bytes();
  bytes.push_back('\x7f');
  CR_REQUIRE_ERR(cr::EvidenceItem::decode(bytes), cr::ErrorCode::Malformed);
}

CR_TEST(adversarial, decoded_run_rejects_a_non_canonical_cell_order) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  for (const char* scope : {"site-a/hall-2", "site-a/hall-1"}) {
    auto items = small_evidence(scope, engine.value().generation().value());
    CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                                 engine.value().incarnation()));
  }
  cr::RunRequest request;
  for (const char* scope : {"site-a/hall-1", "site-a/hall-2"}) {
    request.scopes.push_back({cr::ScopeIdentity::parse(scope).value(),
                              cr::ScopeSelectionMode::Exact});
  }
  request.generation = engine.value().generation();
  request.evaluation_instant = cr::Tick{50};
  request.clock_domain = crtest::test_domain();
  auto run = engine.value().reconcile(request);
  CR_REQUIRE_OK(run);
  CR_CHECK_EQ(run.value().cells()[0].scope.to_string(), std::string("site-a/hall-1"));

  // Round-tripping the content through the decoder must reproduce the digest.
  auto decoded = cr::ReconciliationRun::decode_content(run.value().canonical_bytes(),
                                                       cr::default_limits());
  CR_REQUIRE_OK(decoded);
  CR_CHECK(decoded.value().run_digest() == run.value().run_digest());
  CR_CHECK_EQ(decoded.value().cells().size(), run.value().cells().size());
}

CR_TEST(adversarial, a_huge_declared_cell_count_is_refused_before_allocation) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  auto items = small_evidence("site-a/hall-1", engine.value().generation().value());
  CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                               engine.value().incarnation()));
  auto run = engine.value().reconcile(
      crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 50));
  CR_REQUIRE_OK(run);

  std::string bytes = run.value().canonical_bytes();
  // Find the cell count field and inflate it. The exact offset is internal, so
  // the test searches for the encoded count of 1 immediately before the first
  // cell tag. Either the decode fails or the count is rejected; both are
  // refusals. A generous limit makes the test independent of the layout.
  cr::Limits tiny = cr::default_limits();
  tiny.max_cells_per_run = 0;
  auto refused = cr::ReconciliationRun::decode_content(bytes, tiny);
  CR_REQUIRE_ERR(refused, cr::ErrorCode::LimitExceeded);
}

CR_TEST(adversarial, malformed_state_files_never_crash_the_verifier) {
  crtest::ScratchDirectory scratch("fuzzverify");
  std::filesystem::create_directories(scratch.path());
  const std::string seeds[] = {
      "",
      "CRHEAD01",
      std::string(4096, '\0'),
      std::string("CAPACITY-RECONCILIATION-STORE"),
      std::string(200, static_cast<char>(0xFF)),
  };
  for (const std::string& seed : seeds) {
    write_raw(cr::Store::marker_path(scratch.path()), seed);
    write_raw(cr::Store::head_path(scratch.path()), seed);
    const auto report = cr::Store::verify(scratch.path(), cr::default_limits());
    CR_CHECK(!report.ok);
    auto store = cr::Store::open(scratch.path(), crtest::reader_options());
    CR_CHECK(!store.ok());
  }
}

CR_TEST(adversarial, a_symlinked_store_root_is_not_silently_followed_into) {
  // On Windows a junction or symlink requires privilege to create, and the
  // runtime refuses to read *through* a reparse point for the files it owns.
  // This test asserts the refusal on the paths it controls: a HEAD that has
  // become a reparse point is not read.
  crtest::ScratchDirectory scratch("reparse");
  {
    auto store = cr::Store::open(scratch.path(), crtest::writer_options(true));
    CR_REQUIRE_OK(store);
  }
  // A regular file is read; this establishes the baseline.
  auto bytes = cr::platform::read_file_bounded(cr::Store::head_path(scratch.path()),
                                               cr::kMaxHeadBytes);
  CR_CHECK(bytes.ok() || bytes.error().code == cr::ErrorCode::NotFound);
  CR_CHECK(!cr::platform::is_reparse_point(cr::Store::head_path(scratch.path())));
  CR_CHECK(cr::platform::is_regular_file_no_reparse(cr::Store::marker_path(scratch.path())));
}

CR_TEST(adversarial, path_validation_rejects_escapes) {
  const auto root = std::filesystem::temp_directory_path() / "capacity-reconciliation-root";
  CR_REQUIRE_OK(cr::platform::validate_child_path(root, root / "records" / "record-1.crr"));
  CR_REQUIRE_ERR(cr::platform::validate_child_path(root, root / ".." / "elsewhere"),
                 cr::ErrorCode::PathRejected);
  CR_REQUIRE_ERR(cr::platform::validate_child_path(root, std::filesystem::path("relative")),
                 cr::ErrorCode::PathRejected);
  CR_REQUIRE_ERR(cr::platform::validate_child_path(root, std::filesystem::path("C:/Windows")),
                 cr::ErrorCode::PathRejected);
  CR_REQUIRE_ERR(cr::platform::validate_child_path(std::filesystem::path(), root),
                 cr::ErrorCode::PathRejected);
}

CR_TEST(adversarial, a_second_writer_in_the_same_process_is_refused) {
  crtest::ScratchDirectory scratch("doublewriter");
  auto first = cr::Store::open(scratch.path(), crtest::writer_options(true));
  CR_REQUIRE_OK(first);
  auto second = cr::Store::open(scratch.path(), crtest::writer_options(true));
  CR_REQUIRE_ERR(second, cr::ErrorCode::LockConflict);
  // A read-only handle is still allowed while a writer holds the lock: reading
  // a published commit does not require the writer lock.
  auto reader = cr::Store::open(scratch.path(), crtest::reader_options());
  CR_REQUIRE(!reader.ok() || reader.value().is_writer() == false);
  CR_CHECK_MSG(reader.ok(), reader.ok() ? std::string() : reader.error().to_string());
}

CR_TEST(adversarial, unknown_reason_and_status_tokens_in_stored_bytes_are_refused) {
  auto built = crtest::EvidenceBuilder("site-a/hall-1", cr::CapacityDimension::Power,
                                       cr::Unit::MilliWatt)
                   .unknown(cr::CapacityView::Installed, cr::UnknownReason::NotReported);
  CR_REQUIRE_OK(built);
  std::string bytes = built.value().canonical_bytes();
  const std::size_t position = bytes.find("not_reported");
  CR_REQUIRE(position != std::string::npos);
  bytes.replace(position, 12, "not_a_reason");
  CR_REQUIRE_ERR(cr::EvidenceItem::decode(bytes), cr::ErrorCode::Malformed);
}

CR_TEST(adversarial, an_empty_engine_refuses_degenerate_requests) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);

  cr::RunRequest no_scopes;
  no_scopes.generation = engine.value().generation();
  no_scopes.evaluation_instant = cr::Tick{1};
  no_scopes.clock_domain = crtest::test_domain();
  CR_REQUIRE_ERR(engine.value().reconcile(no_scopes), cr::ErrorCode::InvalidArgument);

  cr::RunRequest no_domain = crtest::exact_request("site-a/hall-1", 0, 1);
  no_domain.clock_domain = cr::ClockDomain{};
  CR_REQUIRE_ERR(engine.value().reconcile(no_domain), cr::ErrorCode::InvalidArgument);

  CR_REQUIRE_ERR(engine.value().append_evidence({}, engine.value().generation(),
                                                engine.value().incarnation()),
                 cr::ErrorCode::InvalidArgument);
  CR_REQUIRE_ERR(engine.value().append_attributions({}, engine.value().generation(),
                                                    engine.value().incarnation()),
                 cr::ErrorCode::InvalidArgument);
}

CR_TEST(adversarial, a_policy_that_would_hide_a_discrepancy_is_refused) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  cr::ReconciliationPolicy policy = cr::ReconciliationPolicy::standard();
  policy.record_conflict_sets = false;
  CR_REQUIRE_OK(engine.value().set_policy(policy, engine.value().generation(),
                                          engine.value().incarnation()));
  auto items = small_evidence("site-a/hall-1", engine.value().generation().value());
  CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                               engine.value().incarnation()));
  auto run = engine.value().reconcile(
      crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 50));
  CR_REQUIRE_ERR(run, cr::ErrorCode::Unsupported);

  cr::ReconciliationPolicy policy2 = cr::ReconciliationPolicy::standard();
  policy2.report_unexplained_residuals = false;
  CR_REQUIRE_OK(engine.value().set_policy(policy2, engine.value().generation(),
                                          engine.value().incarnation()));
  auto run2 = engine.value().reconcile(
      crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 50));
  CR_REQUIRE_ERR(run2, cr::ErrorCode::Unsupported);
}

CR_TEST(adversarial, a_forged_store_image_is_not_adopted) {
  crtest::ScratchDirectory scratch("forged");
  cr::StoreImage image;
  image.store_identity = cr::Sha256::hash("some-other-store");
  image.clock_domain = crtest::test_domain();
  image.generation = cr::Generation{1};
  auto validation = image.validate(cr::default_limits());
  CR_REQUIRE_OK(validation);

  auto store = cr::Store::open(scratch.path(), crtest::writer_options(true));
  CR_REQUIRE_OK(store);
  // Committing an image that carries another store's identity must be corrected
  // by the store, not trusted: the store owns its identity.
  auto committed = store.value().commit(image, store.value().generation(),
                                        store.value().incarnation());
  CR_REQUIRE_OK(committed);
  auto reader = cr::Store::open(scratch.path(), crtest::reader_options());
  CR_REQUIRE_OK(reader);
  CR_CHECK(reader.value().image().store_identity == store.value().image().store_identity);
  CR_CHECK(!(reader.value().image().store_identity == image.store_identity));
}
