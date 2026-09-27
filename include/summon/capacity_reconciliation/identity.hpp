// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Strongly typed identities, generations, epochs and counters.
//
// Two identities from different families are different types and do not
// convert into one another, implicitly or explicitly. The only sanctioned
// bridge is the textual form produced by to_string()/parse(), which is
// prefix-checked.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "summon/capacity_reconciliation/status.hpp"

namespace summon::capacity_reconciliation {

/// Tags used to give each identity family its own distinct type.
struct ReconciliationRunIdTag {};
struct EvidenceIdTag {};
struct AttributionIdTag {};
struct ConflictIdTag {};
struct NodeIdTag {};
struct IncarnationIdTag {};

/// A 128-bit identity rendered as `<prefix><32 lowercase hex digits>`.
///
/// The value is generated from the operating system CSPRNG. There is no
/// "nil" identity: a default constructed identity is invalid and `valid()`
/// reports it, so an accidentally defaulted field is detectable rather than
/// silently matching another defaulted field.
template <typename Tag>
class BasicId {
 public:
  using tag_type = Tag;

  constexpr BasicId() noexcept = default;

  constexpr explicit BasicId(std::array<std::uint8_t, 16> bytes) noexcept : bytes_(bytes) {}

  [[nodiscard]] static constexpr std::size_t byte_size() noexcept { return 16; }

  [[nodiscard]] constexpr const std::array<std::uint8_t, 16>& bytes() const noexcept {
    return bytes_;
  }

  [[nodiscard]] constexpr bool valid() const noexcept {
    for (std::uint8_t b : bytes_) {
      if (b != 0) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] std::string to_string() const;

  /// Strict parse. Rejects wrong prefix, wrong length, uppercase hex,
  /// non-hex characters and the all-zero identity.
  [[nodiscard]] static std::optional<BasicId> parse(std::string_view text) noexcept;

  friend constexpr bool operator==(const BasicId& a, const BasicId& b) noexcept {
    return a.bytes_ == b.bytes_;
  }
  friend constexpr bool operator!=(const BasicId& a, const BasicId& b) noexcept {
    return !(a == b);
  }
  friend constexpr bool operator<(const BasicId& a, const BasicId& b) noexcept {
    return a.bytes_ < b.bytes_;
  }

  [[nodiscard]] static BasicId generate();

 private:
  std::array<std::uint8_t, 16> bytes_{};
};

using ReconciliationRunId = BasicId<ReconciliationRunIdTag>;
using EvidenceId = BasicId<EvidenceIdTag>;
using AttributionId = BasicId<AttributionIdTag>;
using ConflictId = BasicId<ConflictIdTag>;
using NodeId = BasicId<NodeIdTag>;
using IncarnationId = BasicId<IncarnationIdTag>;

/// Prefix tokens. Stable; used by to_string and parse.
[[nodiscard]] std::string_view id_prefix(ReconciliationRunIdTag) noexcept;
[[nodiscard]] std::string_view id_prefix(EvidenceIdTag) noexcept;
[[nodiscard]] std::string_view id_prefix(AttributionIdTag) noexcept;
[[nodiscard]] std::string_view id_prefix(ConflictIdTag) noexcept;
[[nodiscard]] std::string_view id_prefix(NodeIdTag) noexcept;
[[nodiscard]] std::string_view id_prefix(IncarnationIdTag) noexcept;

template <typename Tag>
std::string BasicId<Tag>::to_string() const {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string_view prefix = id_prefix(Tag{});
  std::string out;
  out.reserve(prefix.size() + 32);
  out.append(prefix);
  for (std::uint8_t b : bytes_) {
    out.push_back(kHex[(b >> 4u) & 0x0Fu]);
    out.push_back(kHex[b & 0x0Fu]);
  }
  return out;
}

template <typename Tag>
std::optional<BasicId<Tag>> BasicId<Tag>::parse(std::string_view text) noexcept {
  const std::string_view prefix = id_prefix(Tag{});
  if (text.size() != prefix.size() + 32) {
    return std::nullopt;
  }
  if (text.substr(0, prefix.size()) != prefix) {
    return std::nullopt;
  }
  std::array<std::uint8_t, 16> bytes{};
  for (std::size_t i = 0; i < 16; ++i) {
    const char hi = text[prefix.size() + (i * 2)];
    const char lo = text[prefix.size() + (i * 2) + 1];
    auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9') {
        return c - '0';
      }
      if (c >= 'a' && c <= 'f') {
        return (c - 'a') + 10;
      }
      return -1;
    };
    const int h = nibble(hi);
    const int l = nibble(lo);
    if (h < 0 || l < 0) {
      return std::nullopt;
    }
    bytes[i] = static_cast<std::uint8_t>((h << 4) | l);
  }
  BasicId id{bytes};
  if (!id.valid()) {
    return std::nullopt;
  }
  return id;
}

