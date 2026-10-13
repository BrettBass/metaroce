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

#include <folly/io/Cursor.h>
#include <folly/io/IOBufQueue.h>
#include <gtest/gtest.h>
#include <cstdint>
#include <vector>

#include <libsoftmetaroce/codec/Packet.h>

namespace libsoftmetaroce {
namespace {

constexpr uint32_t kDefaultSrcQp = 2;
constexpr uint32_t kDefaultDstQp = 5;
constexpr uint32_t kDefaultFlowId = 1;
constexpr uint32_t kDefaultFsn = 5;
constexpr uint32_t kDefaultUna = 5;
constexpr uint32_t kDefaultMsn = 1;
constexpr uint32_t kDefaultCsn = 1;
constexpr uint16_t kDefaultPosn = 10;
constexpr uint32_t kDefaultTimestamp = 0;
constexpr uint8_t kDefaultVersion = 0;

BaseHeader createBaseHeader(
    BaseHeader::Opcode::OperationType opType =
        BaseHeader::Opcode::OperationType::SEND) {
  return BaseHeader::BaseHeaderBuilder()
      .setOpcode(BaseHeader::Opcode(opType))
      .setVersion(kDefaultVersion)
      .setFlowId(kDefaultFlowId)
      .setDstQp(kDefaultDstQp)
      .setSrcQp(kDefaultSrcQp)
      .build();
}

DataHeader createDataHeader() {
  return DataHeader::DataHeaderBuilder()
      .setFsn(kDefaultFsn)
      .setUna(kDefaultUna)
      .setAckRequest(true)
      .setPosn(kDefaultPosn)
      .setMsn(kDefaultMsn)
      .setCsn(kDefaultCsn)
      .setTimestamp(kDefaultTimestamp)
      .build();
}

} // namespace

TEST(PacketTest, OwnPayloadDeepCopiesAndSurvivesSourceFree) {
  // Guards the use-after-free where a held packet outlives the WQE buffer.
  std::vector<uint8_t> wqeBuffer = {
      0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};
  const std::vector<uint8_t> original = wqeBuffer;

  DataPacket packet(
      createBaseHeader(),
      createDataHeader(),
      wqeBuffer.data(),
      wqeBuffer.size());
  packet.ownPayload();

  // Simulate the WQE buffer being freed/reused after the packet is held for a
  // delayed send.
  for (auto& byte : wqeBuffer) {
    byte = 0;
  }

  std::vector<uint8_t> delivered(original.size());
  packet.deliverPayload(delivered.data(), delivered.size());
  EXPECT_EQ(delivered, original)
      << "ownPayload must still deliver the original bytes after the source "
         "buffer is overwritten";
}

TEST(PacketTest, CloneIsSelfContainedAfterSourceFree) {
  // clone() must own its payload so a duplicated packet outlives the WQE
  // buffer.
  std::vector<uint8_t> wqeBuffer = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  const std::vector<uint8_t> original = wqeBuffer;

  DataPacket packet(
      createBaseHeader(),
      createDataHeader(),
      wqeBuffer.data(),
      wqeBuffer.size());
  auto cloned = packet.clone();

  for (auto& byte : wqeBuffer) {
    byte = 0;
  }

  // static_cast (type is always DataPacket) + CHECK_NOTNULL: NULLSAFECLANG does
  // not treat gtest's ASSERT_NE as a null guard.
  auto& clonedData = static_cast<DataPacket&>(*CHECK_NOTNULL(cloned.get()));
  std::vector<uint8_t> delivered(original.size());
  clonedData.deliverPayload(delivered.data(), delivered.size());
  EXPECT_EQ(delivered, original)
      << "clone() must deliver the original bytes after the source buffer is "
         "overwritten";
}

TEST(PacketTest, ReadWriteCloneIsSelfContainedAfterSourceFree) {
  // ReadWritePacket::clone() must own its payload the same as DataPacket's.
  std::vector<uint8_t> wqeBuffer = {0x77, 0x88, 0x99, 0xAA};
  const std::vector<uint8_t> original = wqeBuffer;

  ReadWritePacket packet(
      createBaseHeader(BaseHeader::Opcode::OperationType::WRITE),
      createDataHeader(),
      ReadWriteHeader(
          /*messageAddr=*/0,
          /*key=*/0,
          static_cast<uint32_t>(wqeBuffer.size())),
      wqeBuffer.data(),
      wqeBuffer.size());
  auto cloned = packet.clone();

  for (auto& byte : wqeBuffer) {
    byte = 0;
  }

  auto& clonedRw = static_cast<DataPacket&>(*CHECK_NOTNULL(cloned.get()));
  std::vector<uint8_t> delivered(original.size());
  clonedRw.deliverPayload(delivered.data(), delivered.size());
  EXPECT_EQ(delivered, original)
      << "ReadWritePacket::clone must deliver the original bytes after the "
         "source buffer is freed";
}

TEST(PacketTest, PingOwnPayloadSurvivesSourceFree) {
  // PingPacket exposes no payload getter, so verify ownPayload() via serialize:
  // the payload is written last, so it is the trailing bytes of the output.
  // A 4-byte-aligned payload so the survival check compares the trailing bytes
  // directly, independent of the 4-byte padding exercised elsewhere.
  std::vector<uint8_t> wqeBuffer = {0xAB, 0xCD, 0xEF, 0x12};
  const std::vector<uint8_t> original = wqeBuffer;

  PingPacket ping(
      createBaseHeader(),
      ControlHeader(static_cast<uint16_t>(ControlPacketType::PING), 0, 0),
      PingHeader(),
      wqeBuffer.data(),
      wqeBuffer.size());
  ping.ownPayload();

  for (auto& byte : wqeBuffer) {
    byte = 0;
  }

  folly::IOBufQueue queue{folly::IOBufQueue::cacheChainLength()};
  folly::io::QueueAppender appender(&queue, 128);
  ping.serialize(appender);
  auto serialized = queue.move();
  // CHECK_NOTNULL for NULLSAFECLANG (serialize always produces bytes here);
  // coalesce() returns a contiguous ByteRange, avoiding further pointer derefs.
  const auto bytes = CHECK_NOTNULL(serialized.get())->coalesce();
  ASSERT_GE(bytes.size(), original.size());
  std::vector<uint8_t> tail(bytes.end() - original.size(), bytes.end());
  EXPECT_EQ(tail, original)
      << "PingPacket::ownPayload must serialize the original payload bytes "
         "after the source buffer is freed";
}

// Serializes `packet` and returns the raw wire bytes.
std::vector<uint8_t> serializePacket(const Packet& packet) {
  folly::IOBufQueue queue{folly::IOBufQueue::cacheChainLength()};
  folly::io::QueueAppender appender(&queue, 256);
  packet.serialize(appender);
  auto buf = queue.move();
  const auto bytes = CHECK_NOTNULL(buf.get())->coalesce();
  return {bytes.begin(), bytes.end()};
}

std::vector<uint8_t> serializeWrite(const std::vector<uint8_t>& payload) {
  return serializePacket(ReadWritePacket(
      createBaseHeader(BaseHeader::Opcode::OperationType::WRITE),
      createDataHeader(),
      ReadWriteHeader(
          /*messageAddr=*/0, /*key=*/0, static_cast<uint32_t>(payload.size())),
      payload.data(),
      payload.size()));
}

std::vector<uint8_t> serializeSend(const std::vector<uint8_t>& payload) {
  return serializePacket(DataPacket(
      createBaseHeader(BaseHeader::Opcode::OperationType::SEND),
      createDataHeader(),
      payload.data(),
      payload.size()));
}

std::vector<uint8_t> serializePing(const std::vector<uint8_t>& payload) {
  return serializePacket(PingPacket(
      createBaseHeader(),
      ControlHeader(static_cast<uint16_t>(ControlPacketType::PING), 0, 0),
      PingHeader(),
      payload.data(),
      payload.size()));
}

// Checks that `bytes` ends with `payload` followed by exactly `expectedPad`
// zero bytes.
void expectPayloadThenPad(
    const std::vector<uint8_t>& bytes,
    const std::vector<uint8_t>& payload,
    uint32_t expectedPad) {
  std::vector<uint8_t> expectedTail = payload;
  expectedTail.insert(expectedTail.end(), expectedPad, 0x00);
  ASSERT_GE(bytes.size(), expectedTail.size())
      << "serialized packet is shorter than its payload plus padding";
  const std::vector<uint8_t> actualTail(
      bytes.begin() + (bytes.size() - expectedTail.size()), bytes.end());
  EXPECT_EQ(actualTail, expectedTail)
      << "the wire must end with the payload followed by " << expectedPad
      << " zero pad byte(s) to reach a 4-byte boundary";
}

TEST(PacketTest, WriteSendPingPayloadPaddedToFourBytes) {
  // Sub-4-byte payloads gain zero padding to the next 4-byte boundary; already
  // aligned payloads gain none. Every payload-carrying packet type shares one
  // padding path.
  const std::vector<std::pair<uint32_t, uint32_t>> sizeToPad = {
      {1, 3}, {2, 2}, {3, 1}, {4, 0}, {7, 1}, {8, 0}};
  for (const auto [size, expectedPad] : sizeToPad) {
    const std::vector<uint8_t> payload(size, 0xAB);
    expectPayloadThenPad(serializeWrite(payload), payload, expectedPad);
    expectPayloadThenPad(serializeSend(payload), payload, expectedPad);
    expectPayloadThenPad(serializePing(payload), payload, expectedPad);
  }
}

TEST(PacketTest, WriteSendPingGetSizeBytesReflectsPadding) {
  // getSizeBytes() must equal what serialize() actually emits, for every
  // payload-carrying packet type.
  for (const uint32_t size : {1u, 2u, 3u, 4u, 7u, 8u}) {
    const std::vector<uint8_t> payload(size, 0x00);
    ReadWritePacket write(
        createBaseHeader(BaseHeader::Opcode::OperationType::WRITE),
        createDataHeader(),
        ReadWriteHeader(/*messageAddr=*/0, /*key=*/0, size),
        payload.data(),
        size);
    EXPECT_EQ(write.getSizeBytes(), serializePacket(write).size())
        << "WRITE getSizeBytes must equal the serialized length for size="
        << size;
    DataPacket send(
        createBaseHeader(BaseHeader::Opcode::OperationType::SEND),
        createDataHeader(),
        payload.data(),
        size);
    EXPECT_EQ(send.getSizeBytes(), serializePacket(send).size())
        << "SEND getSizeBytes must equal the serialized length for size="
        << size;
    PingPacket ping(
        createBaseHeader(),
        ControlHeader(static_cast<uint16_t>(ControlPacketType::PING), 0, 0),
        PingHeader(),
        payload.data(),
        size);
    EXPECT_EQ(ping.getSizeBytes(), serializePacket(ping).size())
        << "PING getSizeBytes must equal the serialized length for size="
        << size;
  }
}

} // namespace libsoftmetaroce
