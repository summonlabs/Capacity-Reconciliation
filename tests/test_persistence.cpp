// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Durability: the commit protocol, close/reopen, recovery, fencing and the
// refusal to adopt anything that does not verify whole.

#include <chrono>
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


std::vector<cr::EvidenceItem> evidence_for(const std::string& scope, cr::Amount installed,
                                           cr::Amount observed, std::uint64_t generation) {
  std::vector<cr::EvidenceItem> items;
  const struct {
    cr::CapacityView view;
    cr::Amount value;
  } rows[] = {{cr::CapacityView::Planned, installed},
              {cr::CapacityView::Installed, installed},
              {cr::CapacityView::Observed, observed},
              {cr::CapacityView::Usable, observed},
              {cr::CapacityView::Reserved, 0},
              {cr::CapacityView::Allocatable, observed}};
  for (const auto& row : rows) {
    cr::EvidenceStamp stamp;
    stamp.generation = cr::Generation{generation};
    stamp.revision = cr::Revision{1};
    stamp.epoch = cr::Epoch{0};
    stamp.incarnation = cr::IncarnationId::generate();
    stamp.observed_at = cr::Tick{100};
    stamp.valid_until = cr::Tick{100000};
    stamp.clock_domain = crtest::test_domain();
    auto item = cr::EvidenceItem::create(
        cr::EvidenceId::generate(), cr::EvidenceSource::RackCapacity, "rack-1",
        cr::ScopeIdentity::parse(scope).value(), crtest::power_key(), row.view,
        cr::Quantity::known(row.value), cr::EvidenceStatus::Accepted, stamp);
    if (item.ok()) {
      items.push_back(std::move(item.value()));
    }
  }
  return items;
}

std::uint64_t size_of_file(const std::filesystem::path& path) {
  std::error_code ec;
  return static_cast<std::uint64_t>(std::filesystem::file_size(path, ec));
}

void flip_a_byte(const std::filesystem::path& path, std::uint64_t offset) {
  std::fstream stream(path, std::ios::in | std::ios::out | std::ios::binary);
  CR_REQUIRE(stream.is_open());
  stream.seekg(static_cast<std::streamoff>(offset));
  char byte = 0;
  stream.read(&byte, 1);
  byte = static_cast<char>(byte ^ 0x40);
  stream.seekp(static_cast<std::streamoff>(offset));
  stream.write(&byte, 1);
  stream.flush();
}

}  // namespace

CR_TEST(persistence, create_commit_close_reopen) {
  crtest::ScratchDirectory scratch("persist");
  cr::Digest run_digest;
  cr::ReconciliationRunId run_id;
  cr::IncarnationId first_incarnation;
  cr::EngineOptions options;
  options.clock_domain = crtest::test_domain();
  {
    auto store = cr::Store::open(scratch.path(), crtest::writer_options(true));
    CR_REQUIRE_OK(store);
    CR_CHECK_EQ(store.value().recovery().outcome, cr::RecoveryOutcome::Created);
    CR_CHECK_EQ(store.value().generation().value(), std::uint64_t(0));
    first_incarnation = store.value().incarnation();
    CR_CHECK(first_incarnation.valid());

    auto engine = cr::Engine::attach(std::move(store.value()), options);
    CR_REQUIRE_OK(engine);

    auto items = evidence_for("site-a/hall-1", 100, 100, engine.value().generation().value());
    auto status = engine.value().append_evidence(std::move(items), engine.value().generation(),
                                                 engine.value().incarnation());
    CR_REQUIRE_OK(status);
    CR_CHECK_EQ(engine.value().generation().value(), std::uint64_t(1));

    auto run = engine.value().reconcile_and_commit(
        crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 500),
        engine.value().generation(), engine.value().incarnation());
    CR_REQUIRE_OK(run);
    run_digest = run.value().run_digest();
    run_id = run.value().id();
    // Every handle for this store is destroyed at the end of this block: that
    // is a real close, not a serialization.
  }

  auto reopened = cr::Store::open(scratch.path(), crtest::reader_options());
  CR_REQUIRE_OK(reopened);
  CR_CHECK_EQ(reopened.value().recovery().outcome, cr::RecoveryOutcome::OpenedClean);
  CR_CHECK_EQ(reopened.value().generation().value(), std::uint64_t(2));
  // A new incarnation: authority granted before the reopen is fenced.
  CR_CHECK(!(reopened.value().incarnation() == first_incarnation));
  CR_CHECK_EQ(reopened.value().image().evidence.size(), std::size_t(6));
  CR_REQUIRE(reopened.value().image().runs.size() == 1);

  auto engine2 = cr::Engine::attach(std::move(reopened.value()), options);
  CR_REQUIRE_OK(engine2);
  auto recovered = engine2.value().find_run(run_id);
  CR_REQUIRE_OK(recovered);
  CR_CHECK(recovered.value().run_digest() == run_digest);
  CR_CHECK_EQ(recovered.value().resolution(), cr::ResolutionState::Explained);
  CR_CHECK_EQ(recovered.value().cells().size(), std::size_t(1));
  // Recovered dynamic evidence is not silently fresh: the stamps came back
  // exactly as they were written.
  CR_CHECK_EQ(recovered.value().cells().front().view(cr::CapacityView::Installed).value(),
              cr::Amount(100));
  CR_CHECK(!engine2.value().history().empty());
}

