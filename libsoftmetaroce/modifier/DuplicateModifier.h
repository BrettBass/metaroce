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

#include <libsoftmetaroce/DeadlineQueue.h>
#include <libsoftmetaroce/modifier/Modifier.h>
#include <memory>

namespace libsoftmetaroce {

/*
 * Provides a way to duplicate packets, sending the original immediately
 * and the duplicate after a specified delay in microseconds.
 */
class DuplicateModifier : public Modifier {
 public:
  explicit DuplicateModifier(std::unique_ptr<ModifierConfig> config);

  ssize_t modifyTxPacket(
      SendPacketFn& sendPacketFn,
      std::unique_ptr<TxPacketWithMetadata> txPacketData) override;

  void modifyRxPacket(
      [[maybe_unused]] ReceivePacketFn& receivePacketFn,
      [[maybe_unused]] std::unique_ptr<IngressPacket> packet) override {}

  void onPacketSent() override {}

  void stopTimers() override {
    deadlineQueue_.stop();
  }

 private:
  // Configuration.
  uint32_t delayUs_;

  // Runtime state.
  SendPacketFn* sendPacketFn_{nullptr};
  // deadlineQueue_ must be declared last so it is destroyed first, discarding
  // pending work before the members that work touches are torn down.
  DeadlineQueue deadlineQueue_{"lsmrDup"};
};

} // namespace libsoftmetaroce
