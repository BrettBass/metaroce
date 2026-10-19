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

#include <libsoftmetaroce/SgeList.h>
#include <cstdint>
#include <iterator>

namespace libsoftmetaroce {

inline LocalSges oneSge(void* address, uint32_t length, uint32_t lkey = 0) {
  return LocalSges{{.address = address, .length = length, .lkey = lkey}};
}

template <typename ByteBuffer>
  requires requires(ByteBuffer& buffer) {
    std::data(buffer);
    std::size(buffer);
  }
LocalSges oneSge(ByteBuffer& buffer, uint32_t lkey = 0) {
  static_assert(
      sizeof(*std::data(buffer)) == 1,
      "std::size counts a buffer's bytes only when its elements are bytes");
  return oneSge(
      std::data(buffer), static_cast<uint32_t>(std::size(buffer)), lkey);
}

} // namespace libsoftmetaroce
