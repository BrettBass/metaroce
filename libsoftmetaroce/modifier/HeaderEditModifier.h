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
#include <string>

namespace libsoftmetaroce {

/**
 * Generalized modifier for editing any metaroce header field with valid CRC.
 * Supports editing fields in BaseHeader, DataHeader, ReadWriteHeader,
 * AckHeader, and RnrCancelHeader.
 */
class HeaderEditModifier : public Modifier {
 public:
  explicit HeaderEditModifier(std::unique_ptr<ModifierConfig> config);

  ssize_t modifyTxPacket(
      SendPacketFn& sendPacketFn,
      std::unique_ptr<TxPacketWithMetadata> txPacketData) override;

  void modifyRxPacket(
      ReceivePacketFn& receivePacketFn,
      std::unique_ptr<IngressPacket> ingressPacket) override;

  void onPacketSent() override {}

 private:
  bool tryEditPacket(
      std::unique_ptr<Packet>& packet,
      std::string& editDescription);
  bool editBaseHeader(
      std::unique_ptr<Packet>& packet,
      std::string& editDescription);
  bool editDataHeader(
      std::unique_ptr<Packet>& packet,
      std::string& editDescription);
  bool editReadWriteHeader(
      std::unique_ptr<Packet>& packet,
      std::string& editDescription);
  bool editAckHeader(
      std::unique_ptr<Packet>& packet,
      std::string& editDescription);
  bool editRnrCancelHeader(
      std::unique_ptr<Packet>& packet,
      std::string& editDescription);

  // The base class owns the config, so this outlives every edit.
  const HeaderEditModifierConfig& conf_;
  uint32_t editedPackets_{0}; // Track number of edited packets
};

} // namespace libsoftmetaroce
