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

#include <type_traits>

namespace libsoftmetaroce {

template <typename T1, typename T2>
constexpr bool beforeOrEqual(T1 a, T2 b) noexcept {
  static_assert(sizeof(T1) == sizeof(T2), "Types must have same width");
  static_assert(std::is_unsigned_v<T1>, "T1 must be unsigned");
  static_assert(std::is_unsigned_v<T2>, "T2 must be unsigned");
  // Need to static_cast to avoid usual arithmetic conversions for types smaller
  // than unsigned int. Doesn't matter if we use T1 or T2 because they're
  // unsigned integers of the same width
  return (static_cast<T1>(a - b - 1) >> (sizeof(T1) * 8 - 1));
}

template <typename T1, typename T2>
constexpr bool before(T1 a, T2 b) noexcept {
  static_assert(sizeof(T1) == sizeof(T2), "Types must have same width");
  static_assert(std::is_unsigned_v<T1>, "T1 must be unsigned");
  static_assert(std::is_unsigned_v<T2>, "T2 must be unsigned");
  // Need to static_cast to avoid usual arithmetic conversions for types smaller
  // than unsigned int. Doesn't matter if we use T1 or T2 because they're
  // unsigned integers of the same width
  return (static_cast<T1>(a - b) >> (sizeof(T1) * 8 - 1));
}

template <typename T1, typename T2>
constexpr bool after(T1 a, T2 b) noexcept {
  return !beforeOrEqual(a, b);
}

template <typename T1, typename T2>
constexpr bool afterOrEqual(T1 a, T2 b) noexcept {
  return !before(a, b);
}

} // namespace libsoftmetaroce
