// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// capacity_reconciliation_worker -- a second, independent operating-system
// process used to prove process-level authority.
//
// This tool exists so that lock fencing, writer exclusion and process-death
// release are demonstrated with real processes rather than with threads, which
// cannot prove either property. It prints exactly one machine-readable line per
// outcome and exits with a code the caller can assert on.
//
//   hold      <store> [--release PATH]  open as writer, print ACQUIRED, wait
//   try       <store>               open as writer, print ACQUIRED or LOCK_CONFLICT
//   read      <store>               open read-only, print GENERATION <n>
//   commit    <store> --scope P --dim D:U --planned N --installed N --observed N ...
//   abort     <store>               open as writer, print ACQUIRED, then abort()
//   verify    <store>               verify and print VERIFY ok|failed

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "cli_support.hpp"
#include "summon/capacity_reconciliation/engine.hpp"
#include "summon/capacity_reconciliation/report.hpp"
#include "summon/capacity_reconciliation/store.hpp"

namespace cr = summon::capacity_reconciliation;
namespace tools = summon::capacity_reconciliation::tools;

namespace {

int usage() {
  std::fputs(
      "capacity_reconciliation_worker hold|try|read|commit|abort|verify <store> [options]\n",
      stderr);
  return 2;
}

cr::Result<cr::Store> open_writer(const std::string& path) {
  cr::StoreOpenOptions options;
  options.create_if_missing = true;
  options.writer = true;
  return cr::Store::open(std::filesystem::path(path), options);
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
  // A worker that dies on purpose must die *silently*: no abort message, no
  // fault-report dialog, no Windows Error Reporting prompt. The whole point is
  // to observe what the operating system does when a process disappears, and an
  // interactive dialog would both block that observation and violate the
  // no-popup rule.
  _set_abort_behavior(0, 0);
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
  if (argc < 3) {
    return usage();
  }
  const std::string command(argv[1]);
  const std::string store_path(argv[2]);
  const tools::Args args(argc, argv, 3);

  if (command == "try") {
    auto store = open_writer(store_path);
    if (!store.ok()) {
      if (store.error().code == cr::ErrorCode::LockConflict) {
        std::printf("LOCK_CONFLICT %s\n", store.error().message.c_str());
        std::fflush(stdout);
        return 10;
      }
      std::printf("ERROR %s %s\n", cr::to_string(store.error().code),
                  store.error().message.c_str());
      std::fflush(stdout);
      return 1;
    }
    std::printf("ACQUIRED %llu\n",
                static_cast<unsigned long long>(store.value().generation().value()));
    std::fflush(stdout);
    return 0;
  }

  if (command == "hold") {
    auto store = open_writer(store_path);
    if (!store.ok()) {
      if (store.error().code == cr::ErrorCode::LockConflict) {
        std::printf("LOCK_CONFLICT %s\n", store.error().message.c_str());
        std::fflush(stdout);
        return 10;
      }
      std::printf("ERROR %s\n", cr::to_string(store.error().code));
      std::fflush(stdout);
      return 1;
    }
    std::printf("ACQUIRED %llu\n",
                static_cast<unsigned long long>(store.value().generation().value()));
    std::fflush(stdout);
    // Hold the lock until the caller releases it. Two mechanisms, both
    // event-based: a release file the parent creates, or closure of standard
    // input. Neither is a timeout, so a holder never releases the lock because
    // time passed.
    const std::string release = args.value_or("release", "");
    if (!release.empty()) {
      const std::filesystem::path release_path(release);
      std::error_code ec;
      while (!std::filesystem::exists(release_path, ec)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    } else {
      char buffer[16];
      while (std::fgets(buffer, sizeof(buffer), stdin) != nullptr) {
      }
    }
    std::printf("RELEASING\n");
    std::fflush(stdout);
    return 0;
  }

  if (command == "abort") {
    auto store = open_writer(store_path);
    if (!store.ok()) {
      std::printf("ERROR %s\n", cr::to_string(store.error().code));
      std::fflush(stdout);
      return 1;
    }
    std::printf("ACQUIRED %llu\n",
                static_cast<unsigned long long>(store.value().generation().value()));
    std::fflush(stdout);
    // Deliberately die without unwinding, without closing handles and without
    // running any destructor. This is how a killed writer looks.
    std::abort();
  }

  if (command == "read") {
    cr::StoreOpenOptions options;
    options.create_if_missing = false;
    options.writer = false;
    auto store = cr::Store::open(std::filesystem::path(store_path), options);
    if (!store.ok()) {
      std::printf("ERROR %s\n", cr::to_string(store.error().code));
      std::fflush(stdout);
      return 1;
    }
    std::printf("GENERATION %llu\n",
                static_cast<unsigned long long>(store.value().generation().value()));
    std::fflush(stdout);
    return 0;
  }

  if (command == "verify") {
    const auto report = cr::Store::verify(std::filesystem::path(store_path), cr::default_limits());
    std::printf("VERIFY %s\n", report.ok ? "ok" : "failed");
    for (const std::string& problem : report.problems) {
      std::printf("PROBLEM %s\n", problem.c_str());
    }
    std::fflush(stdout);
    return report.ok ? 0 : 1;
  }

  if (command == "commit") {
    auto store = open_writer(store_path);
    if (!store.ok()) {
      std::printf("ERROR %s\n", cr::to_string(store.error().code));
      std::fflush(stdout);
      return 1;
    }
    cr::EngineOptions options;
    options.clock_domain = cr::ClockDomain{args.value_or("domain", "facility-default")};
    auto engine = cr::Engine::attach(std::move(store.value()), options);
    if (!engine.ok()) {
      std::printf("ERROR %s\n", cr::to_string(engine.error().code));
      std::fflush(stdout);
      return 1;
    }
    const std::string scope_text = args.value_or("scope", "site-a/hall-1/row-1");
    auto scope = cr::ScopeIdentity::parse(scope_text);
    if (!scope.ok()) {
      std::printf("ERROR %s\n", cr::to_string(scope.error().code));
      std::fflush(stdout);
      return 1;
    }
    auto dimension = cr::DimensionKey::parse(args.value_or("dim", "power:milliwatt"));
    if (!dimension.has_value()) {
      std::printf("ERROR invalid_argument\n");
      std::fflush(stdout);
      return 1;
    }
    const auto generation = engine.value().generation();
    const auto incarnation = engine.value().incarnation();

    cr::EvidenceStamp stamp;
    stamp.generation = generation;
    stamp.revision = cr::Revision{1};
    stamp.epoch = cr::Epoch{0};
    stamp.incarnation = incarnation;
    stamp.observed_at = cr::Tick{std::strtoull(args.value_or("tick", "1").c_str(), nullptr, 10)};
    stamp.valid_until = cr::Tick{std::strtoull(args.value_or("until", "1000000").c_str(),
                                               nullptr, 10)};
    stamp.clock_domain = engine.value().options().clock_domain;

    std::vector<cr::EvidenceItem> items;
    const auto make_item = [&](cr::CapacityView view,
                               const std::string& text) -> cr::Result<cr::EvidenceItem> {
      cr::Quantity quantity = cr::Quantity::unknown(cr::UnknownReason::NotReported);
      if (text.rfind("unknown:", 0) == 0) {
        auto reason = cr::unknown_reason_from_string(text.substr(8));
        if (!reason.has_value()) {
          return cr::make_error(cr::ErrorCode::InvalidArgument, "unknown reason", "value");
        }
        quantity = cr::Quantity::unknown(*reason);
      } else {
        quantity = cr::Quantity::known(std::strtoll(text.c_str(), nullptr, 10));
      }
      return cr::EvidenceItem::create(cr::EvidenceId::generate(), cr::EvidenceSource::OperatorDeclaration,
                                      "worker", scope.value(), *dimension, view, quantity,
                                      cr::EvidenceStatus::Accepted, stamp);
    };

    const char* keys[] = {"planned", "reserved", "installed", "observed", "usable", "allocatable"};
    const cr::CapacityView views[] = {cr::CapacityView::Planned, cr::CapacityView::Reserved,
                                      cr::CapacityView::Installed, cr::CapacityView::Observed,
                                      cr::CapacityView::Usable, cr::CapacityView::Allocatable};
    for (std::size_t i = 0; i < 6; ++i) {
      auto value = args.value(keys[i]);
      if (!value.has_value()) {
        continue;
      }
      auto item = make_item(views[i], *value);
      if (!item.ok()) {
        std::printf("ERROR %s\n", cr::to_string(item.error().code));
        std::fflush(stdout);
        return 1;
      }
      items.push_back(std::move(item.value()));
    }

    auto status = engine.value().append_evidence(std::move(items), generation, incarnation);
    if (!status.ok()) {
      std::printf("ERROR %s %s\n", cr::to_string(status.error().code),
                  status.error().message.c_str());
      std::fflush(stdout);
      return 1;
    }
    std::printf("COMMITTED %llu\n",
                static_cast<unsigned long long>(engine.value().generation().value()));
    std::fflush(stdout);
    return 0;
  }

  return usage();
}