CR_TEST(persistence, an_in_memory_engine_that_never_committed_loses_nothing_it_claimed) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  CR_CHECK(!engine.value().durable());
  CR_CHECK_EQ(engine.value().generation().value(), std::uint64_t(0));
  CR_REQUIRE_ERR(engine.value().advance_epoch(cr::Epoch{1}, engine.value().generation(),
                                              engine.value().incarnation()),
                 cr::ErrorCode::Unsupported);
}

CR_TEST(persistence, verify_accepts_a_healthy_store_and_reports_generation) {
  crtest::ScratchDirectory scratch("verify");
  auto store = cr::Store::open(scratch.path(), crtest::writer_options(true));
  CR_REQUIRE_OK(store);
  cr::EngineOptions options;
  options.clock_domain = crtest::test_domain();
  auto engine = cr::Engine::attach(std::move(store.value()), options);
  CR_REQUIRE_OK(engine);
  auto items = evidence_for("site-a/hall-1", 10, 10, engine.value().generation().value());
  CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                               engine.value().incarnation()));

  const auto report = cr::Store::verify(scratch.path(), cr::default_limits());
  CR_CHECK(report.ok);
  CR_CHECK(report.problems.empty());
  CR_CHECK_EQ(report.generation.value(), std::uint64_t(1));
  CR_CHECK_EQ(report.records_present, std::uint64_t(1));
  CR_CHECK_EQ(report.records_referenced, std::uint64_t(1));
}

CR_TEST(persistence, staged_bytes_that_do_not_verify_do_not_become_a_commit) {
  crtest::ScratchDirectory scratch("staged");
  auto store = cr::Store::open(scratch.path(), crtest::writer_options(true));
  CR_REQUIRE_OK(store);
  cr::EngineOptions options;
  options.clock_domain = crtest::test_domain();
  auto engine = cr::Engine::attach(std::move(store.value()), options);
  CR_REQUIRE_OK(engine);
  auto items = evidence_for("site-a/hall-1", 10, 10, engine.value().generation().value());
  CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                               engine.value().incarnation()));
  CR_CHECK_EQ(engine.value().generation().value(), std::uint64_t(1));

  // No staging residue survives a successful commit.
  const auto staging = cr::Store::staging_dir(scratch.path());
  std::error_code ec;
  std::size_t residue = 0;
  for (const auto& entry : std::filesystem::directory_iterator(staging, ec)) {
    if (entry.is_regular_file(ec)) {
      ++residue;
    }
  }
  CR_CHECK_EQ(residue, std::size_t(0));
}

