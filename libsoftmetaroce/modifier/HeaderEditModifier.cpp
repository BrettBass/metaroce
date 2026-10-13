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

#include <libsoftmetaroce/modifier/HeaderEditModifier.h>

#include <fmt/core.h>
#include <glog/logging.h>
#include <libsoftmetaroce/codec/AckHeader.h>
#include <libsoftmetaroce/codec/BaseHeader.h>
#include <libsoftmetaroce/codec/DataHeader.h>
#include <libsoftmetaroce/codec/Packet.h>
#include <libsoftmetaroce/codec/ReadWriteHeader.h>
#include <libsoftmetaroce/codec/RnrCancelHeader.h>
#include <stdexcept>

namespace libsoftmetaroce {

namespace {
const HeaderEditModifierConfig& asHeaderEditConfig(ModifierConfig& config) {
  const auto* conf = dynamic_cast<const HeaderEditModifierConfig*>(&config);
  if (conf == nullptr) {
    throw std::runtime_error(
        "HeaderEditModifier: config is not a HeaderEditModifierConfig");
  }
  return *conf;
}
} // namespace

HeaderEditModifier::HeaderEditModifier(std::unique_ptr<ModifierConfig> config)
    : Modifier(std::move(config)),
      conf_(asHeaderEditConfig(getModifierConfig())) {}

ssize_t HeaderEditModifier::modifyTxPacket(
    SendPacketFn& sendPacketFn,
    std::unique_ptr<TxPacketWithMetadata> txPacketData) {
  const auto& packet = txPacketData->packet;
  logPacketDetails(packet, txPacketData->remoteSocketAddr, "Tx");

  // Clone the packet based on its type
  std::unique_ptr<Packet> clonedPacket = packet->clone();
  std::string editDescription;

  if (!tryEditPacket(clonedPacket, editDescription)) {
    LOG(ERROR) << fmt::format(
        "HeaderEditModifier: Failed to edit field '{}', sending packet unchanged",
        conf_.fieldName);
    return sendPacketFn(
        txPacketData->remoteSocketAddr,
        std::move(txPacketData->packet),
        std::nullopt);
  }

  VLOG(1) << fmt::format(
      "HeaderEditModifier: Edited packet {}, method={}",
      editDescription,
      conf_.operationName());

  // Send the modified packet and CRC will be computed after serialization
  ssize_t ret = sendPacketFn(
      txPacketData->remoteSocketAddr, std::move(clonedPacket), std::nullopt);

  editedPackets_++;
  VLOG(1) << fmt::format(
      "HeaderEditModifier: Packet sent with edited {}. Total edited: {}",
      conf_.fieldName,
      editedPackets_);
  return ret;
}

void HeaderEditModifier::modifyRxPacket(
    ReceivePacketFn& receivePacketFn,
    std::unique_ptr<IngressPacket> ingressPacket) {
  const auto& packet = ingressPacket->packet;
  logPacketDetails(packet, ingressPacket->sourceSocketAddress, "Rx");

  // Clone the packet based on its type
  std::unique_ptr<Packet> clonedPacket = packet->clone();
  std::string editDescription;

  if (!tryEditPacket(clonedPacket, editDescription)) {
    LOG(ERROR) << fmt::format(
        "HeaderEditModifier: Failed to edit field '{}', receiving packet unchanged",
        conf_.fieldName);
    receivePacketFn(std::move(ingressPacket));
    return;
  }

  VLOG(1) << fmt::format(
      "HeaderEditModifier: Edited RX packet {}, method={}",
      editDescription,
      conf_.operationName());

  // Update the ingressPacket with the modified packet
  ingressPacket->packet = std::move(clonedPacket);
  receivePacketFn(std::move(ingressPacket));

  editedPackets_++;
  VLOG(1) << fmt::format(
      "HeaderEditModifier: RX packet received with edited {}. Total edited: {}",
      conf_.fieldName,
      editedPackets_);
}

bool HeaderEditModifier::tryEditPacket(
    std::unique_ptr<Packet>& packet,
    std::string& editDescription) {
  switch (conf_.headerType) {
    case HeaderEditModifierConfig::HeaderType::Base:
      return editBaseHeader(packet, editDescription);
    case HeaderEditModifierConfig::HeaderType::Data:
      return editDataHeader(packet, editDescription);
    case HeaderEditModifierConfig::HeaderType::ReadWrite:
      return editReadWriteHeader(packet, editDescription);
    case HeaderEditModifierConfig::HeaderType::Ack:
      return editAckHeader(packet, editDescription);
    case HeaderEditModifierConfig::HeaderType::RnrCancel:
      return editRnrCancelHeader(packet, editDescription);
    default:
      LOG(ERROR) << "HeaderEditModifier: Unknown header type";
      return false;
  }
}

bool HeaderEditModifier::editBaseHeader(
    std::unique_ptr<Packet>& packet,
    std::string& editDescription) {
  BaseHeader& baseHeader = packet->getBaseHeader();
  const std::string& fieldName = conf_.fieldName;

  if (fieldName == "opcode") {
    BaseHeader::Opcode before = baseHeader.getOpcode();
    BaseHeader::Opcode after = BaseHeader::Opcode(
        static_cast<uint8_t>(conf_.editedValue(before.value())));
    baseHeader.setOpcode(after);
    editDescription = fmt::format(
        "BaseHeader.opcode: 0x{:02X} -> 0x{:02X}",
        before.value(),
        after.value());
    return true;
  } else if (fieldName == "version") {
    uint8_t before = baseHeader.getVersion();
    uint8_t after = static_cast<uint8_t>(conf_.editedValue(before));

    baseHeader.setVersion(after);
    editDescription =
        fmt::format("BaseHeader.version: 0x{:02X} -> 0x{:02X}", before, after);
    return true;

  } else if (
      fieldName == "flow_id" || fieldName == "flowId" ||
      fieldName == "path_id" || fieldName == "pathId") { // legacy aliases
    uint16_t before = baseHeader.getFlowId();
    uint16_t after = static_cast<uint16_t>(conf_.editedValue(before));

    baseHeader.setFlowId(after);
    editDescription =
        fmt::format("BaseHeader.flowId: 0x{:04X} -> 0x{:04X}", before, after);
    return true;

  } else if (fieldName == "dst_qp" || fieldName == "dstQp") {
    uint32_t before = baseHeader.getDstQp();
    uint32_t after = static_cast<uint32_t>(conf_.editedValue(before));

    baseHeader.setDstQp(after);
    editDescription =
        fmt::format("BaseHeader.dstQp: 0x{:08X} -> 0x{:08X}", before, after);
    return true;

  } else if (fieldName == "src_qp" || fieldName == "srcQp") {
    uint32_t before = baseHeader.getSrcQp();
    uint32_t after = static_cast<uint32_t>(conf_.editedValue(before));

    baseHeader.setSrcQp(after);
    editDescription =
        fmt::format("BaseHeader.srcQp: 0x{:08X} -> 0x{:08X}", before, after);
    return true;

  } else {
    LOG(ERROR) << fmt::format(
        "HeaderEditModifier: Unknown BaseHeader field '{}'. "
        "Supported fields: opcode, version, flowId, dstQp, srcQp",
        fieldName);
    return false;
  }
}

bool HeaderEditModifier::editDataHeader(
    std::unique_ptr<Packet>& packet,
    std::string& editDescription) {
  auto* dataPacket = dynamic_cast<DataPacket*>(packet.get());
  if (!dataPacket) {
    LOG(ERROR) << "HeaderEditModifier: Packet is not a DataPacket";
    return false;
  }

  DataHeader& dataHeader = dataPacket->getDataHeader();
  const std::string& fieldName = conf_.fieldName;

  if (fieldName == "fsn") {
    uint16_t before = dataHeader.getFsn();
    uint16_t after = static_cast<uint16_t>(conf_.editedValue(before));

    dataHeader.setFsn(after);
    editDescription =
        fmt::format("DataHeader.fsn: 0x{:04X} -> 0x{:04X}", before, after);
    return true;

  } else if (fieldName == "una") {
    uint16_t before = dataHeader.getUna();
    uint16_t after = static_cast<uint16_t>(conf_.editedValue(before));

    dataHeader.setUna(after);
    editDescription =
        fmt::format("DataHeader.una: 0x{:04X} -> 0x{:04X}", before, after);
    return true;

  } else if (fieldName == "tag") {
    uint8_t before = dataHeader.getTag();
    uint8_t after = static_cast<uint8_t>(conf_.editedValue(before));

    dataHeader.setTag(after);
    editDescription =
        fmt::format("DataHeader.tag: 0x{:02X} -> 0x{:02X}", before, after);
    return true;

  } else if (
      fieldName == "ack_request" || fieldName == "ackRequest" ||
      fieldName == "flush_ack" || fieldName == "flushAck") { // legacy aliases
    bool before = dataHeader.getAckRequest();
    bool after = static_cast<bool>(conf_.valueToSet.value_or(!before));

    dataHeader.setAckRequest(after);
    editDescription =
        fmt::format("DataHeader.ackRequest: {} -> {}", before, after);
    return true;

  } else if (fieldName == "is_retransmit" || fieldName == "isRetransmit") {
    bool before = dataHeader.getIsRetransmit();
    bool after = static_cast<bool>(conf_.valueToSet.value_or(!before));

    dataHeader.setIsRetransmit(after);
    editDescription =
        fmt::format("DataHeader.isRetransmit: {} -> {}", before, after);
    return true;

  } else if (fieldName == "is_timeout" || fieldName == "isTimeout") {
    bool before = dataHeader.getIsTimeout();
    bool after = static_cast<bool>(conf_.valueToSet.value_or(!before));

    dataHeader.setIsTimeout(after);
    editDescription =
        fmt::format("DataHeader.isTimeout: {} -> {}", before, after);
    return true;

  } else if (fieldName == "posn") {
    uint16_t before = dataHeader.getPosn();
    uint16_t after = static_cast<uint16_t>(conf_.editedValue(before));

    dataHeader.setPosn(after);
    editDescription =
        fmt::format("DataHeader.posn: 0x{:04X} -> 0x{:04X}", before, after);
    return true;

  } else if (fieldName == "csn") {
    uint16_t before = dataHeader.getCsn();
    uint16_t after = static_cast<uint16_t>(conf_.editedValue(before));

    dataHeader.setCsn(after);
    editDescription =
        fmt::format("DataHeader.csn: 0x{:04X} -> 0x{:04X}", before, after);
    return true;

  } else if (fieldName == "msn") {
    uint16_t before = dataHeader.getMsn();
    uint16_t after = static_cast<uint16_t>(conf_.editedValue(before));

    dataHeader.setMsn(after);
    editDescription =
        fmt::format("DataHeader.msn: 0x{:04X} -> 0x{:04X}", before, after);
    return true;

  } else if (fieldName == "rate_hints" || fieldName == "rateHints") {
    uint32_t before = dataHeader.getRateHints();
    uint32_t after = static_cast<uint32_t>(conf_.editedValue(before));

    dataHeader.setRateHints(after);
    editDescription = fmt::format(
        "DataHeader.rateHints: 0x{:08X} -> 0x{:08X}", before, after);
    return true;

  } else if (fieldName == "timestamp") {
    uint32_t before = dataHeader.getTimestamp();
    uint32_t after = static_cast<uint32_t>(conf_.editedValue(before));

    dataHeader.setTimestamp(after);
    editDescription = fmt::format(
        "DataHeader.timestamp: 0x{:08X} -> 0x{:08X}", before, after);
    return true;

  } else {
    LOG(ERROR) << fmt::format(
        "HeaderEditModifier: Unknown DataHeader field '{}'. "
        "Supported fields: fsn, una, tag, ackRequest, isRetransmit, isTimeout, "
        "posn, csn, msn, rateHints, timestamp",
        fieldName);
    return false;
  }
}

bool HeaderEditModifier::editReadWriteHeader(
    std::unique_ptr<Packet>& packet,
    std::string& editDescription) {
  auto* rwPacket = dynamic_cast<ReadWritePacket*>(packet.get());
  if (!rwPacket) {
    LOG(ERROR) << "HeaderEditModifier: Packet is not a ReadWritePacket";
    return false;
  }

  ReadWriteHeader& rwHeader = rwPacket->getReadWriteHeader();
  const std::string& fieldName = conf_.fieldName;

  if (fieldName == "key" || fieldName == "rkey") {
    uint32_t before = rwHeader.getKey();
    uint32_t after = static_cast<uint32_t>(conf_.editedValue(before));

    rwHeader.setKey(after);
    editDescription =
        fmt::format("ReadWriteHeader.key: 0x{:08X} -> 0x{:08X}", before, after);
    return true;

  } else if (
      fieldName == "message_addr" || fieldName == "messageAddr" ||
      fieldName == "address") {
    uint64_t before = rwHeader.getMessageAddr();
    uint64_t after = conf_.editedValue(before);

    rwHeader.setMessageAddr(after);
    editDescription = fmt::format(
        "ReadWriteHeader.messageAddr: 0x{:016X} -> 0x{:016X}", before, after);
    return true;

  } else if (
      fieldName == "message_length" || fieldName == "messageLength" ||
      fieldName == "length") {
    uint32_t before = rwHeader.getMessageLength();
    uint32_t after = static_cast<uint32_t>(conf_.editedValue(before));

    rwHeader.setMessageLength(after);
    editDescription =
        fmt::format("ReadWriteHeader.messageLength: {} -> {}", before, after);
    return true;

  } else {
    LOG(ERROR) << fmt::format(
        "HeaderEditModifier: Unknown ReadWriteHeader field '{}'. "
        "Supported fields: key/rkey, message_addr/messageAddr/address, "
        "message_length/messageLength/length",
        fieldName);
    return false;
  }
}

bool HeaderEditModifier::editAckHeader(
    std::unique_ptr<Packet>& packet,
    std::string& editDescription) {
  auto* ackPacket = dynamic_cast<AckPacket*>(packet.get());
  if (!ackPacket) {
    LOG(ERROR) << "HeaderEditModifier: Packet is not an AckPacket";
    return false;
  }

  AckHeader& ackHeader = ackPacket->getAckHeader();
  const std::string& fieldName = conf_.fieldName;

  if (fieldName == "cnp_ce" || fieldName == "cnpCE") {
    uint8_t before = ackHeader.getCnpCE();
    uint8_t after = static_cast<uint8_t>(conf_.editedValue(before));

    ackHeader.setCnpCE(after);
    editDescription =
        fmt::format("AckHeader.cnpCE: 0x{:02X} -> 0x{:02X}", before, after);
    return true;

  } else if (fieldName == "tag") {
    uint8_t before = ackHeader.getTag();
    uint8_t after = static_cast<uint8_t>(conf_.editedValue(before));

    ackHeader.setTag(after);
    editDescription =
        fmt::format("AckHeader.tag: 0x{:02X} -> 0x{:02X}", before, after);
    return true;

  } else if (fieldName == "cumulative_fsn" || fieldName == "cumulativeFsn") {
    uint16_t before = ackHeader.getCumulativeFsn();
    uint16_t after = static_cast<uint16_t>(conf_.editedValue(before));

    ackHeader.setCumulativeFsn(after);
    editDescription = fmt::format(
        "AckHeader.cumulativeFsn: 0x{:04X} -> 0x{:04X}", before, after);
    return true;

  } else if (fieldName == "max_rcvd_fsn" || fieldName == "maxRcvdFsn") {
    uint16_t before = ackHeader.getMaxRcvdFsn();
    uint16_t after = static_cast<uint16_t>(conf_.editedValue(before));

    ackHeader.setMaxRcvdFsn(after);
    editDescription = fmt::format(
        "AckHeader.maxRcvdFsn: 0x{:04X} -> 0x{:04X}", before, after);
    return true;

  } else if (fieldName == "ack_status" || fieldName == "ackStatus") {
    AckStatus before = ackHeader.getAckStatus();
    AckStatus after;

    if (conf_.valueToSet.has_value()) {
      after = static_cast<AckStatus>(conf_.valueToSet.value());
    } else {
      // For enum types, we can't XOR, so just toggle between ACK and
      // ACK_OUT_OF_ORDER
      after = (before == AckStatus::ACK) ? AckStatus::ACK_OUT_OF_ORDER
                                         : AckStatus::ACK;
    }

    ackHeader.setAckStatus(after);
    editDescription = fmt::format(
        "AckHeader.ackStatus: {} -> {}",
        static_cast<uint8_t>(before),
        static_cast<uint8_t>(after));
    return true;

  } else if (
      fieldName == "cumul_resp_delivered_msn" ||
      fieldName == "cumulRespDeliveredMsn") {
    uint16_t before = ackHeader.getCumulRespDeliveredMsn();
    uint16_t after = static_cast<uint16_t>(conf_.editedValue(before));

    ackHeader.setCumulRespDeliveredMsn(after);
    editDescription = fmt::format(
        "AckHeader.cumulRespDeliveredMsn: 0x{:04X} -> 0x{:04X}", before, after);
    return true;

  } else if (fieldName == "rate_hints" || fieldName == "rateHints") {
    uint32_t before = ackHeader.getRateHints();
    uint32_t after = static_cast<uint32_t>(conf_.editedValue(before));

    ackHeader.setRateHints(after);
    editDescription =
        fmt::format("AckHeader.rateHints: 0x{:08X} -> 0x{:08X}", before, after);
    return true;

  } else if (fieldName == "timestamp") {
    uint32_t before = ackHeader.getTimestamp();
    uint32_t after = static_cast<uint32_t>(conf_.editedValue(before));

    ackHeader.setTimestamp(after);
    editDescription =
        fmt::format("AckHeader.timestamp: 0x{:08X} -> 0x{:08X}", before, after);
    return true;

  } else {
    LOG(ERROR) << fmt::format(
        "HeaderEditModifier: Unknown AckHeader field '{}'. "
        "Supported fields: cnpCE, tag, cumulativeFsn, maxRcvdFsn, ackStatus, "
        "cumulRespDeliveredMsn, rateHints, timestamp",
        fieldName);
    return false;
  }
}

bool HeaderEditModifier::editRnrCancelHeader(
    std::unique_ptr<Packet>& packet,
    std::string& editDescription) {
  auto* rnrCancelPacket = dynamic_cast<RnrCancelPacket*>(packet.get());
  if (!rnrCancelPacket) {
    LOG(ERROR) << "HeaderEditModifier: Packet is not an RnrCancelPacket";
    return false;
  }

  RnrCancelHeader& rnrCancelHeader = rnrCancelPacket->getRnrCancelHeader();
  const std::string& fieldName = conf_.fieldName;

  if (fieldName == "flows_available" || fieldName == "flowsAvailable" ||
      fieldName == "paths_available" ||
      fieldName == "pathsAvailable") { // legacy aliases
    uint8_t before = rnrCancelHeader.getFlowsAvailable();
    uint8_t after = static_cast<uint8_t>(conf_.editedValue(before));

    rnrCancelHeader.setFlowsAvailable(after);
    editDescription = fmt::format(
        "RnrCancelHeader.flowsAvailable: 0x{:02X} -> 0x{:02X}", before, after);
    return true;

  } else if (fieldName == "tag") {
    uint8_t before = rnrCancelHeader.getTag();
    uint8_t after = static_cast<uint8_t>(conf_.editedValue(before));

    rnrCancelHeader.setTag(after);
    editDescription =
        fmt::format("RnrCancelHeader.tag: 0x{:02X} -> 0x{:02X}", before, after);
    return true;

  } else if (fieldName == "posted_csn" || fieldName == "postedCsn") {
    uint16_t before = rnrCancelHeader.getPostedCsn();
    uint16_t after = static_cast<uint16_t>(conf_.editedValue(before));

    rnrCancelHeader.setPostedCsn(after);
    editDescription = fmt::format(
        "RnrCancelHeader.postedCsn: 0x{:04X} -> 0x{:04X}", before, after);
    return true;

  } else if (fieldName == "rate_hints" || fieldName == "rateHints") {
    uint32_t before = rnrCancelHeader.getRateHints();
    uint32_t after = static_cast<uint32_t>(conf_.editedValue(before));

    rnrCancelHeader.setRateHints(after);
    editDescription = fmt::format(
        "RnrCancelHeader.rateHints: 0x{:08X} -> 0x{:08X}", before, after);
    return true;

  } else {
    LOG(ERROR) << fmt::format(
        "HeaderEditModifier: Unknown RnrCancelHeader field '{}'. "
        "Supported fields: flowsAvailable, tag, postedCsn, rateHints",
        fieldName);
    return false;
  }
}

} // namespace libsoftmetaroce
