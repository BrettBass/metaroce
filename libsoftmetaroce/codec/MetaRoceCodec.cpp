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

#include <libsoftmetaroce/codec/MetaRoceCodec.h>

#include <glog/logging.h>
#include <libsoftmetaroce/Encapsulation.h>
#include <libsoftmetaroce/codec/Packet.h>

namespace {

bool hasBytes(const folly::io::Cursor& cursor, size_t count) {
  return cursor.totalLength() >= count;
}

std::unique_ptr<folly::IOBuf> deepCopyPayloadFromCursor(
    folly::io::Cursor& cursor,
    size_t framedPayloadSize,
    uint8_t padding = 0) {
  if (padding > framedPayloadSize) {
    return nullptr;
  }
  const size_t payloadSize = framedPayloadSize - padding;
  auto buffer = folly::IOBuf::create(payloadSize);
  cursor.pull(buffer->writableData(), payloadSize);
  buffer->append(payloadSize);
  cursor.skip(padding);
  return buffer;
}

} // namespace

namespace libsoftmetaroce {

size_t MetaRoceCodec::getSerializedSize(const Packet& packet) const {
  return packet.getSizeBytes();
}

void MetaRoceCodec::serialize(
    const Packet& packet,
    folly::io::QueueAppender& appender) const {
  packet.serialize(appender);
}

std::unique_ptr<Packet> MetaRoceCodec::parse(
    folly::io::Cursor& cursor,
    const Encapsulation& encapsulation) const {
  if (!hasBytes(cursor, BaseHeader::kBaseHeaderSizeBytes)) {
    return nullptr;
  }
  BaseHeader baseHeader(cursor);
  const auto& opcode = baseHeader.getOpcode();

  if (!opcode.isMetaRoce()) {
    return nullptr;
  }

  if (opcode.isAnAck()) {
    // Bounded to the ack's own bytes, so a trailing mCRC is not read as part
    // of a bit vector.
    folly::io::Cursor ackBytes(cursor, encapsulation.payloadSize(cursor));
    if (!hasBytes(ackBytes, AckHeader::kMainAckHeaderSizeBytes)) {
      return nullptr;
    }
    // An ack's bit vectors may be abbreviated, so its length is not fixed.
    AckHeader ackHeader(ackBytes);
    return std::make_unique<AckPacket>(
        std::move(baseHeader), std::move(ackHeader));
  }
  if (opcode.isRnrCancel()) {
    if (!hasBytes(cursor, RnrCancelHeader::kRnrCancelHeaderSizeBytes)) {
      return nullptr;
    }
    RnrCancelHeader rnrCancelHeader(cursor);
    return std::make_unique<RnrCancelPacket>(
        std::move(baseHeader), std::move(rnrCancelHeader));
  }
  if (opcode.isData()) {
    if (!hasBytes(cursor, DataHeader::kDataHeaderSizeBytes)) {
      return nullptr;
    }
    DataHeader dataHeader(cursor);
    if (opcode.hasRwHdr()) {
      if (!hasBytes(cursor, ReadWriteHeader::kReadWriteHeaderSizeBytes)) {
        return nullptr;
      }
      ReadWriteHeader rwHeader(cursor);
      std::optional<uint32_t> maybeImmData;
      if (opcode.hasImmData()) {
        if (!hasBytes(cursor, sizeof(uint32_t))) {
          return nullptr;
        }
        maybeImmData = cursor.read<uint32_t>();
      }

      const auto payloadSize = encapsulation.payloadSize(cursor);
      auto ownedPayload = deepCopyPayloadFromCursor(
          cursor, payloadSize, dataHeader.getPadding());
      if (!ownedPayload) {
        return nullptr;
      }
      return std::make_unique<ReadWritePacket>(
          std::move(baseHeader),
          std::move(dataHeader),
          std::move(rwHeader),
          std::move(ownedPayload),
          maybeImmData);
    }
    if (opcode.operationType() ==
        BaseHeader::Opcode::OperationType::SEND_IMM_P) {
      if (!hasBytes(cursor, sizeof(uint32_t))) {
        return nullptr;
      }
      const uint32_t immData = cursor.read<uint32_t>();
      const auto payloadSize = encapsulation.payloadSize(cursor);
      auto ownedPayload = deepCopyPayloadFromCursor(
          cursor, payloadSize, dataHeader.getPadding());
      if (!ownedPayload) {
        return nullptr;
      }
      return std::make_unique<DataPacket>(
          std::move(baseHeader),
          std::move(dataHeader),
          std::move(ownedPayload),
          immData);
    }
    if (opcode.hasAtomicHdr()) {
      if (!hasBytes(cursor, AtomicHeader::kAtomicHeaderSizeBytes)) {
        return nullptr;
      }
      AtomicHeader atomicHeader(cursor);
      return std::make_unique<AtomicPacket>(
          std::move(baseHeader),
          std::move(dataHeader),
          std::move(atomicHeader));
    }
    if (opcode.hasAtomicAckHdr()) {
      if (!hasBytes(cursor, AtomicAckHeader::kAtomicAckHeaderSizeBytes)) {
        return nullptr;
      }
      AtomicAckHeader atomicAckHeader(cursor);
      return std::make_unique<AtomicAckPacket>(
          std::move(baseHeader),
          std::move(dataHeader),
          std::move(atomicAckHeader));
    }
    if (opcode.operationType() == BaseHeader::Opcode::OperationType::SEND ||
        opcode.operationType() ==
            BaseHeader::Opcode::OperationType::SEND_LAST ||
        opcode.operationType() ==
            BaseHeader::Opcode::OperationType::READ_RESP) {
      const auto payloadSize = encapsulation.payloadSize(cursor);
      auto ownedPayload = deepCopyPayloadFromCursor(
          cursor, payloadSize, dataHeader.getPadding());
      if (!ownedPayload) {
        return nullptr;
      }
      return std::make_unique<DataPacket>(
          std::move(baseHeader),
          std::move(dataHeader),
          std::move(ownedPayload));
    }

    // The only place opcodes are classified, so tag an undecodable data
    // opcode here and let the responder NAK it.
    const auto payloadSize = encapsulation.payloadSize(cursor);
    auto ownedPayload = deepCopyPayloadFromCursor(cursor, payloadSize);
    return std::make_unique<UnsupportedPacket>(
        std::move(baseHeader), std::move(dataHeader), std::move(ownedPayload));
  }
  if (opcode.isControl()) {
    if (!hasBytes(cursor, ControlHeader::kControlHeaderSizeBytes)) {
      return nullptr;
    }
    ControlHeader controlHeader(cursor);
    if (controlHeader.getType() == ControlPacketType::PING ||
        controlHeader.getType() == ControlPacketType::PING_RESPONSE) {
      if (!hasBytes(cursor, PingHeader::kPingHeaderSizeBytes)) {
        return nullptr;
      }
      PingHeader pingHeader(cursor);
      const auto payloadSize = encapsulation.payloadSize(cursor);
      auto ownedPayload = deepCopyPayloadFromCursor(cursor, payloadSize);
      return std::make_unique<PingPacket>(
          std::move(baseHeader),
          std::move(controlHeader),
          std::move(pingHeader),
          std::move(ownedPayload));
    }

    LOG(ERROR) << "Received control packet with type "
               << static_cast<uint16_t>(controlHeader.getType())
               << " but that is not a supported control packet type";
    return nullptr;
  }

  LOG(ERROR) << "Received opcode " << opcode
             << " but only ack and data packets are expected to be received.";
  return nullptr;
}

} // namespace libsoftmetaroce
