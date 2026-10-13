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
 * Edits write packets by modifying the rkey or remote address fields with valid
 * CRC.
 */
class WriteHeaderEditModifier : public Modifier {
 private:
  static constexpr uint8_t WRITE_OPCODE = 0xC6;
  static constexpr uint8_t WRITE_IMM_OPCODE = 0xC7;

 public:
  explicit WriteHeaderEditModifier(std::unique_ptr<ModifierConfig> config);

  ssize_t modifyTxPacket(
      SendPacketFn& sendPacketFn,
      std::unique_ptr<TxPacketWithMetadata> txPacketData) override;

  void modifyRxPacket(
      ReceivePacketFn& receivePacketFn,
      std::unique_ptr<IngressPacket> ingressPacket) override;

  void onPacketSent() override {}

 private:
  // The base class owns the config, so this outlives every edit.
  const WriteHeaderEditModifierConfig& conf_;
  uint32_t editedPackets_{0}; // Track number of edited packets
};

} // namespace libsoftmetaroce
