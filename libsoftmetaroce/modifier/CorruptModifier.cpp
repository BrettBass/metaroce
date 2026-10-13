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

#include <libsoftmetaroce/modifier/CorruptModifier.h>

#include <fmt/core.h>
#include <folly/io/IOBuf.h>
#include <glog/logging.h>
#include <libsoftmetaroce/Icrc.h>
#include <stdexcept>

namespace libsoftmetaroce {

namespace {

class CorruptedPacket : public Packet {
 public:
  CorruptedPacket(BaseHeader baseHeader, std::unique_ptr<folly::IOBuf> buffer)
      : Packet(std::move(baseHeader)), buffer_(std::move(buffer)) {}

  void serialize(folly::io::QueueAppender& appender) const override {
    if (buffer_) {
      appender.insert(buffer_->clone());
    }
  }

  [[nodiscard]] size_t getSizeBytes() const override {
    return buffer_ ? buffer_->length() : 0;
  }

  std::unique_ptr<Packet> clone() const override {
    return std::make_unique<CorruptedPacket>(
        getBaseHeader(), buffer_ ? buffer_->clone() : nullptr);
  }

 private:
  std::unique_ptr<folly::IOBuf> buffer_;
};

const CorruptModifierConfig& asCorruptConfig(ModifierConfig& config) {
  const auto* conf = dynamic_cast<const CorruptModifierConfig*>(&config);
  if (conf == nullptr) {
    throw std::runtime_error(
        "CorruptModifier: config is not a CorruptModifierConfig");
  }
  return *conf;
}

} // namespace

CorruptModifier::CorruptModifier(std::unique_ptr<ModifierConfig> config)
    : Modifier(std::move(config)),
      conf_(asCorruptConfig(getModifierConfig())) {}

ssize_t CorruptModifier::modifyTxPacket(
    SendPacketFn& sendPacketFn,
    std::unique_ptr<TxPacketWithMetadata> txPacketData) {
  const auto& packet = txPacketData->packet;
  logPacketDetails(packet, txPacketData->remoteSocketAddr, "Tx");

  uint32_t positionToCorrupt = conf_.positionToCorrupt;
  std::optional<uint8_t> valueToSet = conf_.valueToSet;
  bool calculateNewIcrc = conf_.calculateNewIcrc;

  // Serialize the original packet to an IOBuf.
  // writePayload() uses appender.insert() which creates a separate IOBuf
  // node for the payload (zero-copy wrap). We coalesce the chain into a
  // single contiguous buffer so that position checking and byte corruption
  // work across headers + payload.
  auto packetSize = packet->getSizeBytes();
  auto packetBuf = folly::IOBuf::create(packetSize);
  folly::IOBufQueue bufferQueue;
  bufferQueue.append(std::move(packetBuf));
  folly::io::QueueAppender appender(&bufferQueue, /* growth */ 0);
  packet->serialize(appender);
  auto buf = bufferQueue.move();
  if (buf && buf->isChained()) {
    buf->coalesce();
  }

  // Compute ICRC on the original serialized packet
  if (buf && buf->length() > 0) {
    if (positionToCorrupt >= buf->length()) {
      LOG(ERROR) << fmt::format(
          "CorruptModifier: Position to corrupt ({}) is beyond packet size ({}). Skipping corruption.",
          positionToCorrupt,
          buf->length());
      // Send original packet without corruption
      ssize_t ret = sendPacketFn(
          txPacketData->remoteSocketAddr,
          std::move(txPacketData->packet),
          std::nullopt);
      return ret;
    }

    folly::io::Cursor cursor(buf.get());
    uint32_t computedICRC = Icrc::computeICRC(cursor);
    VLOG(1) << fmt::format(
        "CorruptModifier: Computed ICRC on original packet: 0x{:08X}",
        computedICRC);

    // Clone and corrupt the buffer at the specified position
    auto modifiedBuf = buf->clone();
    uint8_t* data = modifiedBuf->writableData();
    uint8_t originalByte = data[positionToCorrupt];
    uint8_t corruptedByte = valueToSet.value_or(originalByte ^ 0xFF);
    data[positionToCorrupt] = corruptedByte;

    VLOG(1) << fmt::format(
        "CorruptModifier: Corrupted packet at offset {}: original_byte=0x{:02X}, corrupted_byte=0x{:02X}, total_size={}, method={}",
        positionToCorrupt,
        originalByte,
        corruptedByte,
        modifiedBuf->length(),
        valueToSet.has_value() ? "set_value" : "xor_0xFF");

    // Build a corrupted packet
    auto corruptedPacket = std::make_unique<CorruptedPacket>(
        packet->getBaseHeader(), std::move(modifiedBuf));
    ssize_t ret = 0;
    if (!calculateNewIcrc) {
      ret = sendPacketFn(
          txPacketData->remoteSocketAddr,
          std::move(corruptedPacket),
          computedICRC);
    } else {
      ret = sendPacketFn(
          txPacketData->remoteSocketAddr,
          std::move(corruptedPacket),
          std::nullopt);
    }

    corruptedPackets_++;
    if (!calculateNewIcrc) {
      VLOG(1) << fmt::format(
          "CorruptModifier: Packet sent with corrupted data and original ICRC 0x{:08X}. Total corrupted: {}",
          computedICRC,
          corruptedPackets_);
    } else {
      VLOG(1) << fmt::format(
          "CorruptModifier: Packet sent with corrupted data and new ICRC. Total corrupted: {}",
          corruptedPackets_);
    }
    return ret;
  } else {
    LOG(ERROR) << "CorruptModifier: Empty buffer, cannot corrupt packet";
    return -1;
  }
}

} // namespace libsoftmetaroce
