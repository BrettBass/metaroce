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

#include <libsoftmetaroce/SequenceNumbers.h>
#include <cstdint>
#include <deque>

namespace libsoftmetaroce {

// Where a tag stands in the sequence its peer sends. Holds no queue pair
// state, so whether a tag may still be renumbered is its owner's to decide.
class ReceiveSequence {
 public:
  // Delivery starts one msn behind nextMsn, which the peer has yet to send.
  explicit ReceiveSequence(SequenceNumbers numbering);

  // The msn through which delivery has run, with no gap behind it.
  [[nodiscard]] uint16_t cumulativeDelivered() const;

  // The csn through which receives have completed, with no gap behind it.
  [[nodiscard]] uint16_t cumulativeCompletedCsn() const;
  // False where the csn is not the one after the edge; a shared queue's
  // receives take the csns a peer numbered.
  [[nodiscard]] bool tryMarkCsnCompleted(uint16_t csn);

  [[nodiscard]] uint16_t nextCsn() const;
  [[nodiscard]] uint16_t takeNextCsn();

  // Widens the window of messages that can be recorded until msn falls inside
  // it, which a message arriving far ahead of the gap needs.
  void noteInFlight(uint16_t msn);

  // Records msn as delivered and answers how many consecutive deliveries the
  // cumulative msn then ran over, the first being one past where it stood.
  [[nodiscard]] uint16_t markDelivered(uint16_t msn);

 private:
  uint16_t cumulativeDelivered_;
  // Front stands for cumulativeDelivered_ + 1, which has not been delivered.
  std::deque<bool> delivered_{false};
  uint16_t nextCsn_;
  uint16_t cumulativeCompletedCsn_;
};

} // namespace libsoftmetaroce
