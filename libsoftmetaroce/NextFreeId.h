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

#include <cstdint>
#include <optional>

namespace libsoftmetaroce {

// The first id from `next` for which inUse is false, wrapping once and
// leaving `next` past it. Empty where every id in [lo, hi] is in use.
template <typename InUse>
std::optional<uint32_t>
nextFreeId(uint32_t& next, uint32_t lo, uint32_t hi, const InUse& inUse) {
  const uint64_t span = static_cast<uint64_t>(hi) - lo + 1;
  for (uint64_t i = 0; i < span; ++i) {
    const uint32_t candidate = next;
    next = (next >= hi) ? lo : next + 1;
    if (!inUse(candidate)) {
      return candidate;
    }
  }
  return std::nullopt;
}

} // namespace libsoftmetaroce
