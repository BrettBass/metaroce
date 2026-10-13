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

#include <libsoftmetaroce/modifier/DuplicateModifier.h>

namespace libsoftmetaroce {

DuplicateModifier::DuplicateModifier(std::unique_ptr<ModifierConfig> config)
    : Modifier(std::move(config)) {
  auto* dupConfig =
      dynamic_cast<DuplicateModifierConfig*>(&getModifierConfig());
  if (!dupConfig) {
    throw std::runtime_error(
        "DuplicateModifier: config is not a DuplicateModifierConfig");
  }
  delayUs_ = dupConfig->delayUs;
}

ssize_t DuplicateModifier::modifyTxPacket(
    SendPacketFn& sendPacketFn,
    std::unique_ptr<TxPacketWithMetadata> txPacketData) {
  // Store pointer to sendPacketFn for use in timer callback.
  // The caller must ensure sendPacketFn outlives this modifier.
  sendPacketFn_ = &sendPacketFn;

  // Create a copy of the remote address
  auto dst = txPacketData->remoteSocketAddr;

  // Clone the packet for delayed sending. clone() returns a self-contained copy
  // (owns its payload), so it survives the WQE send buffer being freed; the
  // original is sent immediately below with its still-valid payload view.
  auto delayedPacket = txPacketData->packet->clone();

  // Send the original packet immediately
  auto ret = sendPacketFn(dst, std::move(txPacketData->packet), std::nullopt);

  // Schedule a delay send of the cloned packet using stored pointer
  auto sendLater = [this, dst, pkt = std::move(delayedPacket)]() mutable {
    if (sendPacketFn_) {
      (*sendPacketFn_)(dst, std::move(pkt), std::nullopt);
    }
  };

  // Schedule the function to run after the delay
  deadlineQueue_.scheduleAfter(
      std::chrono::microseconds(delayUs_), std::move(sendLater));
  return ret;
}

} // namespace libsoftmetaroce
