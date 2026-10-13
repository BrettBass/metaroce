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

#include <folly/Function.h>
#include <folly/Synchronized.h>
#include <libsoftmetaroce/DeadlineQueue.h>
#include <libsoftmetaroce/modifier/Modifier.h>
#include <deque>
#include <memory>
#include <queue>

namespace libsoftmetaroce {

/*
 * Provides a way to reorder packets based on a distance, which can be
 * represented by time or number of packets.
 */
class ReorderModifier : public Modifier {
 public:
  using DelayedSend = folly::Function<void()>;

  struct PacketReorderComparator {
    bool operator()(
        const std::pair<uint32_t, std::unique_ptr<DelayedSend>>& a,
        const std::pair<uint32_t, std::unique_ptr<DelayedSend>>& b) {
      return a.first > b.first; // Send the packets in the order of their
                                // distance relative to the latest packet sent.
    }
  };

  explicit ReorderModifier(std::unique_ptr<ModifierConfig> config);

  ssize_t modifyTxPacket(
      SendPacketFn& sendPacketFn,
      std::unique_ptr<TxPacketWithMetadata> txPacketData) override;

  void modifyRxPacket(
      ReceivePacketFn& receivePacketFn,
      std::unique_ptr<IngressPacket> ingressPacket) override;

  void onPacketSent() override;
  void onPacketReceived() override;

  [[nodiscard]] bool mayDeferReceive() const noexcept override {
    return actsOnReceive();
  }

  void stopTimers() override {
    deadlineQueue_.stop();
  }

 private:
  // Configuration.
  ReorderModifierConfig::Distance distance_{
      0,
      ReorderModifierConfig::Distance::Unit::TimeUs};

  // Runtime state.
  // Stored pointers to send/receive functions used in delayed work.
  SendPacketFn* sendPacketFn_{nullptr};
  ReceivePacketFn* receivePacketFn_{nullptr};
  // The queue is a min-heap storing packets that are reordered and waiting to
  // be sent. Packet-count distance only: a position in the packet stream is
  // not a deadline, so deadlineQueue_ cannot express it.
  std::priority_queue<
      std::pair<uint32_t, std::unique_ptr<DelayedSend>>,
      std::deque<std::pair<uint32_t, std::unique_ptr<DelayedSend>>>,
      PacketReorderComparator>
      queue;
  folly::Synchronized<uint32_t> totalPacketsSent_{0};
  // The queue is a min-heap storing received packets that are to be reordered
  // and waiting to be received.
  std::priority_queue<
      std::pair<uint32_t, std::unique_ptr<DelayedSend>>,
      std::deque<std::pair<uint32_t, std::unique_ptr<DelayedSend>>>,
      PacketReorderComparator>
      rxQueue_;
  folly::Synchronized<uint32_t> totalPacketsReceived_{0};
  // deadlineQueue_ must be declared last so it is destroyed first, discarding
  // pending work before the members that work touches are torn down.
  DeadlineQueue deadlineQueue_{"lsmrReord"};
};

} // namespace libsoftmetaroce
