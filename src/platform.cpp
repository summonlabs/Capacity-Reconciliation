// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Platform primitives.
//
// Windows is the exercised platform (MSVC). The POSIX branch is written to the
// same contract and kept deliberately small, but it is not built or executed on
// this host, so no POSIX behaviour is claimed as proven.

#include "summon/capacity_reconciliation/platform.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <bcrypt.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#endif

namespace summon::capacity_reconciliation::platform {
namespace {

#if defined(_WIN32)

std::string format_win_error(DWORD code) {
  LPWSTR buffer = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPWSTR>(&buffer), 0,
      nullptr);
  std::string out = "windows error " + std::to_string(code);
  if (length != 0 && buffer != nullptr) {
    std::wstring wide(buffer, length);
    // Trim trailing CR/LF, then narrow byte-by-byte. The message is diagnostic
    // only; no product behaviour depends on it.
    while (!wide.empty() && (wide.back() == L'\r' || wide.back() == L'\n' || wide.back() == L' ')) {
      wide.pop_back();
    }
    out += ": ";
    for (wchar_t wc : wide) {
      out.push_back(wc >= 32 && wc < 127 ? static_cast<char>(wc) : '?');
    }
    LocalFree(buffer);
  }
  return out;
}

Error last_error_status(ErrorCode code, std::string what, std::string_view path) {
  return Error{code,
                    std::string(what) + " failed for '" + std::string(path) + "': " +
                        format_win_error(GetLastError()),
                    "os"};
}

#else

std::string format_errno(int value) {
  return std::string(std::strerror(value)) + " (errno " + std::to_string(value) + ")";
}

Error last_error_status(ErrorCode code, std::string what, std::string_view path) {
  return Error{code,
                    std::string(what) + " failed for '" + std::string(path) + "': " +
                        format_errno(errno),
                    "os"};
}

#endif

}  // namespace

#if defined(_WIN32)

struct ExclusiveFileLock::Handle {
  HANDLE file = INVALID_HANDLE_VALUE;
  ~Handle() {
    if (file != INVALID_HANDLE_VALUE) {
      CloseHandle(file);
    }
  }
};

#else

struct ExclusiveFileLock::Handle {
  int fd = -1;
  ~Handle() {
    if (fd >= 0) {
      ::close(fd);
    }
  }
};

#endif

ExclusiveFileLock::ExclusiveFileLock() = default;

ExclusiveFileLock::~ExclusiveFileLock() = default;

ExclusiveFileLock::ExclusiveFileLock(ExclusiveFileLock&& other) noexcept
    : handle_(std::move(other.handle_)), handle_value_(other.handle_value_) {
  other.handle_value_ = 0;
}

ExclusiveFileLock& ExclusiveFileLock::operator=(ExclusiveFileLock&& other) noexcept {
  if (this != &other) {
    release();
    handle_ = std::move(other.handle_);
    handle_value_ = other.handle_value_;
    other.handle_value_ = 0;
  }
  return *this;
}

void ExclusiveFileLock::release() noexcept {
  handle_.reset();
  handle_value_ = 0;
}

Result<ExclusiveFileLock> ExclusiveFileLock::acquire(const fs::path& path) {
  ExclusiveFileLock lock;
#if defined(_WIN32)
  // dwShareMode == 0 means no other handle -- in this process or any other --
  // may open the file at all until this handle closes. That makes the lock
  // exclusive by construction, and the operating system releases it when the
  // process dies, which is exactly the fencing property required.
  HANDLE file = CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) {
      return make_error(ErrorCode::LockConflict,
                        "store lock is held by another writer: " + format_win_error(error),
                        "lock");
    }
    // Access denied is *not* a lock conflict: it means the path is not the kind
    // of object a lock can be taken on, or the caller lacks permission.
    // Reporting it as a conflict would send an operator looking for a
    // nonexistent second writer.
    if (error == ERROR_ACCESS_DENIED) {
      return make_error(ErrorCode::PermissionDenied,
                        "the store lock path cannot be opened for exclusive use: " +
                            format_win_error(error),
                        "lock");
    }
    return last_error_status(ErrorCode::IoFailure, "locking store", path.string());
  }
  lock.handle_ = std::make_unique<Handle>();
  lock.handle_->file = file;
  lock.handle_value_ = reinterpret_cast<std::uintptr_t>(file);
