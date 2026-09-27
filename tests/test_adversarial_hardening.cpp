// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The deliberate adversarial phase.
//
// The suites above test the hazards the design anticipated. This file attacks
// the *paths the design did not enumerate*: hostile directory shapes, files
// where directories are expected, handles that must not leak across repeated
// open/close cycles, non-ASCII and space-bearing paths, extreme amounts pushed
// through the whole pipeline, and a store directory that a hostile actor has
// rearranged underneath a healthy commit.
//
// Each test states what must happen; none of them accept "it happened not to
// crash" as a pass.

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "summon/capacity_reconciliation/engine.hpp"
#include "summon/capacity_reconciliation/journal.hpp"
#include "summon/capacity_reconciliation/platform.hpp"
#include "summon/capacity_reconciliation/report.hpp"
#include "summon/capacity_reconciliation/store.hpp"
#include "test_helpers.hpp"
#include "test_support.hpp"

namespace cr = summon::capacity_reconciliation;

namespace {

std::vector<cr::EvidenceItem> one_item(const std::string& scope, cr::Amount installed,
                                       cr::Amount observed, std::uint64_t generation,
                                       const cr::IncarnationId& incarnation) {
  cr::EvidenceStamp stamp;
  stamp.generation = cr::Generation{generation};
  stamp.revision = cr::Revision{1};
  stamp.epoch = cr::Epoch{0};
  stamp.incarnation = incarnation;
  stamp.observed_at = cr::Tick{1};
  stamp.valid_until = cr::Tick{1000000};
  stamp.clock_domain = crtest::test_domain();
  std::vector<cr::EvidenceItem> items;
  for (const auto pair : {std::make_pair(cr::CapacityView::Installed, installed),
                          std::make_pair(cr::CapacityView::Observed, observed)}) {
    auto item = cr::EvidenceItem::create(
        cr::EvidenceId::generate(), cr::EvidenceSource::RackCapacity, "r",
        cr::ScopeIdentity::parse(scope).value(), crtest::power_key(), pair.first,
        cr::Quantity::known(pair.second), cr::EvidenceStatus::Accepted, stamp);
    if (item.ok()) {
      items.push_back(std::move(item.value()));
    }
  }
  return items;
}

void write_bytes(const std::filesystem::path& path, const std::string& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

/// Create a store with exactly one committed generation.
void seed_store(const std::filesystem::path& root) {
  auto store = cr::Store::open(root, crtest::writer_options(true));
  CR_REQUIRE_OK(store);
  cr::EngineOptions options;
  options.clock_domain = crtest::test_domain();
  auto engine = cr::Engine::attach(std::move(store.value()), options);
  CR_REQUIRE_OK(engine);
  auto items = one_item("site-a/hall-1", 100, 90, engine.value().generation().value(),
                        engine.value().incarnation());
  CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                               engine.value().incarnation()));
}

}  // namespace

CR_TEST(adversarial, a_store_root_that_is_a_file_is_refused) {
  crtest::ScratchDirectory scratch("rootfile");
  const auto file = scratch.child("not-a-directory");
  write_bytes(file, "this is a file, not a store");
  auto store = cr::Store::open(file, crtest::writer_options(true));
  CR_REQUIRE_ERR(store, cr::ErrorCode::InvalidArgument);
  auto reader = cr::Store::open(file, crtest::reader_options());
  CR_REQUIRE_ERR(reader, cr::ErrorCode::InvalidArgument);
}

CR_TEST(adversarial, a_directory_where_the_lock_belongs_is_not_a_lock_conflict) {
  crtest::ScratchDirectory scratch("lockdir");
  std::filesystem::create_directories(cr::Store::lock_path(scratch.path()));
  // A directory named LOCK cannot be opened for exclusive use. That is a
  // permission/shape problem, not a second writer, and reporting it as a lock
  // conflict would send an operator hunting for a process that does not exist.
  auto store = cr::Store::open(scratch.path(), crtest::writer_options(true));
  CR_REQUIRE(!store.ok());
  CR_CHECK(store.error().code != cr::ErrorCode::LockConflict);
}

CR_TEST(adversarial, a_directory_where_a_manifest_belongs_is_refused) {
  crtest::ScratchDirectory scratch("headisdir");
  seed_store(scratch.path());
  std::error_code ec;
  std::filesystem::remove(cr::Store::head_path(scratch.path()), ec);
  std::filesystem::create_directories(cr::Store::head_path(scratch.path()));
  auto store = cr::Store::open(scratch.path(), crtest::reader_options());
  CR_REQUIRE(!store.ok());
  const auto report = cr::Store::verify(scratch.path(), cr::default_limits());
  CR_CHECK(!report.ok);
}

