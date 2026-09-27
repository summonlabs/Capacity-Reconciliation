// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Real multi-process proof.
//
// Threads cannot prove writer exclusion, because two threads share a process
// and therefore share every handle the process holds. These tests start
// genuinely separate operating-system processes through the worker tool and
// assert on what the operating system does when one of them dies.
//
// The synchronisation here is event-based, not time-based: the parent waits for
// the child to publish a readiness marker or to exit. There is no timeout and
// no watchdog. If a child hangs, the test hangs, and that is a defect to
// diagnose rather than a test to kill.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "summon/capacity_reconciliation/store.hpp"
#include "test_helpers.hpp"
#include "test_support.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cr = summon::capacity_reconciliation;

namespace {

/// Path of the worker executable, discovered next to this test binary.
std::filesystem::path worker_path() {
#if defined(_WIN32)
  wchar_t buffer[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
  std::filesystem::path self(std::wstring(buffer, length));
  return self.parent_path().parent_path() / "tools" / "capacity_reconciliation_worker.exe";
#else
  return std::filesystem::current_path() / "capacity_reconciliation_worker";
#endif
}

#if defined(_WIN32)

/// A child process with its standard output redirected to a file the parent
/// owns. Output is read after the child exits, so nothing depends on timing.
class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess() { close(); }
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  [[nodiscard]] static bool spawn(const std::vector<std::string>& arguments,
                                  const std::filesystem::path& output_path,
                                  ChildProcess* out);

  /// True while the child has not exited.
  [[nodiscard]] bool running() const {
    return process_ != nullptr && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
  }

  /// Block until the child exits, however long that takes.
  void wait() {
    if (process_ != nullptr) {
      WaitForSingleObject(process_, INFINITE);
    }
  }

  [[nodiscard]] unsigned long exit_code() const {
    DWORD code = 0;
    if (process_ != nullptr) {
      GetExitCodeProcess(process_, &code);
    }
    return code;
  }

  /// Terminate without unwinding, without running destructors and without any
  /// cooperative cleanup. This is what an operator killing a process looks
  /// like.
  void terminate(unsigned long code) {
    if (process_ != nullptr) {
      TerminateProcess(process_, code);
      WaitForSingleObject(process_, INFINITE);
    }
  }

  [[nodiscard]] std::string read_output() const {
    std::ifstream stream(output_path_, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(stream)),
                       std::istreambuf_iterator<char>());
  }

  void close() {
    if (process_ != nullptr) {
      CloseHandle(process_);
      process_ = nullptr;
    }
  }

 private:
  HANDLE process_ = nullptr;
  std::filesystem::path output_path_;
};

