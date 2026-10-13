/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <libsoftmetaroce/codec/BitVector.h>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace libsoftmetaroce {

// A fixed-capacity bitset of N bits (N a multiple of 64) supporting the
// sliding-window operations the TX ack path needs to find newly-resolved
// packets without re-scanning the whole in-flight window:
//   - shifting bits down toward index 0 as snd_una advances and the retired
//     low entries leave the window (shiftDown),
//   - set-difference against another bitset to isolate newly-set bits (andNot),
//   - counting a contiguous run of set bits from index 0 (countTrailingOnes),
//   - population counts over a sub-range (popcountRange),
//   - iterating only the set bits (forEachSetBit).
//
// Bit i lives in word i/64 at bit i%64. In integer terms bit 0 is the least
// significant bit of word 0, so a run of set bits starting at index 0 is the
// integer's count of trailing ones -- hence the method name. The TX path
// indexes in-flight packets by their offset from snd_una, so index 0 is the
// oldest unacked packet.
template <size_t N>
class ShiftableBitset {
  static_assert(N % 64 == 0, "N must be a multiple of 64");
  static constexpr size_t kWords = N / 64;

 public:
  ShiftableBitset() noexcept = default;

  void clear() noexcept {
    words_.fill(0);
  }

  [[nodiscard]] bool test(size_t i) const noexcept {
    return (words_[i >> 6] >> (i & 63)) & 1ULL;
  }
  void set(size_t i) noexcept {
    words_[i >> 6] |= (1ULL << (i & 63));
  }
  void reset(size_t i) noexcept {
    words_[i >> 6] &= ~(1ULL << (i & 63));
  }

  [[nodiscard]] bool any() const noexcept {
    for (auto w : words_) {
      if (w != 0) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] bool operator==(const ShiftableBitset& o) const noexcept {
    return words_ == o.words_;
  }

  // Shift all bits down by k (new bit i is old bit i+k); the low k bits are
  // discarded and the high k bits become 0. Used when snd_una advances by k.
  void shiftDown(size_t k) noexcept {
    if (k >= N) {
      clear();
      return;
    }
    const size_t wordShift = k >> 6;
    const size_t bitShift = k & 63;
    std::array<uint64_t, kWords> out{};
    for (size_t i = 0; i < kWords; ++i) {
      const size_t src = i + wordShift;
      const uint64_t lo = src < kWords ? words_[src] : 0;
      if (bitShift == 0) {
        out[i] = lo;
      } else {
        const uint64_t hi = (src + 1) < kWords ? words_[src + 1] : 0;
        out[i] = (lo >> bitShift) | (hi << (64 - bitShift));
      }
    }
    words_ = out;
  }

  // True if any bit is set in both; short-circuits on the first overlapping
  // word, so detecting overlap costs no temporary and often less than a full &.
  [[nodiscard]] bool intersects(const ShiftableBitset& o) const noexcept {
    for (size_t i = 0; i < kWords; ++i) {
      if (words_[i] & o.words_[i]) {
        return true;
      }
    }
    return false;
  }

  ShiftableBitset& operator|=(const ShiftableBitset& o) noexcept {
    for (size_t i = 0; i < kWords; ++i) {
      words_[i] |= o.words_[i];
    }
    return *this;
  }

  // this &= ~other (set-difference: keep bits set here but not in other).
  void andNot(const ShiftableBitset& o) noexcept {
    for (size_t i = 0; i < kWords; ++i) {
      words_[i] &= ~o.words_[i];
    }
  }

  // Keep bits [0, n); clear bits [n, N).
  void keepLowBits(size_t n) noexcept {
    if (n >= N) {
      return;
    }
    const size_t fullWords = n >> 6;
    const size_t rem = n & 63;
    for (size_t i = fullWords; i < kWords; ++i) {
      if (i == fullWords && rem != 0) {
        words_[i] &= (1ULL << rem) - 1;
      } else {
        words_[i] = 0;
      }
    }
  }

  // Number of consecutive set bits starting at index 0 (stops at the first 0
  // bit or at N). In integer terms, the count of trailing ones.
  [[nodiscard]] size_t countTrailingOnes() const noexcept {
    size_t count = 0;
    for (size_t i = 0; i < kWords; ++i) {
      if (words_[i] == ~0ULL) {
        count += 64;
        continue;
      }
      // ~word has its lowest set bit at the first 0 of word; its trailing-zero
      // count is the number of low set bits of word.
      count += static_cast<size_t>(std::countr_zero(~words_[i]));
      break;
    }
    return count;
  }

  [[nodiscard]] size_t popcount() const noexcept {
    size_t total = 0;
    for (auto w : words_) {
      total += static_cast<size_t>(std::popcount(w));
    }
    return total;
  }

  // Population count over [lo, hi).
  [[nodiscard]] size_t popcountRange(size_t lo, size_t hi) const noexcept {
    if (lo >= hi) {
      return 0;
    }
    size_t total = 0;
    for (size_t i = 0; i < kWords; ++i) {
      const size_t wordLo = i * 64;
      const size_t wordHi = wordLo + 64;
      if (wordHi <= lo || wordLo >= hi) {
        continue;
      }
      uint64_t mask = ~0ULL;
      if (lo > wordLo) {
        mask &= (~0ULL) << (lo - wordLo);
      }
      if (hi < wordHi) {
        mask &= (~0ULL) >> (wordHi - hi);
      }
      total += static_cast<size_t>(std::popcount(words_[i] & mask));
    }
    return total;
  }

  // Invoke fn(i) for each set bit i, in ascending order.
  template <typename Fn>
  void forEachSetBit(Fn&& fn) const {
    for (size_t i = 0; i < kWords; ++i) {
      for (uint64_t t = words_[i]; t != 0; t &= (t - 1)) {
        fn(i * 64 + static_cast<size_t>(std::countr_zero(t)));
      }
    }
  }

  // Build from a BitVector<N> (array of 32-bit, little-endian words).
  [[nodiscard]] static ShiftableBitset fromBitVector(
      const BitVector<N>& bv) noexcept {
    ShiftableBitset r;
    const auto& w32 = bv.getArray();
    for (size_t i = 0; i < kWords; ++i) {
      const uint64_t lo = w32[2 * i];
      const uint64_t hi = w32[2 * i + 1];
      r.words_[i] = lo | (hi << 32);
    }
    return r;
  }

 private:
  std::array<uint64_t, kWords> words_{};
};

} // namespace libsoftmetaroce
