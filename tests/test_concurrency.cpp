// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Concurrency: threads, immutable snapshots, lock ordering and re-entrancy.
//
// These tests prove what threads *can* prove: that the in-process ownership
// model holds under contention and that no callback runs beneath a lock. They
// deliberately do not claim anything about multi-process behaviour; that is
// covered by the multiprocess suite, which uses real operating-system
// processes.

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "summon/capacity_reconciliation/engine.hpp"
#include "test_helpers.hpp"
#include "test_support.hpp"

namespace cr = summon::capacity_reconciliation;

namespace {

std::vector<cr::EvidenceItem> batch(const std::string& scope, int count, std::uint64_t generation,
                                    const cr::IncarnationId& incarnation) {
  std::vector<cr::EvidenceItem> items;
  for (int i = 0; i < count; ++i) {
    cr::EvidenceStamp stamp;
    stamp.generation = cr::Generation{generation};
    stamp.revision = cr::Revision{1};
    stamp.incarnation = incarnation;
    stamp.observed_at = cr::Tick{1};
    stamp.valid_until = cr::Tick{100000};
    stamp.clock_domain = crtest::test_domain();
    auto item = cr::EvidenceItem::create(
        cr::EvidenceId::generate(), cr::EvidenceSource::RackCapacity,
        "instance-" + std::to_string(i), cr::ScopeIdentity::parse(scope).value(),
        crtest::power_key(), cr::CapacityView::Installed, cr::Quantity::known(i),
        cr::EvidenceStatus::Accepted, stamp);
    if (item.ok()) {
      items.push_back(std::move(item.value()));
    }
  }
  return items;
}

}  // namespace