bool ChildProcess::spawn(const std::vector<std::string>& arguments,
                         const std::filesystem::path& output_path, ChildProcess* out) {
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;

  HANDLE output = CreateFileW(output_path.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              &attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (output == INVALID_HANDLE_VALUE) {
    return false;
  }

  std::string command_line;
  for (const std::string& argument : arguments) {
    if (!command_line.empty()) {
      command_line.push_back(' ');
    }
    command_line.push_back('"');
    command_line.append(argument);
    command_line.push_back('"');
  }
  std::vector<wchar_t> wide(command_line.begin(), command_line.end());
  wide.push_back(L'\0');

  // All three standard handles must be valid *and* inheritable whenever
  // STARTF_USESTDHANDLES is set. Reusing this process's stdin is not safe: it
  // may be an invalid or non-inheritable handle, and the child would then be
  // started with a broken standard output as well.
  HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (input == INVALID_HANDLE_VALUE) {
    CloseHandle(output);
    return false;
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = output;
  startup.hStdError = output;
  startup.hStdInput = input;

  PROCESS_INFORMATION info{};
  const BOOL created = CreateProcessW(nullptr, wide.data(), nullptr, nullptr, TRUE, 0, nullptr,
                                      nullptr, &startup, &info);
  CloseHandle(output);
  CloseHandle(input);
  if (created == 0) {
    return false;
  }
  CloseHandle(info.hThread);
  out->process_ = info.hProcess;
  out->output_path_ = output_path;
  return true;
}

#else

class ChildProcess {
 public:
  [[nodiscard]] static bool spawn(const std::vector<std::string>&, const std::filesystem::path&,
                                  ChildProcess*) {
    return false;
  }
  [[nodiscard]] bool running() const { return false; }
  void wait() {}
  [[nodiscard]] unsigned long exit_code() const { return 0; }
  void terminate(unsigned long) {}
  [[nodiscard]] std::string read_output() const { return std::string(); }
  void close() {}
};

#endif

bool contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

/// Wait until the child either publishes `marker` in its output or exits. No
/// timeout: if the child does neither, the test hangs and is a defect.
bool wait_for_marker(ChildProcess* child, const std::string& marker, std::string* output) {
  for (;;) {
    *output = child->read_output();
    if (contains(*output, marker)) {
      return true;
    }
    if (!child->running()) {
      *output = child->read_output();
      return contains(*output, marker);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

}  // namespace

CR_TEST(multiprocess, a_second_process_cannot_acquire_a_held_store) {
  crtest::ScratchDirectory scratch("mp-lock");
  const auto worker = worker_path();
  CR_REQUIRE(std::filesystem::exists(worker));

  ChildProcess holder;
  CR_REQUIRE(ChildProcess::spawn({worker.string(), "hold", scratch.path().string(), "--release",
                                      scratch.child("release").string()},
                                 scratch.child("holder.log"), &holder));
  std::string holder_output;
  CR_REQUIRE(wait_for_marker(&holder, "ACQUIRED", &holder_output));

  // A second, independent process must be refused.
  ChildProcess contender;
  CR_REQUIRE(ChildProcess::spawn({worker.string(), "try", scratch.path().string()},
                                 scratch.child("contender.log"), &contender));
  contender.wait();
  const std::string contender_output = contender.read_output();
  CR_CHECK_EQ(contender.exit_code(), 10UL);
  CR_CHECK(contains(contender_output, "LOCK_CONFLICT"));

  // A read-only open in a third process still succeeds: reading a published
  // commit does not need the writer lock.
  ChildProcess reader;
  CR_REQUIRE(ChildProcess::spawn({worker.string(), "read", scratch.path().string()},
                                 scratch.child("reader.log"), &reader));
  reader.wait();
  CR_CHECK_EQ(reader.exit_code(), 0UL);
  CR_CHECK(contains(reader.read_output(), "GENERATION"));

  holder.terminate(1);
  holder.wait();
  CR_CHECK(!holder.running());
}

CR_TEST(multiprocess, process_death_relinquishes_writer_authority) {
  crtest::ScratchDirectory scratch("mp-death");
  const auto worker = worker_path();
  CR_REQUIRE(std::filesystem::exists(worker));

  // The child opens the store, prints ACQUIRED and then kills itself with
  // abort(): no unwinding, no destructors, no cooperative release.
  ChildProcess doomed;
  CR_REQUIRE(ChildProcess::spawn({worker.string(), "abort", scratch.path().string()},
                                 scratch.child("doomed.log"), &doomed));
  std::string doomed_output;
  CR_REQUIRE(wait_for_marker(&doomed, "ACQUIRED", &doomed_output));
  doomed.wait();
  CR_CHECK(!doomed.running());
  CR_CHECK(!doomed.exit_code() == 0UL);

  // The operating system must have released the lock with the process.
  ChildProcess successor;
  CR_REQUIRE(ChildProcess::spawn({worker.string(), "try", scratch.path().string()},
                                 scratch.child("successor.log"), &successor));
  successor.wait();
  const std::string output = successor.read_output();
  CR_CHECK_EQ(successor.exit_code(), 0UL);
  CR_CHECK(contains(output, "ACQUIRED"));

  // The store survived the death intact and still verifies.
  ChildProcess verifier;
  CR_REQUIRE(ChildProcess::spawn({worker.string(), "verify", scratch.path().string()},
                                 scratch.child("verify.log"), &verifier));
  verifier.wait();
  CR_CHECK_EQ(verifier.exit_code(), 0UL);
  CR_CHECK(contains(verifier.read_output(), "VERIFY ok"));
}

CR_TEST(multiprocess, a_terminated_process_relinquishes_writer_authority) {
  crtest::ScratchDirectory scratch("mp-terminate");
  const auto worker = worker_path();
  CR_REQUIRE(std::filesystem::exists(worker));

  ChildProcess holder;
  CR_REQUIRE(ChildProcess::spawn({worker.string(), "hold", scratch.path().string(), "--release",
                                      scratch.child("release").string()},
                                 scratch.child("holder.log"), &holder));
  std::string holder_output;
  CR_REQUIRE(wait_for_marker(&holder, "ACQUIRED", &holder_output));
  CR_CHECK(holder.running());

  // A hard kill from outside: no signal handler, no cleanup path.
  holder.terminate(1);
  holder.wait();

  ChildProcess successor;
  CR_REQUIRE(ChildProcess::spawn({worker.string(), "try", scratch.path().string()},
                                 scratch.child("successor.log"), &successor));
  successor.wait();
  CR_CHECK_EQ(successor.exit_code(), 0UL);
  CR_CHECK(contains(successor.read_output(), "ACQUIRED"));
}

CR_TEST(multiprocess, one_process_commits_and_another_reads_the_commit_back) {
  crtest::ScratchDirectory scratch("mp-commit");
  const auto worker = worker_path();
  CR_REQUIRE(std::filesystem::exists(worker));

  ChildProcess committed;
  CR_REQUIRE(ChildProcess::spawn({worker.string(), "commit", scratch.path().string(),
                                  "--scope", "site-a/hall-1", "--dim", "power:milliwatt",
                                  "--installed", "1000", "--observed", "900", "--planned",
                                  "1000"},
                                 scratch.child("commit.log"), &committed));
  committed.wait();
  CR_CHECK_EQ(committed.exit_code(), 0UL);
  CR_CHECK(contains(committed.read_output(), "COMMITTED"));

  // A separate process reads the committed generation back from disk.
  ChildProcess reader;
  CR_REQUIRE(ChildProcess::spawn({worker.string(), "read", scratch.path().string()},
                                 scratch.child("read.log"), &reader));
  reader.wait();
  CR_CHECK_EQ(reader.exit_code(), 0UL);
  CR_CHECK(contains(reader.read_output(), "GENERATION 1"));

  // And the parent process, which never held the lock, sees the same state.
  cr::StoreOpenOptions options;
  options.create_if_missing = false;
  options.writer = false;
  auto store = cr::Store::open(scratch.path(), options);
  CR_REQUIRE_OK(store);
  CR_CHECK_EQ(store.value().generation().value(), std::uint64_t(1));
  CR_CHECK_EQ(store.value().image().evidence.size(), std::size_t(3));

  // The parent can now take the lock, because the child released it by exiting.
  options.writer = true;
  auto writer = cr::Store::open(scratch.path(), options);
  CR_REQUIRE_OK(writer);
  const auto report = cr::Store::verify(scratch.path(), cr::default_limits());
  CR_CHECK(report.ok);
  CR_CHECK_EQ(report.generation.value(), std::uint64_t(1));
}

CR_TEST(multiprocess, a_process_that_dies_mid_commit_leaves_one_whole_state) {
  crtest::ScratchDirectory scratch("mp-midcommit");
  const auto worker = worker_path();
  CR_REQUIRE(std::filesystem::exists(worker));

  // Establish a committed generation.
  ChildProcess first;
  CR_REQUIRE(ChildProcess::spawn({worker.string(), "commit", scratch.path().string(),
                                  "--installed", "500", "--observed", "500"},
                                 scratch.child("first.log"), &first));
  first.wait();
  CR_REQUIRE(contains(first.read_output(), "COMMITTED"));

  // Kill a process that has only just acquired the store, before it commits.
  ChildProcess doomed;
  CR_REQUIRE(ChildProcess::spawn({worker.string(), "abort", scratch.path().string()},
                                 scratch.child("doomed.log"), &doomed));
  std::string doomed_output;
  CR_REQUIRE(wait_for_marker(&doomed, "ACQUIRED", &doomed_output));
  doomed.wait();

  // The store is still exactly the previous commit, and verifies whole.
  const auto report = cr::Store::verify(scratch.path(), cr::default_limits());
  CR_CHECK(report.ok);
  CR_CHECK_EQ(report.generation.value(), std::uint64_t(1));
  cr::StoreOpenOptions options;
  options.create_if_missing = false;
  options.writer = false;
  auto store = cr::Store::open(scratch.path(), options);
  CR_REQUIRE_OK(store);
  CR_CHECK_EQ(store.value().image().evidence.size(), std::size_t(2));
}

CR_TEST(multiprocess, many_processes_contend_and_exactly_one_wins) {
  crtest::ScratchDirectory scratch("mp-contend");
  const auto worker = worker_path();
  CR_REQUIRE(std::filesystem::exists(worker));

  // Establish the store, then release it.
  {
    ChildProcess first;
    CR_REQUIRE(ChildProcess::spawn({worker.string(), "try", scratch.path().string()},
                                   scratch.child("first.log"), &first));
    first.wait();
    CR_REQUIRE(contains(first.read_output(), "ACQUIRED"));
  }

  // Start eight processes at once. They are independent operating-system
  // processes. The winner holds the store until the parent creates the release
  // file, so the loser count is deterministic rather than a race outcome:
  // whoever wins keeps the lock, and every other process must be refused.
  const auto release = scratch.child("release");
  std::vector<ChildProcess> children(8);
  for (std::size_t i = 0; i < children.size(); ++i) {
    const auto log = scratch.child("contender-" + std::to_string(i) + ".log");
    CR_REQUIRE(ChildProcess::spawn({worker.string(), "hold", scratch.path().string(), "--release",
                                    release.string()},
                                   log, &children[i]));
  }

  // Wait for every child to have published a verdict. A holder that wins blocks
  // until the release file appears, so the loop watches for ACQUIRED or ERROR
  // in each log and for the process to have exited. There is no timeout: if a
  // child never publishes anything, the test hangs, and that is a defect.
  std::vector<bool> won(children.size(), false);
  std::size_t acquired = 0;
  std::size_t refused = 0;
  std::size_t unexplained = 0;
  for (std::size_t i = 0; i < children.size(); ++i) {
    for (;;) {
      const std::string output = children[i].read_output();
      if (contains(output, "ACQUIRED")) {
        ++acquired;
        won[i] = true;
        break;
      }
      if (contains(output, "LOCK_CONFLICT")) {
        ++refused;
        break;
      }
      if (!children[i].running()) {
        ++unexplained;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  // Exactly one process may hold the store; every other contender is refused.
  // Two writers at once is the failure this test exists to catch, and it is
  // decided by the operating system rather than by a convention in this code.
  CR_CHECK_EQ(acquired, std::size_t(1));
  CR_CHECK_EQ(refused, children.size() - 1);
  CR_CHECK_EQ(unexplained, std::size_t(0));

  {
    std::ofstream stream(release, std::ios::trunc);
    stream << "release";
  }
  for (std::size_t i = 0; i < children.size(); ++i) {
    children[i].wait();
    // The winner exits cleanly after the release; every loser exited at once
    // with the documented conflict code.
    CR_CHECK_EQ(children[i].exit_code(), won[i] ? 0UL : 10UL);
  }

  const auto report = cr::Store::verify(scratch.path(), cr::default_limits());
  CR_CHECK(report.ok);
}
