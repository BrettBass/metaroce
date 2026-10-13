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

#include <libsoftmetaroce/Wqe.h>

namespace libsoftmetaroce {

struct PacketId {
  Wqe* wqe;
  uint8_t tagNum;
  uint16_t posn;
  uint16_t sizeBytes;
  PacketId(Wqe* wqe, uint8_t tagNum, uint16_t posn, uint16_t sizeBytes) noexcept
      : wqe{wqe}, tagNum{tagNum}, posn{posn}, sizeBytes{sizeBytes} {
    // Can occur in incomplete unit test code
    CHECK(wqe->getMsn().has_value())
        << "May not create a packet id before the wqe has an MSN assigned.";
  }
  // A tracker entry outlives the WQE it holds once that WQE is resolved, so
  // comparing ids would follow a pointer the tracker contract forbids
  // following. Identify a frame by where it sits in the tracker instead.
  bool operator==(const PacketId&) const = delete;
};

} // namespace libsoftmetaroce