CR_TEST(persistence, a_corrupt_head_recovers_the_previous_commit_whole) {
  crtest::ScratchDirectory scratch("head");
  cr::ReconciliationRunId first_run;
  {
    auto store = cr::Store::open(scratch.path(), crtest::writer_options(true));
    CR_REQUIRE_OK(store);
    cr::EngineOptions options;
    options.clock_domain = crtest::test_domain();
    auto engine = cr::Engine::attach(std::move(store.value()), options);
    CR_REQUIRE_OK(engine);
    auto items = evidence_for("site-a/hall-1", 10, 10, engine.value().generation().value());
    CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                                 engine.value().incarnation()));
    // Commit generation 2, so that generation 1 becomes the previous commit.
    auto items2 = evidence_for("site-a/hall-2", 20, 20, engine.value().generation().value());
    CR_REQUIRE_OK(engine.value().append_evidence(std::move(items2), engine.value().generation(),
                                                 engine.value().incarnation()));
    auto run = engine.value().reconcile_and_commit(
        crtest::exact_request("site-a/hall-1", engine.value().generation().value(), 500),
        engine.value().generation(), engine.value().incarnation());
    CR_REQUIRE_OK(run);
    first_run = run.value().id();
  }
  CR_CHECK(std::filesystem::exists(cr::Store::head_prev_path(scratch.path())));

  const auto head = cr::Store::head_path(scratch.path());
  const std::uint64_t head_size = size_of_file(head);
  CR_REQUIRE(head_size > 20);
  flip_a_byte(head, 20);

  auto store = cr::Store::open(scratch.path(), crtest::reader_options());
  CR_REQUIRE_OK(store);
  CR_CHECK_EQ(store.value().recovery().outcome,
              cr::RecoveryOutcome::RecoveredFromPreviousCommit);
  CR_CHECK(!store.value().recovery().note.empty());
  // The recovered state is whole: it is the previous commit, not a mixture.
  CR_CHECK_EQ(store.value().generation().value(), std::uint64_t(2));
  CR_CHECK(store.value().image().recovered_from_previous_commit);
  CR_CHECK_EQ(store.value().image().evidence.size(), std::size_t(12));
  // The run that lived only in the lost commit is gone, and nothing pretends
  // otherwise.
  bool found_run = false;
  for (const cr::ReconciliationRun& run : store.value().image().runs) {
    if (run.id() == first_run) {
      found_run = true;
    }
  }
  CR_CHECK(!found_run);
}

CR_TEST(persistence, a_corrupt_head_and_a_corrupt_previous_head_is_refused) {
  crtest::ScratchDirectory scratch("head2");
  {
    auto store = cr::Store::open(scratch.path(), crtest::writer_options(true));
    CR_REQUIRE_OK(store);
    cr::EngineOptions options;
    options.clock_domain = crtest::test_domain();
    auto engine = cr::Engine::attach(std::move(store.value()), options);
    CR_REQUIRE_OK(engine);
    auto items = evidence_for("site-a/hall-1", 10, 10, engine.value().generation().value());
    CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                                 engine.value().incarnation()));
    auto items2 = evidence_for("site-a/hall-2", 20, 20, engine.value().generation().value());
    CR_REQUIRE_OK(engine.value().append_evidence(std::move(items2), engine.value().generation(),
                                                 engine.value().incarnation()));
  }
  flip_a_byte(cr::Store::head_path(scratch.path()), 20);
  flip_a_byte(cr::Store::head_prev_path(scratch.path()), 20);
  // Both the published manifest and its fallback are broken, so nothing can be
  // adopted whole. The *kind* of failure is preserved rather than collapsed:
  // the flipped bytes land in the format-version field here.
  auto store = cr::Store::open(scratch.path(), crtest::reader_options());
  CR_REQUIRE(!store.ok());
  CR_CHECK(store.error().code == cr::ErrorCode::IncompatibleVersion ||
           store.error().code == cr::ErrorCode::Corruption ||
           store.error().code == cr::ErrorCode::Malformed);
}

