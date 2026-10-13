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

#include <libsoftmetaroce/Constants.h>
#include <cstdint>

namespace libsoftmetaroce {

// Where one direction of a tag's numbering starts; a tag has two.
struct SequenceNumbers {
  uint16_t nextMsn;
  uint16_t nextCsn;

  [[nodiscard]] static constexpr SequenceNumbers fromPsn(
      uint32_t psn) noexcept {
    return SequenceNumbers{
        .nextMsn = static_cast<uint16_t>(psn & kPsnMsnMask),
        .nextCsn = static_cast<uint16_t>(psn >> kPsnCsnShift)};
  }
};

} // namespace libsoftmetaroce