#else
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) {
    return last_error_status(ErrorCode::IoFailure, "opening store lock", path.string());
  }
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    const int error = errno;
    ::close(fd);
    if (error == EWOULDBLOCK || error == EAGAIN) {
      return make_error(ErrorCode::LockConflict,
                        "store lock is held by another writer", "lock");
    }
    return make_error(ErrorCode::IoFailure,
                      "locking store failed: " + format_errno(error), "os");
  }
  lock.handle_ = std::make_unique<Handle>();
  lock.handle_->fd = fd;
  lock.handle_value_ = static_cast<std::uintptr_t>(fd);
#endif
  return lock;
}

Status random_bytes(void* out, std::size_t count) noexcept {
  if (out == nullptr && count != 0) {
    return make_error(ErrorCode::InvalidArgument, "random_bytes requires a destination",
                      "argument");
  }
  if (count == 0) {
    return Status::success();
  }
#if defined(_WIN32)
  const NTSTATUS status =
      BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), static_cast<ULONG>(count),
                      BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (status != 0) {
    return make_error(ErrorCode::Unavailable,
                      "the platform random generator refused to produce entropy (ntstatus " +
                          std::to_string(static_cast<long>(status)) + ")",
                      "entropy");
  }
  return Status::success();
#else
  std::size_t offset = 0;
  auto* bytes = static_cast<unsigned char*>(out);
  const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return last_error_status(ErrorCode::Unavailable, "opening entropy source", "/dev/urandom");
  }
  while (offset < count) {
    const ssize_t got = ::read(fd, bytes + offset, count - offset);
    if (got <= 0) {
      const int error = errno;
      ::close(fd);
      return make_error(ErrorCode::Unavailable,
                        "reading entropy failed: " + format_errno(error), "entropy");
    }
    offset += static_cast<std::size_t>(got);
  }
  ::close(fd);
  return Status::success();
#endif
}

Status ensure_directory(const fs::path& directory) {
  std::error_code ec;
  if (fs::exists(directory, ec)) {
    if (!fs::is_directory(directory, ec)) {
      return make_error(ErrorCode::InvalidArgument,
                        "path exists and is not a directory: '" + directory.string() + "'",
                        "directory");
    }
    return Status::success();
  }
  if (is_reparse_point(directory)) {
    return make_error(ErrorCode::PathRejected,
                      "refusing to treat a reparse point as a store directory: '" +
                          directory.string() + "'",
                      "reparse_point");
  }
  fs::create_directories(directory, ec);
  if (ec) {
    return make_error(ErrorCode::IoFailure,
                      "creating directory '" + directory.string() + "' failed: " + ec.message(),
                      "os");
  }
  return Status::success();
}

Status write_file_durable(const fs::path& path, std::string_view bytes, bool exclusive_create) {
#if defined(_WIN32)
  const DWORD creation = exclusive_create ? CREATE_NEW : CREATE_ALWAYS;
  HANDLE file = CreateFileW(path.wstring().c_str(), GENERIC_WRITE, 0, nullptr, creation,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
      return make_error(ErrorCode::AlreadyExists,
                        "refusing to overwrite existing file '" + path.string() + "'",
                        "exclusive_create");
    }
    return last_error_status(ErrorCode::IoFailure, "creating file", path.string());
  }

  std::size_t written = 0;
  while (written < bytes.size()) {
    const DWORD chunk = static_cast<DWORD>(
        (bytes.size() - written) > 0x10000000u ? 0x10000000u : (bytes.size() - written));
    DWORD chunk_written = 0;
    if (WriteFile(file, bytes.data() + written, chunk, &chunk_written, nullptr) == 0) {
      const DWORD error = GetLastError();
      CloseHandle(file);
      return make_error(ErrorCode::IoFailure,
                        "writing file '" + path.string() + "' failed: " + format_win_error(error),
                        "os");
    }
    written += chunk_written;
  }

  if (FlushFileBuffers(file) == 0) {
    const DWORD error = GetLastError();
    CloseHandle(file);
    return make_error(ErrorCode::IoFailure,
                      "flushing file '" + path.string() + "' to durable storage failed: " +
                          format_win_error(error),
                      "durability");
  }
  if (CloseHandle(file) == 0) {
    return last_error_status(ErrorCode::IoFailure, "closing file", path.string());
  }
  return Status::success();
