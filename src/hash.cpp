// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/capacity_reconciliation/hash.hpp"

namespace summon::capacity_reconciliation {
namespace {

// FIPS 180-4 round constants.
constexpr std::uint32_t kK[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

constexpr std::uint32_t rotr(std::uint32_t value, unsigned bits) noexcept {
  return (value >> bits) | (value << (32u - bits));
}

constexpr std::uint32_t big_sigma0(std::uint32_t x) noexcept {
  return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}
constexpr std::uint32_t big_sigma1(std::uint32_t x) noexcept {
  return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}
constexpr std::uint32_t small_sigma0(std::uint32_t x) noexcept {
  return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}
constexpr std::uint32_t small_sigma1(std::uint32_t x) noexcept {
  return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}

constexpr char kHexDigits[] = "0123456789abcdef";

}  // namespace

Sha256::Sha256() noexcept { reset(); }

void Sha256::reset() noexcept {
  state_ = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
            0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  buffer_.fill(0);
  total_bytes_ = 0;
  buffered_ = 0;
}

void Sha256::update(const void* data, std::size_t length) noexcept {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  total_bytes_ += static_cast<std::uint64_t>(length);

  if (buffered_ > 0) {
    const std::size_t need = 64 - buffered_;
    if (length < need) {
      for (std::size_t i = 0; i < length; ++i) {
        buffer_[buffered_ + i] = bytes[i];
      }
      buffered_ += length;
      return;
    }
    for (std::size_t i = 0; i < need; ++i) {
      buffer_[buffered_ + i] = bytes[i];
    }
    compress(buffer_.data());
    buffered_ = 0;
    bytes += need;
    length -= need;
  }

  while (length >= 64) {
    compress(bytes);
    bytes += 64;
    length -= 64;
  }

  for (std::size_t i = 0; i < length; ++i) {
    buffer_[i] = bytes[i];
  }
  buffered_ = length;
}

void Sha256::update(std::string_view text) noexcept { update(text.data(), text.size()); }

void Sha256::compress(const std::uint8_t block[64]) noexcept {
  std::uint32_t w[64];
  for (std::size_t i = 0; i < 16; ++i) {
    w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24u) |
           (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16u) |
           (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8u) |
           static_cast<std::uint32_t>(block[i * 4 + 3]);
  }
  for (std::size_t i = 16; i < 64; ++i) {
    w[i] = small_sigma1(w[i - 2]) + w[i - 7] + small_sigma0(w[i - 15]) + w[i - 16];
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t i = 0; i < 64; ++i) {
    const std::uint32_t t1 = h + big_sigma1(e) + ((e & f) ^ (~e & g)) + kK[i] + w[i];
    const std::uint32_t t2 = big_sigma0(a) + ((a & b) ^ (a & c) ^ (b & c));
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

Digest Sha256::finish() noexcept {
  const std::uint64_t bit_length = total_bytes_ * 8u;

  std::uint8_t pad[72];
  pad[0] = 0x80u;
  for (std::size_t i = 1; i < sizeof(pad); ++i) {
    pad[i] = 0;
  }
  const std::size_t remainder = static_cast<std::size_t>(total_bytes_ % 64u);
  const std::size_t pad_length = (remainder < 56) ? (56 - remainder) : (120 - remainder);

  std::uint8_t length_bytes[8];
  for (std::size_t i = 0; i < 8; ++i) {
    length_bytes[i] = static_cast<std::uint8_t>((bit_length >> ((7u - i) * 8u)) & 0xFFu);
  }

  update(pad, pad_length);
  update(length_bytes, sizeof(length_bytes));

  Digest digest;
  for (std::size_t i = 0; i < 8; ++i) {
    digest.bytes_[i * 4] = static_cast<std::uint8_t>((state_[i] >> 24u) & 0xFFu);
    digest.bytes_[i * 4 + 1] = static_cast<std::uint8_t>((state_[i] >> 16u) & 0xFFu);
    digest.bytes_[i * 4 + 2] = static_cast<std::uint8_t>((state_[i] >> 8u) & 0xFFu);
    digest.bytes_[i * 4 + 3] = static_cast<std::uint8_t>(state_[i] & 0xFFu);
  }
  return digest;
}

Digest Sha256::hash(std::string_view data) noexcept {
  Sha256 ctx;
  ctx.update(data);
  return ctx.finish();
}

Digest Sha256::hash(const void* data, std::size_t length) noexcept {
  Sha256 ctx;
  ctx.update(data, length);
  return ctx.finish();
}

std::string Digest::to_hex() const {
  std::string out;
  out.reserve(64);
  for (std::uint8_t b : bytes_) {
    out.push_back(kHexDigits[(b >> 4u) & 0x0Fu]);
    out.push_back(kHexDigits[b & 0x0Fu]);
  }
  return out;
}

std::optional<Digest> Digest::from_hex(std::string_view text) noexcept {
  if (text.size() != 64) {
    return std::nullopt;
  }
  Digest digest;
  for (std::size_t i = 0; i < 32; ++i) {
    const char hi = text[i * 2];
    const char lo = text[i * 2 + 1];
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
    digest.bytes_[i] = static_cast<std::uint8_t>((h << 4) | l);
  }
  return digest;
}

bool Digest::is_zero() const noexcept {
  for (std::uint8_t b : bytes_) {
    if (b != 0) {
      return false;
    }
  }
  return true;
}

Digest digest_with_domain(std::string_view domain, std::string_view payload) noexcept {
  Sha256 ctx;
  // Length-prefixed domain so that no two (domain, payload) splits can collide.
  auto emit_length = [&ctx](std::uint64_t value) {
    std::uint8_t bytes[8];
    for (std::size_t i = 0; i < 8; ++i) {
      bytes[i] = static_cast<std::uint8_t>((value >> (i * 8u)) & 0xFFu);
    }
    ctx.update(bytes, sizeof(bytes));
  };
  emit_length(static_cast<std::uint64_t>(domain.size()));
  ctx.update(domain);
  emit_length(static_cast<std::uint64_t>(payload.size()));
  ctx.update(payload);
  return ctx.finish();
}

}  // namespace summon::capacity_reconciliation