/// Monotonic store generation. Incremented exactly once per committed record.
///
/// Generations order writes. They are not timestamps and carry no notion of
/// wall-clock freshness: freshness is carried by evidence stamps.
class Generation {
 public:
  constexpr Generation() noexcept = default;
  constexpr explicit Generation(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

  /// Checked successor. Refuses to wrap.
  [[nodiscard]] Result<Generation> next() const;

  friend constexpr bool operator==(const Generation& a, const Generation& b) noexcept {
    return a.value_ == b.value_;
  }
  friend constexpr bool operator!=(const Generation& a, const Generation& b) noexcept {
    return a.value_ != b.value_;
  }
  friend constexpr bool operator<(const Generation& a, const Generation& b) noexcept {
    return a.value_ < b.value_;
  }
  friend constexpr bool operator<=(const Generation& a, const Generation& b) noexcept {
    return a.value_ <= b.value_;
  }
  friend constexpr bool operator>(const Generation& a, const Generation& b) noexcept {
    return a.value_ > b.value_;
  }
  friend constexpr bool operator>=(const Generation& a, const Generation& b) noexcept {
    return a.value_ >= b.value_;
  }

 private:
  std::uint64_t value_ = 0;
};

/// Monotonic revision of an upstream object (an asset record, a rack record,
/// an evidence producer's own view of its state).
class Revision {
 public:
  constexpr Revision() noexcept = default;
  constexpr explicit Revision(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

  friend constexpr bool operator==(const Revision& a, const Revision& b) noexcept {
    return a.value_ == b.value_;
  }
  [[nodiscard]] constexpr bool operator<(const Revision& other) const noexcept {
    return value_ < other.value_;
  }

 private:
  std::uint64_t value_ = 0;
};

/// Epoch of the control plane that produced a piece of state. An epoch change
/// invalidates authority granted under the previous epoch.
class Epoch {
 public:
  constexpr Epoch() noexcept = default;
  constexpr explicit Epoch(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

  friend constexpr bool operator==(const Epoch& a, const Epoch& b) noexcept {
    return a.value_ == b.value_;
  }
  friend constexpr bool operator!=(const Epoch& a, const Epoch& b) noexcept {
    return a.value_ != b.value_;
  }
  [[nodiscard]] constexpr bool operator<(const Epoch& other) const noexcept {
    return value_ < other.value_;
  }
  [[nodiscard]] constexpr bool operator>(const Epoch& other) const noexcept {
    return value_ > other.value_;
  }
  [[nodiscard]] constexpr bool operator<=(const Epoch& other) const noexcept {
    return value_ <= other.value_;
  }
  [[nodiscard]] constexpr bool operator>=(const Epoch& other) const noexcept {
    return value_ >= other.value_;
  }

 private:
  std::uint64_t value_ = 0;
};

/// Attempt sequence. Every durable mutation reserves a fresh attempt id, which
/// is recorded in the committed record so a partial write can be attributed.
class AttemptId {
 public:
  constexpr AttemptId() noexcept = default;
  constexpr explicit AttemptId(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

  [[nodiscard]] Result<AttemptId> next() const;

  friend constexpr bool operator==(const AttemptId& a, const AttemptId& b) noexcept {
    return a.value_ == b.value_;
  }
  [[nodiscard]] constexpr bool operator<(const AttemptId& other) const noexcept {
    return value_ < other.value_;
  }

 private:
  std::uint64_t value_ = 0;
};

/// A logical instant in a named clock domain.
///
/// The runtime never reads the wall clock for authoritative accounting. Ticks
/// are supplied by the caller and are compared only within one declared clock
/// domain; mixing domains is refused.
class Tick {
 public:
  constexpr Tick() noexcept = default;
  constexpr explicit Tick(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

  friend constexpr bool operator==(const Tick& a, const Tick& b) noexcept {
    return a.value_ == b.value_;
  }
  friend constexpr bool operator!=(const Tick& a, const Tick& b) noexcept {
    return a.value_ != b.value_;
  }
  [[nodiscard]] constexpr bool operator<(const Tick& other) const noexcept {
    return value_ < other.value_;
  }
  [[nodiscard]] constexpr bool operator<=(const Tick& other) const noexcept {
    return value_ <= other.value_;
  }

 private:
  std::uint64_t value_ = 0;
};

/// Named clock domain of a Tick. Bounded ASCII identifier.
class ClockDomain {
 public:
  ClockDomain() = default;
  explicit ClockDomain(std::string name) : name_(std::move(name)) {}

  [[nodiscard]] static Result<ClockDomain> create(std::string_view name);

  [[nodiscard]] const std::string& name() const noexcept { return name_; }
  [[nodiscard]] bool empty() const noexcept { return name_.empty(); }

  friend bool operator==(const ClockDomain& a, const ClockDomain& b) noexcept {
    return a.name_ == b.name_;
  }
  friend bool operator!=(const ClockDomain& a, const ClockDomain& b) noexcept {
    return !(a == b);
  }

 private:
  std::string name_;
};

/// Free-form, bounded, stable reason / source token.
///
/// Tokens are restricted to `[a-z0-9._:-]` so that they are safe in filenames,
/// JSON and logs, and so that two visually identical tokens cannot differ by an
/// invisible code point.
[[nodiscard]] bool is_valid_token(std::string_view text, std::size_t max_bytes) noexcept;

/// Validate and normalise a bounded token. Rejects empty, over-long, uppercase,
/// non-ASCII and path-significant input.
[[nodiscard]] Result<std::string> make_token(std::string_view text, std::size_t max_bytes,
                                             std::string_view what);

}  // namespace summon::capacity_reconciliation