#else
  const int flags = O_WRONLY | O_CREAT | O_CLOEXEC | (exclusive_create ? O_EXCL : O_TRUNC);
  const int fd = ::open(path.c_str(), flags, 0644);
  if (fd < 0) {
    if (exclusive_create && errno == EEXIST) {
      return make_error(ErrorCode::AlreadyExists,
                        "refusing to overwrite existing file '" + path.string() + "'",
                        "exclusive_create");
    }
    return last_error_status(ErrorCode::IoFailure, "creating file", path.string());
  }
  std::size_t written = 0;
  while (written < bytes.size()) {
    const ssize_t chunk = ::write(fd, bytes.data() + written, bytes.size() - written);
    if (chunk <= 0) {
      const int error = errno;
      ::close(fd);
      return make_error(ErrorCode::IoFailure,
                        "writing file '" + path.string() + "' failed: " + format_errno(error),
                        "os");
    }
    written += static_cast<std::size_t>(chunk);
  }
  if (::fsync(fd) != 0) {
    const int error = errno;
    ::close(fd);
    return make_error(ErrorCode::IoFailure,
                      "flushing file '" + path.string() + "' failed: " + format_errno(error),
                      "durability");
  }
  if (::close(fd) != 0) {
    return last_error_status(ErrorCode::IoFailure, "closing file", path.string());
  }
  return Status::success();
#endif
}

Result<std::string> read_file_bounded(const fs::path& path, std::size_t max_bytes) {
  if (is_reparse_point(path)) {
    return make_error(ErrorCode::PathRejected,
                      "refusing to read through a reparse point: '" + path.string() + "'",
                      "reparse_point");
  }
#if defined(_WIN32)
  // FILE_SHARE_READ only: no writer may open the file while it is being read,
  // and no one may delete or rename it out from under the read.
  HANDLE file = CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                            nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
      return make_error(ErrorCode::NotFound, "file not found: '" + path.string() + "'", "path");
    }
    return last_error_status(ErrorCode::IoFailure, "opening file", path.string());
  }

  LARGE_INTEGER size{};
  if (GetFileSizeEx(file, &size) == 0) {
    const DWORD error = GetLastError();
    CloseHandle(file);
    return make_error(ErrorCode::IoFailure,
                      "sizing file '" + path.string() + "' failed: " + format_win_error(error),
                      "os");
  }
  if (size.QuadPart < 0) {
    CloseHandle(file);
    return make_error(ErrorCode::Corruption,
                      "file reports a negative size: '" + path.string() + "'", "size");
  }
  if (static_cast<std::uint64_t>(size.QuadPart) > static_cast<std::uint64_t>(max_bytes)) {
    CloseHandle(file);
    return make_error(ErrorCode::LimitExceeded,
                      "file '" + path.string() + "' declares " + std::to_string(size.QuadPart) +
                          " bytes, above the limit of " + std::to_string(max_bytes),
                      "declared_length");
  }

  std::string buffer;
  const std::size_t total = static_cast<std::size_t>(size.QuadPart);
  buffer.resize(total);
  std::size_t read_total = 0;
  while (read_total < total) {
    const DWORD chunk =
        static_cast<DWORD>((total - read_total) > 0x10000000u ? 0x10000000u : (total - read_total));
    DWORD chunk_read = 0;
    if (ReadFile(file, buffer.data() + read_total, chunk, &chunk_read, nullptr) == 0) {
      const DWORD error = GetLastError();
      CloseHandle(file);
      return make_error(ErrorCode::IoFailure,
                        "reading file '" + path.string() + "' failed: " + format_win_error(error),
                        "os");
    }
    read_total += chunk_read;
  }
  CloseHandle(file);
  return buffer;
#else
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ENOENT) {
      return make_error(ErrorCode::NotFound, "file not found: '" + path.string() + "'", "path");
    }
    return last_error_status(ErrorCode::IoFailure, "opening file", path.string());
  }
  struct stat info {};
  if (::fstat(fd, &info) != 0) {
    const int error = errno;
    ::close(fd);
    return make_error(ErrorCode::IoFailure,
                      "sizing file '" + path.string() + "' failed: " + format_errno(error), "os");
  }
  if (info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) > max_bytes) {
    ::close(fd);
    return make_error(ErrorCode::LimitExceeded,
                      "file '" + path.string() + "' is larger than the permitted " +
                          std::to_string(max_bytes) + " bytes",
                      "declared_length");
  }
  std::string buffer;
  buffer.resize(static_cast<std::size_t>(info.st_size));
  std::size_t read_total = 0;
  while (read_total < buffer.size()) {
    const ssize_t got = ::read(fd, buffer.data() + read_total, buffer.size() - read_total);
    if (got <= 0) {
      const int error = errno;
      ::close(fd);
      return make_error(ErrorCode::IoFailure,
                        "reading file '" + path.string() + "' failed: " + format_errno(error),
                        "os");
    }
    read_total += static_cast<std::size_t>(got);
  }
  ::close(fd);
  return buffer;
#endif
}