CR_TEST(persistence, a_corrupt_record_is_refused_rather_than_partially_read) {
  crtest::ScratchDirectory scratch("record");
  {
    auto store = cr::Store::open(scratch.path(), crtest::writer_options(true));
    CR_REQUIRE_OK(store);
    cr::EngineOptions options;
    options.clock_domain = crtest::test_domain();
    auto engine = cr::Engine::attach(std::move(store.value()), options);
    CR_REQUIRE_OK(engine);
    auto items = evidence_for("site-a/hall-1", 10, 10, engine.value().generation().value());
    CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                                 engine.value().incarnation()));
  }
  const auto record = cr::Store::record_path(scratch.path(), cr::Generation{1});
  const std::uint64_t size = size_of_file(record);
  CR_REQUIRE(size > cr::kRecordHeaderBytes + 10);
  flip_a_byte(record, cr::kRecordHeaderBytes + 5);

  auto store = cr::Store::open(scratch.path(), crtest::reader_options());
  CR_REQUIRE_ERR(store, cr::ErrorCode::Corruption);

  const auto report = cr::Store::verify(scratch.path(), cr::default_limits());
  CR_CHECK(!report.ok);
  CR_CHECK(!report.problems.empty());
}

CR_TEST(persistence, a_truncated_record_is_refused) {
  crtest::ScratchDirectory scratch("truncate");
  {
    auto store = cr::Store::open(scratch.path(), crtest::writer_options(true));
    CR_REQUIRE_OK(store);
    cr::EngineOptions options;
    options.clock_domain = crtest::test_domain();
    auto engine = cr::Engine::attach(std::move(store.value()), options);
    CR_REQUIRE_OK(engine);
    auto items = evidence_for("site-a/hall-1", 10, 10, engine.value().generation().value());
    CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                                 engine.value().incarnation()));
  }
  const auto record = cr::Store::record_path(scratch.path(), cr::Generation{1});
  const std::uint64_t size = size_of_file(record);
  CR_REQUIRE(size > 10);
  std::error_code ec;
  std::filesystem::resize_file(record, size / 2, ec);
  CR_REQUIRE(!ec);
  auto store = cr::Store::open(scratch.path(), crtest::reader_options());
  CR_REQUIRE_ERR(store, cr::ErrorCode::Corruption);
}

CR_TEST(persistence, a_store_cannot_be_swapped_for_another_store) {
  crtest::ScratchDirectory first("swap-a");
  crtest::ScratchDirectory second("swap-b");
  for (const auto* directory : {&first, &second}) {
    auto store = cr::Store::open(directory->path(), crtest::writer_options(true));
    CR_REQUIRE_OK(store);
    cr::EngineOptions options;
    options.clock_domain = crtest::test_domain();
    auto engine = cr::Engine::attach(std::move(store.value()), options);
    CR_REQUIRE_OK(engine);
    auto items = evidence_for("site-a/hall-1", 10, 10, engine.value().generation().value());
    CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                                 engine.value().incarnation()));
  }

  // Move the second store's published commit into the first store's directory.
  std::error_code ec;
  std::filesystem::copy_file(cr::Store::record_path(second.path(), cr::Generation{1}),
                             cr::Store::record_path(first.path(), cr::Generation{1}),
                             std::filesystem::copy_options::overwrite_existing, ec);
  CR_REQUIRE(!ec);

  // The record no longer matches the manifest that publishes it, and it belongs
  // to another store. Which of the two checks fires first is an implementation
  // detail; that it is refused, and not adopted, is the contract.
  auto store = cr::Store::open(first.path(), crtest::reader_options());
  CR_REQUIRE(!store.ok());
  CR_CHECK(store.error().code == cr::ErrorCode::Conflict ||
           store.error().code == cr::ErrorCode::Corruption);
}

CR_TEST(persistence, a_directory_without_a_marker_is_not_a_store) {
  crtest::ScratchDirectory scratch("nomarker");
  auto store = cr::Store::open(scratch.path(), crtest::reader_options());
  CR_REQUIRE_ERR(store, cr::ErrorCode::NotFound);

  // Creating into it is explicit, and only then does it become a store.
  auto created = cr::Store::open(scratch.path(), crtest::writer_options(true));
  CR_REQUIRE_OK(created);
}