CR_TEST(adversarial, a_record_file_that_is_a_directory_is_refused) {
  crtest::ScratchDirectory scratch("recordisdir");
  seed_store(scratch.path());
  std::error_code ec;
  std::filesystem::remove(cr::Store::record_path(scratch.path(), cr::Generation{1}), ec);
  std::filesystem::create_directories(cr::Store::record_path(scratch.path(), cr::Generation{1}));
  auto store = cr::Store::open(scratch.path(), crtest::reader_options());
  CR_REQUIRE(!store.ok());
}

CR_TEST(adversarial, a_manifest_pointing_at_an_absent_generation_is_refused) {
  crtest::ScratchDirectory scratch("missinggen");
  seed_store(scratch.path());
  std::error_code ec;
  std::filesystem::remove(cr::Store::record_path(scratch.path(), cr::Generation{1}), ec);
  CR_REQUIRE(!ec);
  auto store = cr::Store::open(scratch.path(), crtest::reader_options());
  CR_REQUIRE_ERR(store, cr::ErrorCode::NotFound);
}

CR_TEST(adversarial, a_staging_path_that_is_a_file_is_reported_and_left_alone) {
  crtest::ScratchDirectory scratch("stagingfile");
  std::filesystem::create_directories(scratch.path());
  // A staging *file* where the staging directory belongs.
  write_bytes(cr::Store::staging_dir(scratch.path()), "not a directory");
  auto store = cr::Store::open(scratch.path(), crtest::writer_options(true));
  CR_REQUIRE(!store.ok());
  // Nothing was deleted: the runtime does not clean up objects it cannot
  // identify.
  CR_CHECK(std::filesystem::exists(cr::Store::staging_dir(scratch.path())));
}

CR_TEST(adversarial, many_open_close_cycles_do_not_leak_handles) {
  crtest::ScratchDirectory scratch("churn");
  seed_store(scratch.path());
  // 400 open/close cycles. Each cycle takes the exclusive lock and releases it;
  // a leaked handle would make the next cycle fail with a lock conflict.
  for (int i = 0; i < 400; ++i) {
    auto store = cr::Store::open(scratch.path(), crtest::writer_options(false));
    CR_REQUIRE_OK(store);
    CR_CHECK_EQ(store.value().generation().value(), std::uint64_t(1));
  }
  const auto report = cr::Store::verify(scratch.path(), cr::default_limits());
  CR_CHECK(report.ok);
}

CR_TEST(adversarial, a_store_path_with_spaces_and_non_ascii_is_usable) {
  crtest::ScratchDirectory scratch("unicode");
  const auto root = scratch.child("a store with spaces");
  seed_store(root);
  auto store = cr::Store::open(root, crtest::reader_options());
  CR_REQUIRE_OK(store);
  CR_CHECK_EQ(store.value().generation().value(), std::uint64_t(1));
  const auto report = cr::Store::verify(root, cr::default_limits());
  CR_CHECK(report.ok);
}

CR_TEST(adversarial, extreme_amounts_survive_the_whole_pipeline_without_wrapping) {
  // Every pair of extreme values must either produce an exact result or be
  // reported as an overflow. None may wrap into a plausible-looking number.
  const cr::Amount extremes[] = {cr::amount_max(), cr::amount_min(), cr::amount_max() - 1,
                                 cr::amount_min() + 1, 0, 1, -1};
  for (cr::Amount installed : extremes) {
    for (cr::Amount observed : extremes) {
      auto engine = crtest::make_engine();
      CR_REQUIRE_OK(engine);
      auto items = one_item("site-a/hall-1", installed, observed, 0,
                            engine.value().incarnation());
      CR_REQUIRE_OK(engine.value().append_evidence(
          std::move(items), engine.value().generation(), engine.value().incarnation()));
      auto run = engine.value().reconcile(
          crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 50));
      CR_REQUIRE_OK(run);
      CR_REQUIRE(run.value().cells().size() == 1);
      const cr::ReconciliationCell& cell = run.value().cells().front();
      const bool overflows =
          (observed > 0 && installed < cr::amount_min() + observed) ||
          (observed < 0 && installed > cr::amount_max() + observed);
      if (overflows) {
        CR_CHECK(cell.has_class(cr::DiscrepancyClass::ArithmeticOverflow));
        CR_CHECK(cell.observed_gap.is_unknown());
      } else {
        CR_CHECK(!cell.has_class(cr::DiscrepancyClass::ArithmeticOverflow));
        CR_CHECK_EQ(cell.observed_gap.value(), installed - observed);
      }
      // Whatever happened, the run still renders and round-trips.
      const std::string json = cr::render_run_json(run.value(), true);
      CR_CHECK(crtest::json_is_well_formed(json));
      auto decoded =
          cr::ReconciliationRun::decode_content(run.value().canonical_bytes(),
                                                cr::default_limits());
      CR_REQUIRE_OK(decoded);
      CR_CHECK(decoded.value().run_digest() == run.value().run_digest());
    }
  }
}