Status atomic_replace(const fs::path& from, const fs::path& to) {
#if defined(_WIN32)
  if (MoveFileExW(from.wstring().c_str(), to.wstring().c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    return last_error_status(ErrorCode::IoFailure,
                             "atomically replacing '" + to.string() + "' from '" + from.string() +
                                 "'",
                             to.string());
  }
  return Status::success();
#else
  if (::rename(from.c_str(), to.c_str()) != 0) {
    return last_error_status(ErrorCode::IoFailure,
                             "atomically replacing '" + to.string() + "'", to.string());
  }
  return sync_directory(to.parent_path());
#endif
}

Status sync_directory(const fs::path& directory) {
#if defined(_WIN32)
  // On Windows a directory cannot be opened through the C++ standard library in
  // a way that yields a flushable handle, and NTFS journals the rename that
  // MOVEFILE_WRITE_THROUGH already pushed to stable storage. Rather than
  // pretend, this function reports success and the README states exactly which
  // durability primitive provides the guarantee on each platform.
  (void)directory;
  return Status::success();
#else
  const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    return last_error_status(ErrorCode::IoFailure, "opening directory", directory.string());
  }
  if (::fsync(fd) != 0) {
    const int error = errno;
    ::close(fd);
    return make_error(ErrorCode::IoFailure,
                      "flushing directory '" + directory.string() + "' failed: " +
                          format_errno(error),
                      "durability");
  }
  ::close(fd);
  return Status::success();
#endif
}

Status remove_file_if_present(const fs::path& path) noexcept {
  std::error_code ec;
  fs::remove(path, ec);
  if (ec) {
    return make_error(ErrorCode::IoFailure,
                      "removing '" + path.string() + "' failed: " + ec.message(), "os");
  }
  return Status::success();
}

bool is_reparse_point(const fs::path& path) noexcept {
#if defined(_WIN32)
  const DWORD attributes = GetFileAttributesW(path.wstring().c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    return false;
  }
  return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
  std::error_code ec;
  return fs::is_symlink(fs::symlink_status(path, ec));
#endif
}

bool is_regular_file_no_reparse(const fs::path& path) noexcept {
#if defined(_WIN32)
  const DWORD attributes = GetFileAttributesW(path.wstring().c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    return false;
  }
  if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    return false;
  }
  if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return false;
  }
  return true;
#else
  struct stat info {};
  if (::lstat(path.c_str(), &info) != 0) {
    return false;
  }
  return S_ISREG(info.st_mode);
#endif
}

Status validate_child_path(const fs::path& root, const fs::path& candidate) {
  if (root.empty() || candidate.empty()) {
    return make_error(ErrorCode::PathRejected, "path validation requires non-empty paths",
                      "path");
  }
  const fs::path normal_root = root.lexically_normal();
  const fs::path normal_candidate = candidate.lexically_normal();

  // A candidate must be absolute and must be lexically under the root. Using
  // lexical containment (not weakly_canonical) keeps the check independent of
  // the current state of the filesystem, so it cannot be defeated by a
  // concurrent rename.
  if (!normal_candidate.is_absolute() || !normal_root.is_absolute()) {
    return make_error(ErrorCode::PathRejected,
                      "store paths must be absolute: '" + candidate.string() + "'", "path");
  }

  auto root_it = normal_root.begin();
  auto cand_it = normal_candidate.begin();
  for (; root_it != normal_root.end(); ++root_it, ++cand_it) {
    if (cand_it == normal_candidate.end()) {
      return make_error(ErrorCode::PathRejected,
                        "path '" + candidate.string() + "' is above the store root '" +
                            normal_root.string() + "'",
                        "path");
    }
    if (*root_it != *cand_it) {
      return make_error(ErrorCode::PathRejected,
                        "path '" + candidate.string() + "' escapes the store root '" +
                            normal_root.string() + "'",
                        "path");
    }
  }

  for (const fs::path& component : normal_candidate) {
    const std::string text = component.string();
    if (text == "..") {
      return make_error(ErrorCode::PathRejected,
                        "path '" + candidate.string() + "' contains a parent component", "path");
    }
  }
  return Status::success();
}

std::string last_os_error_string() {
#if defined(_WIN32)
  return format_win_error(GetLastError());
#else
  return format_errno(errno);
#endif
}

std::uint64_t current_process_id() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

std::uint64_t monotonic_nanoseconds() noexcept {
  using clock = std::chrono::steady_clock;
  static const clock::time_point origin = clock::now();
  const auto delta = clock::now() - origin;
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(delta).count());
}

}  // namespace summon::capacity_reconciliation::platform
