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

#include <glog/logging.h>
#include <bit>
#include <cstdint>
#include <vector>

namespace libsoftmetaroce {

// The posns of a message that were sent and came back, waiting to be allocated
// again. A bit each, and the count of them, so the two cannot disagree. The
// bits are held only while a posn is waiting, and cover the posns that have
// gone out.
class RejectedPosnSet {
 public:
  [[nodiscard]] bool contains(uint32_t posn) const noexcept {
    if (bits_.empty()) {
      return false;
    }
    DCHECK_LT(posn >> 6, bits_.size())
        << "posn " << posn << " is past the cursor the bits were sized from";
    return ((bits_[posn >> 6] >> (posn & 63)) & 1ULL) != 0;
  }

  // sentPosnCount is the sender's cursor, which sizes the bits on first use.
  // It cannot have moved while a posn is waiting, so the bits never grow:
  // allocateNextPosn() serves a waiting posn ahead of the cursor.
  void add(uint32_t posn, uint32_t sentPosnCount) noexcept {
    CHECK_LT(posn, sentPosnCount)
        << "posn " << posn
        << " comes back without having gone out, and the "
           "bits reach only as far as the cursor";
    if (bits_.empty()) {
      bits_.assign((sentPosnCount + 63) / 64, 0);
    }
    CHECK_LT(posn >> 6, bits_.size())
        << "posn " << posn << " is past the cursor the bits were sized from";
    if (contains(posn)) {
      return;
    }
    bits_[posn >> 6] |= 1ULL << (posn & 63);
    ++count_;
  }

  void remove(uint32_t posn) noexcept {
    if (!contains(posn)) {
      return;
    }
    bits_[posn >> 6] &= ~(1ULL << (posn & 63));
    if (--count_ == 0) {
      dropBits();
    }
  }

  // The lowest posn waiting, which is the oldest rejected posn still to send.
  [[nodiscard]] uint32_t takeLowest() noexcept {
    CHECK_GT(count_, 0u) << "an empty set has no posn to give";
    for (size_t word = 0; word < bits_.size(); ++word) {
      if (bits_[word] != 0) {
        const uint32_t posn = static_cast<uint32_t>(word) * 64 +
            static_cast<uint32_t>(std::countr_zero(bits_[word]));
        remove(posn);
        return posn;
      }
    }
    LOG(FATAL) << "a count of " << count_ << " matches no posn in the set";
  }

  void clear() noexcept {
    dropBits();
  }

  [[nodiscard]] bool empty() const noexcept {
    return count_ == 0;
  }

  [[nodiscard]] uint32_t count() const noexcept {
    return count_;
  }

 private:
  void dropBits() noexcept {
    std::vector<uint64_t>().swap(bits_);
    count_ = 0;
  }

  std::vector<uint64_t> bits_;
  uint32_t count_{0};
};

} // namespace libsoftmetaroce
