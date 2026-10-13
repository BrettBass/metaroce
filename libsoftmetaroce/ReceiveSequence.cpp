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

#include <fmt/format.h>
#include <glog/logging.h>
#include <libsoftmetaroce/ReceiveSequence.h>
#include <libsoftmetaroce/WrappingCompare.h>
#include <algorithm>

namespace libsoftmetaroce {

ReceiveSequence::ReceiveSequence(SequenceNumbers numbering)
    : cumulativeDelivered_(static_cast<uint16_t>(numbering.nextMsn - 1)),
      nextCsn_(numbering.nextCsn),
      cumulativeCompletedCsn_(static_cast<uint16_t>(numbering.nextCsn - 1)) {}

[[nodiscard]] uint16_t ReceiveSequence::cumulativeDelivered() const {
  return cumulativeDelivered_;
}

[[nodiscard]] uint16_t ReceiveSequence::cumulativeCompletedCsn() const {
  return cumulativeCompletedCsn_;
}

bool ReceiveSequence::tryMarkCsnCompleted(uint16_t csn) {
  if (csn != static_cast<uint16_t>(cumulativeCompletedCsn_ + 1)) {
    return false;
  }
  cumulativeCompletedCsn_ = csn;
  return true;
}

[[nodiscard]] uint16_t ReceiveSequence::nextCsn() const {
  return nextCsn_;
}

[[nodiscard]] uint16_t ReceiveSequence::takeNextCsn() {
  return nextCsn_++;
}

void ReceiveSequence::noteInFlight(uint16_t msn) {
  if (after(
          static_cast<uint16_t>(msn - delivered_.size()),
          cumulativeDelivered_)) {
    delivered_.resize(
        std::min<size_t>(
            static_cast<uint16_t>(msn - cumulativeDelivered_) * 2, UINT16_MAX),
        false);
  }
}

[[nodiscard]] uint16_t ReceiveSequence::markDelivered(uint16_t msn) {
  const uint16_t index =
      static_cast<uint16_t>(msn - (cumulativeDelivered_ + 1));
  CHECK_LT(index, delivered_.size()) << fmt::format(
      "delivered index {} >= size {} (msn={}, cumulativeDelivered={})",
      index,
      delivered_.size(),
      msn,
      cumulativeDelivered_);

  delivered_[index] = true;
  uint16_t advanced = 0;
  while (!delivered_.empty() && delivered_.front()) {
    ++cumulativeDelivered_;
    delivered_.pop_front();
    ++advanced;
  }
  return advanced;
}

} // namespace libsoftmetaroce
