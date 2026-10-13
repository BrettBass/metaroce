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

#include <libsoftmetaroce/modifier/TruncateModifier.h>

#include <stdexcept>

#include <libsoftmetaroce/codec/Packet.h>

namespace libsoftmetaroce {

TruncateModifier::TruncateModifier(std::unique_ptr<ModifierConfig> config)
    : Modifier(std::move(config)) {
  const auto* conf =
      dynamic_cast<const TruncateModifierConfig*>(&getModifierConfig());
  if (conf == nullptr) {
    throw std::runtime_error(
        "TruncateModifier: config is not a TruncateModifierConfig");
  }
  truncatedPayloadSizeBytes_ = conf->payloadSizeBytes;
}

std::unique_ptr<Packet> TruncateModifier::truncatedClone(
    const std::unique_ptr<Packet>& packet) {
  // Only data packets carry a payload; non-data packets pass through untouched
  // (and are not cloned).
  if (dynamic_cast<const DataPacket*>(packet.get()) == nullptr) {
    return nullptr;
  }
  std::unique_ptr<Packet> cloned = packet->clone();
  dynamic_cast<DataPacket&>(*cloned).truncatePayload(
      truncatedPayloadSizeBytes_);
  return cloned;
}

ssize_t TruncateModifier::modifyTxPacket(
    SendPacketFn& sendPacketFn,
    std::unique_ptr<TxPacketWithMetadata> txPacketData) {
  logPacketDetails(txPacketData->packet, txPacketData->remoteSocketAddr, "Tx");

  std::unique_ptr<Packet> truncated = truncatedClone(txPacketData->packet);
  return sendPacketFn(
      txPacketData->remoteSocketAddr,
      truncated ? std::move(truncated) : std::move(txPacketData->packet),
      std::nullopt);
}

void TruncateModifier::modifyRxPacket(
    ReceivePacketFn& receivePacketFn,
    std::unique_ptr<IngressPacket> ingressPacket) {
  logPacketDetails(
      ingressPacket->packet, ingressPacket->sourceSocketAddress, "Rx");

  std::unique_ptr<Packet> truncated = truncatedClone(ingressPacket->packet);
  if (truncated != nullptr) {
    ingressPacket->packet = std::move(truncated);
  }
  receivePacketFn(std::move(ingressPacket));
}

} // namespace libsoftmetaroce