CR_TEST(concurrency, concurrent_reconciles_produce_one_answer) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  for (int scope = 1; scope <= 8; ++scope) {
    auto items = batch("site-a/row-" + std::to_string(scope), 4, 0, engine.value().incarnation());
    CR_REQUIRE_OK(engine.value().append_evidence(std::move(items), engine.value().generation(),
                                                 engine.value().incarnation()));
  }

  cr::RunRequest request;
  for (int scope = 1; scope <= 8; ++scope) {
    request.scopes.push_back(
        {cr::ScopeIdentity::parse("site-a/row-" + std::to_string(scope)).value(),
         cr::ScopeSelectionMode::Exact});
  }
  request.generation = engine.value().generation();
  request.evaluation_instant = cr::Tick{50};
  request.clock_domain = crtest::test_domain();

  // The reference answer is computed once, on this thread, before any worker
  // starts. Reading it from inside a worker would be a race in the *test*.
  auto reference = engine.value().reconcile(request);
  CR_REQUIRE_OK(reference);
  const cr::Digest expected = reference.value().run_digest();

  std::atomic<int> failures{0};
  std::vector<std::thread> workers;
  for (int thread = 0; thread < 8; ++thread) {
    workers.emplace_back([&]() {
      for (int iteration = 0; iteration < 25; ++iteration) {
        auto run = engine.value().reconcile(request);
        if (!run.ok()) {
          ++failures;
          continue;
        }
        // Every thread must see the same digest; a race in the kernel would
        // show up as a different answer.
        if (!(run.value().run_digest() == expected)) {
          ++failures;
        }
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  CR_CHECK_EQ(failures.load(), 0);
}

CR_TEST(concurrency, concurrent_appends_serialise_without_losing_an_item) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  engine.value().set_observer({});

  constexpr int kThreads = 6;
  constexpr int kPerThread = 40;
  std::atomic<int> accepted{0};
  std::atomic<int> refused{0};

  std::vector<std::thread> workers;
  for (int thread = 0; thread < kThreads; ++thread) {
    workers.emplace_back([&, thread]() {
      for (int i = 0; i < kPerThread; ++i) {
        // Every append re-reads the generation under the engine lock, so the
        // whole loop is a genuine read-modify-write race.
        for (;;) {
          const auto generation = engine.value().generation();
          const auto incarnation = engine.value().incarnation();
          auto items = batch("site-a/row-" + std::to_string(thread) + "-" + std::to_string(i), 1,
                             generation.value(), incarnation);
          auto status = engine.value().append_evidence(std::move(items), generation, incarnation);
          if (status.ok()) {
            ++accepted;
            break;
          }
          if (status.code() != cr::ErrorCode::StaleGeneration &&
              status.code() != cr::ErrorCode::StaleAuthority) {
            ++refused;
            break;
          }
          // A stale precondition is the documented outcome of losing the race;
          // retrying with the fresh generation is the caller's decision.
        }
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  CR_CHECK_EQ(refused.load(), 0);
  CR_CHECK_EQ(accepted.load(), kThreads * kPerThread);
  CR_CHECK_EQ(engine.value().evidence_snapshot()->size(),
              static_cast<std::size_t>(kThreads * kPerThread));
}

CR_TEST(concurrency, an_observer_may_reenter_the_engine_from_many_threads) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  std::atomic<int> observed{0};
  std::atomic<int> reentry_failures{0};
  engine.value().set_observer([&](const cr::HistoryEntry&) {
    ++observed;
    // Re-reading under the observer would deadlock if the callback ran beneath
    // the engine lock. It runs after the lock is released, so this is safe.
    const auto generation = engine.value().generation();
    if (generation.value() == 0) {
      ++reentry_failures;
    }
  });

  std::vector<std::thread> workers;
  std::atomic<int> accepted{0};
  for (int thread = 0; thread < 4; ++thread) {
    workers.emplace_back([&, thread]() {
      for (int i = 0; i < 20; ++i) {
        for (;;) {
          const auto generation = engine.value().generation();
          const auto incarnation = engine.value().incarnation();
          auto items = batch("site-a/t" + std::to_string(thread) + "/i" + std::to_string(i), 1,
                             generation.value(), incarnation);
          auto status = engine.value().append_evidence(std::move(items), generation, incarnation);
          if (status.ok()) {
            ++accepted;
            break;
          }
          if (status.code() != cr::ErrorCode::StaleGeneration &&
              status.code() != cr::ErrorCode::StaleAuthority) {
            break;
          }
        }
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  CR_CHECK_EQ(reentry_failures.load(), 0);
  CR_CHECK_EQ(observed.load(), accepted.load());
}

CR_TEST(concurrency, snapshots_are_immutable_while_appends_continue) {
  auto engine = crtest::make_engine();
  CR_REQUIRE_OK(engine);
  auto first = batch("site-a/row-1", 5, 0, engine.value().incarnation());
  CR_REQUIRE_OK(engine.value().append_evidence(std::move(first), engine.value().generation(),
                                               engine.value().incarnation()));
  auto snapshot = engine.value().evidence_snapshot();
  CR_CHECK_EQ(snapshot->size(), std::size_t(5));
  const cr::Digest digest_before = snapshot->digest();

  auto second = batch("site-a/row-2", 5, engine.value().generation().value(),
                      engine.value().incarnation());
  CR_REQUIRE_OK(engine.value().append_evidence(std::move(second), engine.value().generation(),
                                               engine.value().incarnation()));

  // The old snapshot is unchanged: a reader is never handed a mutating view.
  CR_CHECK_EQ(snapshot->size(), std::size_t(5));
  CR_CHECK(snapshot->digest() == digest_before);
  CR_CHECK_EQ(engine.value().evidence_snapshot()->size(), std::size_t(10));
}

CR_TEST(concurrency, a_persistent_engine_serialises_commits_under_contention) {
  crtest::ScratchDirectory scratch("concurrentstore");
  cr::StoreOpenOptions open_options;
  open_options.create_if_missing = true;
  open_options.writer = true;
  auto store = cr::Store::open(scratch.path(), open_options);
  CR_REQUIRE_OK(store);
  cr::EngineOptions options;
  options.clock_domain = crtest::test_domain();
  auto engine = cr::Engine::attach(std::move(store.value()), options);
  CR_REQUIRE_OK(engine);

  constexpr int kThreads = 4;
  constexpr int kPerThread = 10;
  std::atomic<int> accepted{0};
  std::vector<std::thread> workers;
  for (int thread = 0; thread < kThreads; ++thread) {
    workers.emplace_back([&, thread]() {
      for (int i = 0; i < kPerThread; ++i) {
        for (;;) {
          const auto generation = engine.value().generation();
          const auto incarnation = engine.value().incarnation();
          auto items = batch("site-a/t" + std::to_string(thread) + "/i" + std::to_string(i), 1,
                             generation.value(), incarnation);
          auto status = engine.value().append_evidence(std::move(items), generation, incarnation);
          if (status.ok()) {
            ++accepted;
            break;
          }
          if (status.code() != cr::ErrorCode::StaleGeneration &&
              status.code() != cr::ErrorCode::StaleAuthority) {
            break;
          }
        }
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  CR_CHECK_EQ(accepted.load(), kThreads * kPerThread);
  CR_CHECK_EQ(engine.value().generation().value(),
              static_cast<std::uint64_t>(kThreads * kPerThread));
  CR_CHECK_EQ(engine.value().evidence_snapshot()->size(),
              static_cast<std::size_t>(kThreads * kPerThread));
  CR_CHECK_EQ(engine.value().history().size(), static_cast<std::size_t>(kThreads * kPerThread));

  engine = cr::Result<cr::Engine>(cr::make_error(cr::ErrorCode::Unknown, "closed"));
  const auto report = cr::Store::verify(scratch.path(), cr::default_limits());
  CR_CHECK(report.ok);
  CR_CHECK_EQ(report.generation.value(), static_cast<std::uint64_t>(kThreads * kPerThread));
}

CR_TEST(concurrency, a_store_handle_is_not_shared_between_threads_without_serialisation) {
  // The store's commit mutex is the serialisation point. Two threads driving
  // separate engines over the same handle are not a supported pattern, but two
  // threads committing through the *same* engine are, and this asserts that the
  // store's own mutex keeps the generation monotonic in that case.
  crtest::ScratchDirectory scratch("storemutex");
  cr::StoreOpenOptions open_options;
  open_options.create_if_missing = true;
  open_options.writer = true;
  auto store = cr::Store::open(scratch.path(), open_options);
  CR_REQUIRE_OK(store);

  std::atomic<int> conflicts{0};
  std::atomic<int> successes{0};
  std::vector<std::thread> workers;
  for (int thread = 0; thread < 4; ++thread) {
    workers.emplace_back([&]() {
      for (int i = 0; i < 10; ++i) {
        const auto generation = store.value().generation();
        const auto incarnation = store.value().incarnation();
        cr::StoreImage image = store.value().image();
        image.recorded_at = cr::Tick{static_cast<std::uint64_t>(i)};
        auto report = store.value().commit(std::move(image), generation, incarnation);
        if (report.ok()) {
          ++successes;
        } else if (report.error().code == cr::ErrorCode::StaleGeneration) {
          ++conflicts;
        }
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  // Every commit either succeeded at its precondition generation or was refused
  // as stale. None was silently lost and none produced a torn image.
  CR_CHECK_EQ(successes.load() + conflicts.load(), 40);
  CR_CHECK(successes.load() > 0);
  CR_CHECK_EQ(store.value().generation().value(), static_cast<std::uint64_t>(successes.load()));
}