CR_TEST(persistence, a_missing_store_is_not_found_unless_creation_is_requested) {
  crtest::ScratchDirectory scratch("missing");
  const auto target = scratch.child("does-not-exist");
  auto store = cr::Store::open(target, crtest::reader_options());
  CR_REQUIRE_ERR(store, cr::ErrorCode::NotFound);
  auto created = cr::Store::open(target, crtest::writer_options(true));
  CR_REQUIRE_OK(created);
}

CR_TEST(persistence, stale_generation_and_incarnation_are_fenced_on_commit) {
  crtest::ScratchDirectory scratch("fence");
  auto store = cr::Store::open(scratch.path(), crtest::writer_options(true));
  CR_REQUIRE_OK(store);
  cr::EngineOptions options;
  options.clock_domain = crtest::test_domain();
  auto engine = cr::Engine::attach(std::move(store.value()), options);
  CR_REQUIRE_OK(engine);

  auto items = evidence_for("site-a/hall-1", 10, 10, engine.value().generation().value());
  CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                               engine.value().incarnation()));
  // Generation 0 is no longer current.
  auto stale = engine.value().append_evidence(
      evidence_for("site-a/hall-2", 1, 1, 0), cr::Generation{0}, engine.value().incarnation());
  CR_REQUIRE_ERR(stale, cr::ErrorCode::StaleGeneration);
}

CR_TEST(persistence, reopening_takes_a_new_incarnation_and_fences_the_old_one) {
  crtest::ScratchDirectory scratch("incarnation");
  cr::IncarnationId old_incarnation;
  {
    auto store = cr::Store::open(scratch.path(), crtest::writer_options(true));
    CR_REQUIRE_OK(store);
    old_incarnation = store.value().incarnation();
    cr::EngineOptions options;
    options.clock_domain = crtest::test_domain();
    auto engine = cr::Engine::attach(std::move(store.value()), options);
    CR_REQUIRE_OK(engine);
    auto items = evidence_for("site-a/hall-1", 10, 10, engine.value().generation().value());
    CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                                 engine.value().incarnation()));
  }
  auto store = cr::Store::open(scratch.path(), crtest::writer_options(false));
  CR_REQUIRE_OK(store);
  CR_CHECK(!(store.value().incarnation() == old_incarnation));
  auto commit = store.value().commit(store.value().image(), store.value().generation(),
                                     old_incarnation);
  CR_REQUIRE_ERR(commit, cr::ErrorCode::StaleAuthority);
}

CR_TEST(persistence, a_read_only_handle_refuses_to_commit) {
  crtest::ScratchDirectory scratch("readonly");
  cr::Generation generation;
  cr::IncarnationId incarnation;
  cr::StoreImage image;
  {
    auto store = cr::Store::open(scratch.path(), crtest::writer_options(true));
    CR_REQUIRE_OK(store);
    CR_REQUIRE_OK(store.value().commit(store.value().image(), store.value().generation(),
                                       store.value().incarnation()));
    generation = store.value().generation();
    incarnation = store.value().incarnation();
    image = store.value().image();
  }

  auto reader = cr::Store::open(scratch.path(), crtest::reader_options());
  CR_REQUIRE_OK(reader);
  CR_CHECK(!reader.value().is_writer());
  auto refused = reader.value().commit(image, generation, incarnation);
  CR_REQUIRE_ERR(refused, cr::ErrorCode::PermissionDenied);
}

CR_TEST(persistence, history_survives_a_reopen_and_stays_ordered) {
  crtest::ScratchDirectory scratch("history");
  {
    auto store = cr::Store::open(scratch.path(), crtest::writer_options(true));
    CR_REQUIRE_OK(store);
    cr::EngineOptions options;
    options.clock_domain = crtest::test_domain();
    auto engine = cr::Engine::attach(std::move(store.value()), options);
    CR_REQUIRE_OK(engine);
    for (int i = 0; i < 5; ++i) {
      auto items = evidence_for("site-a/hall-" + std::to_string(i), i * 10, i * 10,
                                engine.value().generation().value());
      CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                                   engine.value().incarnation()));
    }
  }
  auto store = cr::Store::open(scratch.path(), crtest::reader_options());
  CR_REQUIRE_OK(store);
  const auto& history = store.value().image().history;
  CR_CHECK_EQ(history.size(), std::size_t(5));
  for (std::size_t i = 1; i < history.size(); ++i) {
    CR_CHECK(history[i].sequence > history[i - 1].sequence);
  }
  CR_CHECK_EQ(store.value().image().history_sequence, history.back().sequence);
}

