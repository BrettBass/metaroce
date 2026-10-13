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

#include <libsoftmetaroce/modifier/MarkEcnModifier.h>

#include <libsoftmetaroce/Ecn.h>

namespace libsoftmetaroce {

ssize_t MarkEcnModifier::modifyTxPacket(
    SendPacketFn& sendPacketFn,
    std::unique_ptr<TxPacketWithMetadata> txPacketData) {
  const auto& packet = txPacketData->packet;
  logPacketDetails(packet, txPacketData->remoteSocketAddr, "Tx");

  packet->setEcnValue(kEcnCE);

  return sendPacketFn(
      txPacketData->remoteSocketAddr,
      std::move(txPacketData->packet),
      std::nullopt);
}

void MarkEcnModifier::modifyRxPacket(
    ReceivePacketFn& receivePacketFn,
    std::unique_ptr<IngressPacket> ingressPacket) {
  const auto& packet = ingressPacket->packet;
  const auto& baseHeader = packet->getBaseHeader();

  if (baseHeader.getOpcode().isData()) {
    logPacketDetails(packet, ingressPacket->sourceSocketAddress, "Rx");

    uint8_t modifiedTos = (ingressPacket->tos & ~kEcnMask) | kEcnCE;
    auto modifiedIngressPacket = std::make_unique<IngressPacket>(IngressPacket{
        std::move(ingressPacket->packet),
        ingressPacket->sourceSocketAddress,
        modifiedTos});
    receivePacketFn(std::move(modifiedIngressPacket));
  } else {
    receivePacketFn(std::move(ingressPacket));
  }
}

} // namespace libsoftmetaroce
