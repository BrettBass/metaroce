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
#include <array>
#include <cstddef>
#include <cstdint>
namespace libsoftmetaroce {

/*
 * Array of 32 bit words:
 * [31..0][63..32][95..64][127..96]...
 * 0      1       2       3
 */
template <size_t Size>
class BitVector {
  static constexpr size_t WordSize = sizeof(uint32_t) * 8;
  static_assert(Size % WordSize == 0, "Size must be a multiple of 32.");

 public:
  using BitVectorArray = std::array<uint32_t, Size / WordSize>;
  BitVector() noexcept = default;
  explicit BitVector(BitVectorArray words) noexcept
      : words_(std::move(words)) {}

  bool operator==(const BitVector<Size>& other) const noexcept {
    return words_ == other.words_;
  }

  [[nodiscard]] bool getBit(size_t pos) const noexcept {
    CHECK_LT(pos, Size);
    return (words_[pos / WordSize] >> (pos % WordSize)) & 1;
  }

  /* set the bit at pos to 1 */
  void setBit(size_t pos) noexcept {
    CHECK_LT(pos, Size);
    words_[pos / WordSize] |= (static_cast<uint32_t>(1) << (pos % WordSize));
  }

  /* (un)set the bit at pos to 0 */
  void unsetBit(size_t pos) noexcept {
    CHECK_LT(pos, Size);
    words_[pos / WordSize] &= ~(static_cast<uint32_t>(1) << (pos % WordSize));
  }

  bool operator[](size_t pos) const noexcept {
    return getBit(pos);
  }

  [[nodiscard]] const BitVectorArray& getArray() const noexcept {
    return words_;
  }

  void setArray(BitVectorArray words) noexcept {
    words_ = std::move(words);
  }

 private:
  // array of 32 bit words
  BitVectorArray words_ = {};
};

} // namespace libsoftmetaroce