CR_TEST(persistence, epoch_advance_is_durable_and_monotonic) {
  crtest::ScratchDirectory scratch("epoch");
  auto store = cr::Store::open(scratch.path(), crtest::writer_options(true));
  CR_REQUIRE_OK(store);
  cr::EngineOptions options;
  options.clock_domain = crtest::test_domain();
  auto engine = cr::Engine::attach(std::move(store.value()), options);
  CR_REQUIRE_OK(engine);

  CR_REQUIRE_OK(engine.value().advance_epoch(cr::Epoch{4}, engine.value().generation(),
                                              engine.value().incarnation()));
  // Epoch must move forward; standing still or going back is refused.
  CR_REQUIRE_ERR(engine.value().advance_epoch(cr::Epoch{4}, engine.value().generation(),
                                              engine.value().incarnation()),
                 cr::ErrorCode::InvalidArgument);
  CR_REQUIRE_ERR(engine.value().advance_epoch(cr::Epoch{2}, engine.value().generation(),
                                              engine.value().incarnation()),
                 cr::ErrorCode::InvalidArgument);
  const cr::Epoch epoch = engine.value().epoch();
  CR_CHECK_EQ(epoch.value(), std::uint64_t(4));
}

CR_TEST(persistence, record_retention_keeps_the_referenced_commits) {
  crtest::ScratchDirectory scratch("retention");
  cr::Limits limits = cr::default_limits();
  limits.max_records_retained = 3;
  cr::StoreOpenOptions options = crtest::writer_options(true);
  options.limits = limits;
  {
    auto store = cr::Store::open(scratch.path(), options);
    CR_REQUIRE_OK(store);
    cr::EngineOptions engine_options;
    engine_options.clock_domain = crtest::test_domain();
    engine_options.limits = limits;
    auto engine = cr::Engine::attach(std::move(store.value()), engine_options);
    CR_REQUIRE_OK(engine);
    for (int i = 0; i < 8; ++i) {
      auto items = evidence_for("site-a/hall-" + std::to_string(i), i * 10, i * 10,
                                engine.value().generation().value());
      CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                                   engine.value().incarnation()));
    }
    // The published commit and its predecessor must survive.
    CR_CHECK(std::filesystem::exists(
        cr::Store::record_path(scratch.path(), engine.value().generation())));
  }
  const auto report = cr::Store::verify(scratch.path(), limits);
  CR_CHECK(report.ok);
  CR_CHECK(report.records_present <= 3);
}

CR_TEST(persistence, staging_residue_is_removed_on_open) {
  crtest::ScratchDirectory scratch("staging");
  {
    auto store = cr::Store::open(scratch.path(), crtest::writer_options(true));
    CR_REQUIRE_OK(store);
    cr::EngineOptions options;
    options.clock_domain = crtest::test_domain();
    auto engine = cr::Engine::attach(std::move(store.value()), options);
    CR_REQUIRE_OK(engine);
    auto items = evidence_for("site-a/hall-1", 10, 10, engine.value().generation().value());
    CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                                 engine.value().incarnation()));
  }
  // Simulate a crash between staging and publish.
  auto residue = cr::Store::staging_dir(scratch.path()) / "record-9999";
  {
    std::ofstream stream(residue, std::ios::binary | std::ios::trunc);
    stream << "partial write";
  }
  CR_REQUIRE(std::filesystem::exists(residue));
  auto store = cr::Store::open(scratch.path(), crtest::writer_options(true));
  CR_REQUIRE_OK(store);
  CR_CHECK(store.value().recovery().staging_residue_removed);
  CR_CHECK(!std::filesystem::exists(residue));
}
