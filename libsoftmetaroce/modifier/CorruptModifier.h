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

#include <libsoftmetaroce/modifier/Modifier.h>
#include <libsoftmetaroce/modifier/ModifierConfig.h>
#include <memory>

namespace libsoftmetaroce {

/**
 * Corrupts packets by modifying the specified byte in the packet.
 *
 * This modifier works by:
 * 1. Serializing the packet to a buffer
 * 2. Corrupting the packet at a specified position (configurable, defaults to
 * offset 0)
 * 3. Either setting the byte to a specific value or XORing with 0xFF
 * (configurable)
 * 4. Creating a simple packet wrapper that serializes the corrupted buffer
 */
class CorruptModifier : public Modifier {
 public:
  explicit CorruptModifier(std::unique_ptr<ModifierConfig> config);

  ssize_t modifyTxPacket(
      SendPacketFn& sendPacketFn,
      std::unique_ptr<TxPacketWithMetadata> txPacketData) override;

  void modifyRxPacket(
      ReceivePacketFn& receivePacketFn,
      std::unique_ptr<IngressPacket> ingressPacket) override {
    receivePacketFn(std::move(ingressPacket));
  }

  void onPacketSent() override {}

 private:
  // The base class owns the config, so this outlives every corruption.
  const CorruptModifierConfig& conf_;
  uint32_t corruptedPackets_{0}; // Track number of corrupted packets
};

} // namespace libsoftmetaroce
