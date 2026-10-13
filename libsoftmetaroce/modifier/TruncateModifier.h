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

#include <cstdint>

#include <libsoftmetaroce/modifier/Modifier.h>
#include <memory>

namespace libsoftmetaroce {

// Shrinks a data packet's payload to the configured size (default none),
// producing a shorter or header-only packet on the wire. Header fields are left
// untouched, and non-data packets pass through unchanged.
class TruncateModifier : public Modifier {
 public:
  explicit TruncateModifier(std::unique_ptr<ModifierConfig> config);

  ssize_t modifyTxPacket(
      SendPacketFn& sendPacketFn,
      std::unique_ptr<TxPacketWithMetadata> txPacketData) override;
  void modifyRxPacket(
      ReceivePacketFn& receivePacketFn,
      std::unique_ptr<IngressPacket> ingressPacket) override;
  void onPacketSent() override {}

 private:
  // Returns a truncated clone of a data packet, or nullptr for a non-data
  // packet (which the caller forwards unchanged, without cloning).
  std::unique_ptr<Packet> truncatedClone(const std::unique_ptr<Packet>& packet);

  uint32_t truncatedPayloadSizeBytes_;
};

} // namespace libsoftmetaroce