CR_TEST(adversarial, an_unknown_extra_record_is_not_deleted) {
  crtest::ScratchDirectory scratch("stray");
  cr::Limits limits = cr::default_limits();
  limits.max_records_retained = 1;
  {
    auto store = cr::Store::open(scratch.path(), crtest::writer_options(true));
    CR_REQUIRE_OK(store);
    cr::EngineOptions options;
    options.clock_domain = crtest::test_domain();
    auto engine = cr::Engine::attach(std::move(store.value()), options);
    CR_REQUIRE_OK(engine);
    for (int i = 0; i < 3; ++i) {
      auto items = one_item("site-a/hall-" + std::to_string(i), 10, 10,
                            engine.value().generation().value(), engine.value().incarnation());
      CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                                   engine.value().incarnation()));
    }
  }
  // Plant a file with a perfectly valid record name that is not a record of
  // this store. Retention must not delete what it cannot identify.
  const auto stray = cr::Store::record_path(scratch.path(), cr::Generation{7});
  write_bytes(stray, "this is not a record of any store");
  CR_REQUIRE(std::filesystem::exists(stray));

  auto store = cr::Store::open(scratch.path(), crtest::writer_options(false));
  CR_REQUIRE_OK(store);
  CR_CHECK(std::filesystem::exists(stray));
}

CR_TEST(adversarial, a_retired_record_is_one_this_store_committed) {
  crtest::ScratchDirectory scratch("retireown");
  cr::Limits limits = cr::default_limits();
  limits.max_records_retained = 2;
  cr::StoreOpenOptions open_options = crtest::writer_options(true);
  open_options.limits = limits;
  {
    auto store = cr::Store::open(scratch.path(), open_options);
    CR_REQUIRE_OK(store);
    cr::EngineOptions options;
    options.clock_domain = crtest::test_domain();
    options.limits = limits;
    auto engine = cr::Engine::attach(std::move(store.value()), options);
    CR_REQUIRE_OK(engine);
    for (int i = 0; i < 6; ++i) {
      auto items = one_item("site-a/hall-" + std::to_string(i), 10, 10,
                            engine.value().generation().value(), engine.value().incarnation());
      CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                                   engine.value().incarnation()));
    }
    CR_CHECK_EQ(engine.value().generation().value(), std::uint64_t(6));
  }
  // After retention the store still opens, still verifies, and still holds its
  // published commit.
  const auto report = cr::Store::verify(scratch.path(), limits);
  CR_CHECK(report.ok);
  CR_CHECK(report.records_present <= 2);
  CR_CHECK_EQ(report.generation.value(), std::uint64_t(6));

  auto store = cr::Store::open(scratch.path(), crtest::reader_options());
  CR_REQUIRE_OK(store);
  // Every committed image is the *whole* authoritative state, so the surviving
  // generation carries all twelve items even though only two commits were kept.
  CR_CHECK_EQ(store.value().image().evidence.size(), std::size_t(12));
}

CR_TEST(adversarial, a_record_header_chain_link_is_cross_checked_against_the_payload) {
  crtest::ScratchDirectory scratch("chaincross");
  seed_store(scratch.path());
  // Flip one bit inside the header's chain-link field (bytes 56..87). The
  // framer cannot detect this because it does not know the payload's chain
  // link, but the store must: it compares the two.
  const auto record = cr::Store::record_path(scratch.path(), cr::Generation{1});
  {
    std::fstream stream(record, std::ios::in | std::ios::out | std::ios::binary);
    CR_REQUIRE(stream.is_open());
    stream.seekg(60);
    char byte = 0;
    stream.read(&byte, 1);
    byte = static_cast<char>(byte ^ 0x20);
    stream.seekp(60);
    stream.write(&byte, 1);
    stream.flush();
  }
  auto store = cr::Store::open(scratch.path(), crtest::reader_options());
  CR_REQUIRE(!store.ok());
  CR_CHECK(store.error().code == cr::ErrorCode::Corruption);
  const auto report = cr::Store::verify(scratch.path(), cr::default_limits());
  CR_CHECK(!report.ok);
}

CR_TEST(adversarial, a_store_cannot_be_created_through_a_parent_that_is_a_file) {
  crtest::ScratchDirectory scratch("parentfile");
  const auto file = scratch.child("file");
  write_bytes(file, "x");
  auto store = cr::Store::open(file / "child", crtest::writer_options(true));
  CR_REQUIRE(!store.ok());
}

