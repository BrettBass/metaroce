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

#include <folly/Synchronized.h>
#include <libsoftmetaroce/DeadlineQueue.h>
#include <libsoftmetaroce/modifier/Modifier.h>
#include <cstdint>
#include <memory>
#include <optional>

namespace libsoftmetaroce {

/*
 * Enforces a minimum inter-packet interval for matching packets.
 * Unlike ReorderModifier (which applies a fixed delay independently),
 * PacingModifier spaces packets relative to each other: the earliest
 * delivery time for the next packet depends on when the previous
 * packet was sent.
 */
class PacingModifier : public Modifier {
 public:
  explicit PacingModifier(std::unique_ptr<ModifierConfig> config);

  ssize_t modifyTxPacket(
      SendPacketFn& sendPacketFn,
      std::unique_ptr<TxPacketWithMetadata> txPacketData) override;

  void modifyRxPacket(
      ReceivePacketFn& receivePacketFn,
      std::unique_ptr<IngressPacket> ingressPacket) override;

  void onPacketSent() override {}

  [[nodiscard]] bool mayDeferReceive() const noexcept override {
    return actsOnReceive();
  }

  void stopTimers() override {
    deadlineQueue_.stop();
  }

 private:
  // Reserves the interval after the returned release time. nullopt when the
  // previous packet's interval has elapsed and this one is not held.
  std::optional<uint64_t> reserveReleaseTime(uint64_t nowNs);

  // Configuration.
  uint32_t intervalUs_{0};

  // Runtime state.
  // On the clock DeadlineQueue arms against, so release times come from here
  // exactly.
  folly::Synchronized<uint64_t> nextSendTimeNs_{0};
  // deadlineQueue_ must be declared last so it is destroyed first, discarding
  // pending work before the members that work touches are torn down.
  DeadlineQueue deadlineQueue_{"lsmrPace"};
};

} // namespace libsoftmetaroce
