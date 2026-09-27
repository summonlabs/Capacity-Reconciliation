// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Platform primitives: filesystem, durable flush, atomic replace, file locks,
// CSPRNG entropy.
//
// Everything here has an explicit failure mode and an explicit guarantee. The
// guarantees that matter, and that the test suite proves, are:
//
//   * flush_file(): after it returns Ok, the bytes are with the operating
//     system's durable store (FlushFileBuffers / fsync), not merely in a cache;
//   * atomic_replace(): a concurrent reader sees either the old or the new
//     file, never a mixture, and never a missing file;
//   * ExclusiveFileLock: held by the OS on behalf of the process, so process
//     death releases it. This is what makes writer fencing real rather than a
//     convention.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "summon/capacity_reconciliation/status.hpp"

namespace summon::capacity_reconciliation::platform {

namespace fs = std::filesystem;

/// Fill `out` with `count` cryptographically strong random bytes.
[[nodiscard]] Status random_bytes(void* out, std::size_t count) noexcept;

/// Open (creating if needed), write, flush to durable storage and close.
///
/// `exclusive_create` uses O_EXCL / CREATE_NEW so that two processes cannot
/// both believe they created the same file.
[[nodiscard]] Status write_file_durable(const fs::path& path, std::string_view bytes,
                                        bool exclusive_create);

/// Read a whole file, refusing anything larger than `max_bytes` *before*
/// allocating. Opens with FILE_SHARE_READ only, so the file cannot be renamed
/// or deleted underneath the read on Windows.
[[nodiscard]] Result<std::string> read_file_bounded(const fs::path& path, std::size_t max_bytes);

/// Atomically replace `to` with `from`.
///
/// On Windows this is MoveFileExW with MOVEFILE_REPLACE_EXISTING and
/// MOVEFILE_WRITE_THROUGH. On POSIX it is rename(2) followed by an fsync of the
/// containing directory. Either way the operation is a single commit point.
[[nodiscard]] Status atomic_replace(const fs::path& from, const fs::path& to);

/// Flush a directory entry so that a preceding rename survives a crash. On
/// Windows directory handles cannot be flushed through the C++ standard
/// library, and NTFS metadata ordering makes MoveFileExW with
/// MOVEFILE_WRITE_THROUGH the durability point instead; this returns Ok there
/// and is documented as such rather than silently skipped.
[[nodiscard]] Status sync_directory(const fs::path& directory);

/// Create a directory and every missing parent.
[[nodiscard]] Status ensure_directory(const fs::path& directory);

/// Remove a file if present. A missing file is success.
[[nodiscard]] Status remove_file_if_present(const fs::path& path) noexcept;

/// True when the path exists and is a regular file (following no reparse
/// points: a symlink or junction is reported as not regular).
[[nodiscard]] bool is_regular_file_no_reparse(const fs::path& path) noexcept;

/// True when the path exists and is a symlink, junction or other reparse point.
[[nodiscard]] bool is_reparse_point(const fs::path& path) noexcept;

/// Reject a path that names a Windows device, contains a `..` component, is
/// not under `root`, or is otherwise unsafe to open. `root` must be absolute.
[[nodiscard]] Status validate_child_path(const fs::path& root, const fs::path& candidate);

/// An exclusive, OS-level lock on a file.
///
/// Windows: CreateFileW with dwShareMode == 0 (no sharing at all).
/// POSIX: open + flock(LOCK_EX | LOCK_NB).
///
/// The lock dies with the process, which is exactly the property writer
/// fencing needs: a killed writer cannot leave the store permanently locked.
class ExclusiveFileLock {
 public:
  /// Declared rather than defaulted so that the platform handle type may stay
  /// incomplete here.
  ExclusiveFileLock();
  ~ExclusiveFileLock();

  ExclusiveFileLock(const ExclusiveFileLock&) = delete;
  ExclusiveFileLock& operator=(const ExclusiveFileLock&) = delete;
  ExclusiveFileLock(ExclusiveFileLock&& other) noexcept;
  ExclusiveFileLock& operator=(ExclusiveFileLock&& other) noexcept;

  /// Acquire the lock. Returns LOCK_CONFLICT when another process or another
  /// handle in this process already holds it.
  [[nodiscard]] static Result<ExclusiveFileLock> acquire(const fs::path& path);

  [[nodiscard]] bool held() const noexcept { return handle_ != nullptr; }
  void release() noexcept;

  /// Native handle value, for diagnostics only.
  [[nodiscard]] std::uintptr_t native_handle() const noexcept { return handle_value_; }

 private:
  struct Handle;
  std::unique_ptr<Handle> handle_;
  std::uintptr_t handle_value_ = 0;
};

/// Best-effort description of the last OS error, for error messages.
[[nodiscard]] std::string last_os_error_string();

/// Process id of the current process.
[[nodiscard]] std::uint64_t current_process_id() noexcept;

/// Monotonic nanoseconds since an arbitrary origin, for benchmark timing only.
/// Never used in authoritative accounting.
[[nodiscard]] std::uint64_t monotonic_nanoseconds() noexcept;

}  // namespace summon::capacity_reconciliation::platform