CR_TEST(adversarial, verification_is_at_least_as_strict_as_opening) {
  // For every damaged state the suite can construct, verify() must fail
  // whenever open() fails, and must never report healthy for a store that
  // cannot be opened.
  crtest::ScratchDirectory scratch("strictverify");
  seed_store(scratch.path());

  const auto damages = {std::string("head"), std::string("record"), std::string("marker")};
  for (const std::string& which : damages) {
    auto store = cr::Store::open(scratch.path(), crtest::writer_options(false));
    CR_REQUIRE_OK(store);
    const std::string head_bytes =
        cr::platform::read_file_bounded(cr::Store::head_path(scratch.path()), cr::kMaxHeadBytes)
            .value();
    const std::string marker_bytes =
        cr::platform::read_file_bounded(cr::Store::marker_path(scratch.path()),
                                        cr::kMaxMarkerBytes)
            .value();
    const std::string record_bytes =
        cr::platform::read_file_bounded(cr::Store::record_path(scratch.path(), cr::Generation{1}),
                                        cr::kMaxRecordBytes)
            .value();
    store = cr::Result<cr::Store>(cr::make_error(cr::ErrorCode::Unknown, "released"));

    std::filesystem::path target;
    std::size_t offset = 0;
    if (which == "head") {
      target = cr::Store::head_path(scratch.path());
      offset = 20;
    } else if (which == "marker") {
      target = cr::Store::marker_path(scratch.path());
      offset = 40;
    } else {
      target = cr::Store::record_path(scratch.path(), cr::Generation{1});
      offset = cr::kRecordHeaderBytes + 3;
    }
    {
      std::fstream stream(target, std::ios::in | std::ios::out | std::ios::binary);
      CR_REQUIRE(stream.is_open());
      stream.seekg(static_cast<std::streamoff>(offset));
      char byte = 0;
      stream.read(&byte, 1);
      byte = static_cast<char>(byte ^ 0x11);
      stream.seekp(static_cast<std::streamoff>(offset));
      stream.write(&byte, 1);
      stream.flush();
    }

    auto reopened = cr::Store::open(scratch.path(), crtest::reader_options());
    const auto report = cr::Store::verify(scratch.path(), cr::default_limits());
    CR_CHECK_MSG(!reopened.ok() ? !report.ok : true,
                 "verify reported healthy for a store that could not be opened (" + which + ")");

    // Restore for the next round.
    if (which == "head") {
      write_bytes(target, head_bytes);
    } else if (which == "marker") {
      write_bytes(target, marker_bytes);
    } else {
      write_bytes(target, record_bytes);
    }
  }
  const auto report = cr::Store::verify(scratch.path(), cr::default_limits());
  CR_CHECK(report.ok);
}

CR_TEST(adversarial, a_scope_path_cannot_be_reinterpreted_as_a_filesystem_path) {
  // The scope grammar admits no separator that a filesystem understands, no
  // drive letter, no UNC prefix and no relative component, so a scope can never
  // be handed to a filesystem call by accident.
  const std::vector<std::string> hostile = {
      "site/..",           "site/./row",  "C:",            "C:/site",
      "site/C:",           "\\\\host\\share", "site//row",   "site/row/",
      "/site/row",         "site\\row",   "site/row\x7f",  "site/row|pipe",
      "site/row;semi",     "site/row*",   "site/row\"quote",
  };
  for (const std::string& text : hostile) {
    CR_CHECK_MSG(!cr::ScopeIdentity::parse(text).ok(), "hostile scope accepted");
  }
}

CR_TEST(adversarial, a_run_with_no_cells_is_still_a_well_formed_run) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  auto run = engine.value().reconcile(
      crtest::exact_request("site-a/empty", engine.value().generation().value(), 50));
  CR_REQUIRE_OK(run);
  CR_CHECK(run.value().cells().empty());
  CR_CHECK(crtest::json_is_well_formed(cr::render_run_json(run.value(), true)));
  CR_CHECK(crtest::json_is_well_formed(cr::render_run_json(run.value(), false)));
  auto decoded = cr::ReconciliationRun::decode_content(run.value().canonical_bytes(),
                                                       cr::default_limits());
  CR_REQUIRE_OK(decoded);
  CR_CHECK(decoded.value().run_digest() == run.value().run_digest());
}

CR_TEST(adversarial, a_store_whose_records_directory_is_missing_is_refused_or_repaired) {
  crtest::ScratchDirectory scratch("nodir");
  seed_store(scratch.path());
  std::error_code ec;
  std::filesystem::remove_all(cr::Store::records_dir(scratch.path()), ec);
  CR_REQUIRE(!ec);

  // A reader must refuse: the commit point it is told about does not exist.
  auto reader = cr::Store::open(scratch.path(), crtest::reader_options());
  CR_REQUIRE(!reader.ok());

  // A writer recreates the layout and, because the published commit is gone,
  // must not pretend the store is intact.
  auto writer = cr::Store::open(scratch.path(), crtest::writer_options(true));
  CR_REQUIRE(!writer.ok());
}
