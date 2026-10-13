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

#include <libsoftmetaroce/modifier/WriteHeaderEditModifier.h>

#include <fmt/core.h>
#include <glog/logging.h>
#include <libsoftmetaroce/codec/BaseHeader.h>
#include <libsoftmetaroce/codec/Packet.h>
#include <stdexcept>

namespace libsoftmetaroce {

namespace {
const WriteHeaderEditModifierConfig& asWriteHeaderEditConfig(
    ModifierConfig& config) {
  const auto* conf =
      dynamic_cast<const WriteHeaderEditModifierConfig*>(&config);
  if (conf == nullptr) {
    throw std::runtime_error(
        "WriteHeaderEditModifier: config is not a WriteHeaderEditModifierConfig");
  }
  return *conf;
}
} // namespace

WriteHeaderEditModifier::WriteHeaderEditModifier(
    std::unique_ptr<ModifierConfig> config)
    : Modifier(std::move(config)),
      conf_(asWriteHeaderEditConfig(getModifierConfig())) {}

ssize_t WriteHeaderEditModifier::modifyTxPacket(
    SendPacketFn& sendPacketFn,
    std::unique_ptr<TxPacketWithMetadata> txPacketData) {
  const auto& packet = txPacketData->packet;
  logPacketDetails(packet, txPacketData->remoteSocketAddr, "Tx");

  const auto& opcode = packet->getBaseHeader().getOpcode();
  if (!opcode.hasRwHdr()) {
    ssize_t ret = sendPacketFn(
        txPacketData->remoteSocketAddr,
        std::move(txPacketData->packet),
        std::nullopt);
    return ret;
  }

  auto clonedPacket = std::unique_ptr<ReadWritePacket>(
      static_cast<ReadWritePacket*>(packet->clone().release()));
  ReadWriteHeader& rwHeader = clonedPacket->getReadWriteHeader();

  std::string target = conf_.target;
  std::optional<uint64_t> valueToSet = conf_.valueToSet;

  std::string editDescription;
  if (target == "rkey") {
    uint32_t originalRkey = rwHeader.getKey();
    uint32_t editedRkey;

    if (valueToSet.has_value()) {
      editedRkey = static_cast<uint32_t>(valueToSet.value());
    } else {
      editedRkey = originalRkey ^ 0xFF;
    }

    rwHeader.setKey(editedRkey);
    editDescription =
        fmt::format("rkey: 0x{:08X} -> 0x{:08X}", originalRkey, editedRkey);

  } else if (target == "address") {
    uint64_t originalAddress = rwHeader.getMessageAddr();
    uint64_t editedAddress;

    if (valueToSet.has_value()) {
      editedAddress = valueToSet.value();
    } else {
      editedAddress = originalAddress ^ 0xFF;
    }

    rwHeader.setMessageAddr(editedAddress);
    editDescription = fmt::format(
        "address: 0x{:016X} -> 0x{:016X}", originalAddress, editedAddress);

  } else {
    LOG(ERROR) << fmt::format(
        "WriteHeaderEditModifier: Unknown target '{}', sending packet unchanged",
        target);
    ssize_t ret = sendPacketFn(
        txPacketData->remoteSocketAddr,
        std::move(txPacketData->packet),
        std::nullopt);
    return ret;
  }

  VLOG(1) << fmt::format(
      "WriteHeaderEditModifier: Edited write packet {}, method={}",
      editDescription,
      valueToSet.has_value() ? "set_value" : "xor_0xFF");

  // Send the modified packet and CRC will be computed after serialization
  ssize_t ret = sendPacketFn(
      txPacketData->remoteSocketAddr, std::move(clonedPacket), std::nullopt);

  editedPackets_++;
  VLOG(1) << fmt::format(
      "WriteHeaderEditModifier: Write packet sent with edited {}. Total edited: {}",
      target,
      editedPackets_);
  return ret;
}

void WriteHeaderEditModifier::modifyRxPacket(
    ReceivePacketFn& receivePacketFn,
    std::unique_ptr<IngressPacket> ingressPacket) {
  const auto& packet = ingressPacket->packet;
  logPacketDetails(packet, ingressPacket->sourceSocketAddress, "Rx");

  const auto& opcode = packet->getBaseHeader().getOpcode();
  if (!opcode.hasRwHdr()) {
    receivePacketFn(std::move(ingressPacket));
    return;
  }

  auto clonedPacket = std::unique_ptr<ReadWritePacket>(
      static_cast<ReadWritePacket*>(packet->clone().release()));
  ReadWriteHeader& rwHeader = clonedPacket->getReadWriteHeader();

  std::string target = conf_.target;
  std::optional<uint64_t> valueToSet = conf_.valueToSet;

  std::string editDescription;
  if (target == "rkey") {
    uint32_t originalRkey = rwHeader.getKey();
    uint32_t editedRkey;

    if (valueToSet.has_value()) {
      editedRkey = static_cast<uint32_t>(valueToSet.value());
    } else {
      editedRkey = originalRkey ^ 0xFF;
    }

    rwHeader.setKey(editedRkey);
    editDescription =
        fmt::format("rkey: 0x{:08X} -> 0x{:08X}", originalRkey, editedRkey);

  } else if (target == "address") {
    uint64_t originalAddress = rwHeader.getMessageAddr();
    uint64_t editedAddress;

    if (valueToSet.has_value()) {
      editedAddress = valueToSet.value();
    } else {
      editedAddress = originalAddress ^ 0xFF;
    }

    rwHeader.setMessageAddr(editedAddress);
    editDescription = fmt::format(
        "address: 0x{:016X} -> 0x{:016X}", originalAddress, editedAddress);

  } else {
    LOG(ERROR) << fmt::format(
        "WriteHeaderEditModifier: Unknown target '{}', receiving packet unchanged",
        target);
    receivePacketFn(std::move(ingressPacket));
    return;
  }

  VLOG(1) << fmt::format(
      "WriteHeaderEditModifier: Edited RX write packet {}, method={}",
      editDescription,
      valueToSet.has_value() ? "set_value" : "xor_0xFF");

  ingressPacket->packet = std::move(clonedPacket);
  receivePacketFn(std::move(ingressPacket));

  editedPackets_++;
  VLOG(1) << fmt::format(
      "WriteHeaderEditModifier: RX write packet received with edited {}. Total edited: {}",
      target,
      editedPackets_);
}

} // namespace libsoftmetaroce
