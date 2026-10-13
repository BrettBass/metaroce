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
#include <libsoftmetaroce/Tag.h>

#include <libsoftmetaroce/Wqe.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <tuple>

namespace libsoftmetaroce {

// Mirrors TagSet::processWqes for a receive: where the post clears a recorded
// RNR-Cancel request, it returns the csn that RNR-Cancel carries.
inline std::optional<uint16_t>
postReceiveToTag(Tag& tag, std::unique_ptr<ReceiveWqe> wqe, uint32_t mtu) {
  const bool reserved = tag.tryReserveReceiveQueueEntry();
  CHECK(reserved) << "the tag draws from a shared queue, or its receive depth "
                     "is full";
  return tag.processReceiveWqe(std::move(wqe), mtu);
}

// Mirrors TagSet::postWqe: a post claims the tag's depth for its own direction
// before the sender hands the work over. Reaching the post without it returns
// a queue entry the tag never reserved.
//
// The return is whether to signal the sender, which is also false when the
// depth refuses the post: it does not report acceptance.
inline bool postToTag(Tag& tag, std::unique_ptr<Wqe> wqe, uint32_t mtu) {
  const bool isReceive = wqe->getWorkType() == Wqe::WorkType::kReceive;
  const bool reserved = isReceive ? tag.tryReserveReceiveQueueEntry()
                                  : tag.tryReserveSendQueueEntry();
  if (!reserved) {
    return false;
  }
  if (isReceive) {
    std::unique_ptr<ReceiveWqe> receiveWqe(
        static_cast<ReceiveWqe*>(wqe.release()));
    // The discarded return is the csn an RNR-Cancel would carry, which this
    // bool-returning helper has nowhere to report; a case that wants the
    // cancel posts through postReceiveToTag() instead.
    std::ignore = tag.processReceiveWqe(std::move(receiveWqe), mtu);
    return false;
  } else {
    return tag.processTransmitWqe(std::move(wqe));
  }
}

} // namespace libsoftmetaroce
