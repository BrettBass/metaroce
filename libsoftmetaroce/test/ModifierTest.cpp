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

#include <folly/IPAddress.h>
#include <folly/Random.h>
#include <folly/synchronization/Baton.h>
#include <gtest/gtest.h>
#include <libsoftmetaroce/ClockSource.h>
#include <libsoftmetaroce/modifier/CorruptModifier.h>
#include <libsoftmetaroce/modifier/HeaderEditModifier.h>
#include <libsoftmetaroce/modifier/MarkEcnModifier.h>
#include <libsoftmetaroce/modifier/Modifier.h>
#include <libsoftmetaroce/modifier/PacingModifier.h>
#include <libsoftmetaroce/modifier/ReorderModifier.h>
#include <libsoftmetaroce/modifier/TruncateModifier.h>
#include <libsoftmetaroce/modifier/WriteHeaderEditModifier.h>
#include <libsoftmetaroce/test/TimerDelayProbe.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>

using namespace testing;
namespace libsoftmetaroce {

namespace {
// Test constants
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
constexpr auto* kDefaultRemoteAddrStr = "fc00::1";
constexpr auto* kNonMatchingRemoteAddrStr = "fc00::2";
const auto kDefaultRemoteAddr =
    folly::SocketAddress(kDefaultRemoteAddrStr, 2766);
const auto kNonMatchingRemoteAddr =
    folly::SocketAddress(kNonMatchingRemoteAddrStr, 10000);

// Helper function to create a standard BaseHeader for testing
BaseHeader createBaseHeader(
    BaseHeader::Opcode::OperationType opType =
        BaseHeader::Opcode::OperationType::SEND,
    uint32_t srcQp = kDefaultSrcQp,
    uint32_t dstQp = kDefaultDstQp,
    uint32_t flowId = kDefaultFlowId) {
  return BaseHeader::BaseHeaderBuilder()
      .setOpcode(BaseHeader::Opcode(opType))
      .setVersion(kDefaultVersion)
      .setFlowId(flowId)
      .setDstQp(dstQp)
      .setSrcQp(srcQp)
      .build();
}

// Helper function to create a standard DataHeader for testing
DataHeader createDataHeader(
    uint32_t fsn = kDefaultFsn,
    uint32_t msn = kDefaultMsn,
    uint16_t posn = kDefaultPosn) {
  return DataHeader::DataHeaderBuilder()
      .setFsn(fsn)
      .setUna(kDefaultUna)
      .setAckRequest(true)
      .setPosn(posn)
      .setMsn(msn)
      .setCsn(kDefaultCsn)
      .setTimestamp(kDefaultTimestamp)
      .build();
}

// Helper function to create a standard AckHeader for testing
AckHeader createAckHeader(
    uint32_t cumulativeFsn,
    const BitVector<AckHeader::kAckTrackerBits>& fsnBitVector) {
  return AckHeader::AckHeaderBuilder()
      .setCumulativeFsn(cumulativeFsn)
      .setMaxRcvdFsn(4)
      .setTimestamp(kDefaultTimestamp)
      .setAckStatus(AckStatus::ACK_OUT_OF_ORDER)
      .setFsnBitVector(fsnBitVector)
      .setCumulRespDeliveredMsn(2)
      .build();
}

// Helper function to create a standard data packet filter
ModifierConfig::ModifierFilter createDataPacketFilter(
    ModifierConfig::ModifierFilter::Direction direction =
        ModifierConfig::ModifierFilter::Direction::Tx,
    folly::F14FastMap<std::string, FieldConditions> fields = {
        {"src_qp", FieldConditions(FieldCondition(kDefaultSrcQp))},
        {"msn", FieldConditions(FieldCondition(kDefaultMsn))},
        {"fsn", FieldConditions(FieldCondition(kDefaultFsn))},
        {"path_id", FieldConditions(FieldCondition(kDefaultFlowId))}}) {
  return ModifierConfig::ModifierFilter(
      direction,
      ModifierConfig::ModifierFilter::PacketType::Data,
      std::move(fields));
}

// Helper to create a counting SendPacketFn
SendPacketFn createCountingSendFn(int& counter) {
  return [&counter](
             const folly::SocketAddress& /*addr*/,
             std::unique_ptr<Packet> /*packet*/,
             std::optional<uint32_t> /*originalIcrc*/) {
    counter++;
    return 1L;
  };
}

// Helper to create an IngressPacket with a DataPacket having the given FSN
std::unique_ptr<IngressPacket> createIngressDataPacket(uint32_t fsn) {
  return std::make_unique<IngressPacket>(IngressPacket{
      std::make_unique<DataPacket>(
          createBaseHeader(), createDataHeader(fsn), nullptr, 0),
      folly::SocketAddress("fc00::1", 10000),
      0});
}
// Helper to create a ReceivePacketFn that records delivered Data packet FSNs
ReceivePacketFn createDataFsnTracker(std::vector<uint32_t>& deliveredFsns) {
  return [&deliveredFsns](std::unique_ptr<IngressPacket> pkt) {
    auto* dataPkt = dynamic_cast<DataPacket*>(pkt->packet.get());
    if (dataPkt) {
      deliveredFsns.push_back(dataPkt->getDataHeader().getFsn());
    }
  };
}

} // namespace

TEST(ModifierConfigTest, ParseMalformedJson) {
  /*  (1) Type is not specified
   *  (2) Unknown value for filter.direction
   *  (3) Unknown value for filter.packet_type
   *  (4) Extra trailing "," at the end of filter fields
   */
  const auto malformedJsonStr = R"({
        "name": "test_modifier",
        "filter": {
            "direction": "mx",
            "packet_type": "control",
            "fields": {"fsn": 2, "path_id": 1},
        },
        "modification_times": 5,
    })";

  EXPECT_THROW(
      (void)ModifierConfig::buildModifierConfigFromJson(
          folly::parseJson(malformedJsonStr)),
      std::runtime_error);

  const auto jsonStr = R"({
        "name": "test_modifier",
        "type": "drop",
        "filter": {
            "direction": "tx",
            "packet_type": "data",
            "fields": {"fsn": 2, "path_id": 1}
        },
        "modification_times": 5
    })";

  const auto modifierConfig =
      ModifierConfig::buildModifierConfigFromJson(folly::parseJson(jsonStr));
  ASSERT_TRUE(modifierConfig);
  const auto* rawDropModifierConfig =
      dynamic_cast<DropModifierConfig*>(modifierConfig.get());
  ASSERT_NE(rawDropModifierConfig, nullptr);
  EXPECT_EQ(modifierConfig->name, "test_modifier");
  EXPECT_EQ(modifierConfig->type, ModifierConfig::ModifierType::Drop);
  EXPECT_DOUBLE_EQ(rawDropModifierConfig->filter.randomPercent, 100.0);
  EXPECT_EQ(
      modifierConfig->filter.direction,
      ModifierConfig::ModifierFilter::Direction::Tx);
  EXPECT_EQ(modifierConfig->filter.fields.size(), 2);

  const auto& filterFields = modifierConfig->filter.fields;
  EXPECT_EQ(filterFields.at("fsn").value(), 2);
  EXPECT_EQ(filterFields.at("path_id").value(), 1);

  EXPECT_EQ(modifierConfig->modificationTimes, std::optional<uint32_t>(5));
}

TEST(ModifierConfigTest, ParseRandomPercent) {
  const auto jsonStr = R"({
        "name": "test_modifier",
        "type": "duplicate",
        "filter": {
            "direction": "tx",
            "packet_type": "data",
            "random_percent": 12.5
        },
        "delay_us": 1
    })";

  const auto modifierConfig =
      ModifierConfig::buildModifierConfigFromJson(folly::parseJson(jsonStr));
  ASSERT_TRUE(modifierConfig);

  EXPECT_EQ(modifierConfig->type, ModifierConfig::ModifierType::Duplicate);
  EXPECT_DOUBLE_EQ(modifierConfig->filter.randomPercent, 12.5);
}

TEST(ModifierConfigTest, RejectInvalidRandomPercent) {
  const auto zeroJsonStr = R"({
        "name": "test_modifier",
        "type": "drop",
        "filter": {
            "direction": "tx",
            "packet_type": "data",
            "random_percent": 0
        }
    })";
  const auto negativeJsonStr = R"({
        "name": "test_modifier",
        "type": "drop",
        "filter": {
            "direction": "tx",
            "packet_type": "data",
            "random_percent": -1
        }
    })";
  const auto tooLargeJsonStr = R"({
        "name": "test_modifier",
        "type": "drop",
        "filter": {
            "direction": "tx",
            "packet_type": "data",
            "random_percent": 101
        }
    })";
  const auto stringJsonStr = R"({
        "name": "test_modifier",
        "type": "drop",
        "filter": {
            "direction": "tx",
            "packet_type": "data",
            "random_percent": "10"
        }
    })";

  EXPECT_THROW(
      (void)ModifierConfig::buildModifierConfigFromJson(
          folly::parseJson(zeroJsonStr)),
      std::out_of_range);
  EXPECT_THROW(
      (void)ModifierConfig::buildModifierConfigFromJson(
          folly::parseJson(negativeJsonStr)),
      std::out_of_range);
  EXPECT_THROW(
      (void)ModifierConfig::buildModifierConfigFromJson(
          folly::parseJson(tooLargeJsonStr)),
      std::out_of_range);
  EXPECT_THROW(
      (void)ModifierConfig::buildModifierConfigFromJson(
          folly::parseJson(stringJsonStr)),
      std::runtime_error);
}

TEST(ModifierConfigTest, ParseJsonRxAckDropModifier) {
  const auto incorrectJsonStr = R"({
        "name": "test_modifier",
        "type": "Drop",
        "filter": {
            "direction": "Rx",
            "packet_type": "Ack",
            "fields": {"fsn_bit_vector": ["0x0A00", "0x0", "0x0"]}
        },
        "modification_times": 5
    })";

  // Provided 3 elements in the fsn_bit_vector field, but expected 8
  EXPECT_THROW(
      (void)ModifierConfig::buildModifierConfigFromJson(
          folly::parseJson(incorrectJsonStr)),
      std::runtime_error);

  const auto jsonStr = R"({
        "name": "test_modifier",
        "type": "Drop",
        "filter": {
            "direction": "Rx",
            "packet_type": "Ack",
            "fields": {"fsn_bit_vector": ["0x0A00", "0x0", "0x0", "0x0", "0x0", "0x0", "0x0", "0x0"]}
        },
        "modification_times": 5
    })";

  const auto modifierConfig =
      ModifierConfig::buildModifierConfigFromJson(folly::parseJson(jsonStr));
  ASSERT_TRUE(modifierConfig);
  EXPECT_EQ(modifierConfig->name, "test_modifier");
  EXPECT_EQ(modifierConfig->type, ModifierConfig::ModifierType::Drop);
  EXPECT_EQ(modifierConfig->modificationTimes, std::optional<uint32_t>(5));
  EXPECT_EQ(
      modifierConfig->filter.direction,
      ModifierConfig::ModifierFilter::Direction::Rx);
  EXPECT_EQ(
      modifierConfig->filter.packetType,
      ModifierConfig::ModifierFilter::PacketType::Ack);

  const auto& filterFields = modifierConfig->filter.fields;
  EXPECT_EQ(filterFields.at("fsn_bit_vector_0").value(), 0x0A00);
  EXPECT_EQ(filterFields.at("fsn_bit_vector_1").value(), 0x0);
  EXPECT_EQ(filterFields.at("fsn_bit_vector_2").value(), 0x0);
  EXPECT_EQ(filterFields.at("fsn_bit_vector_3").value(), 0x0);
}

TEST(ModifierConfigTest, ParseJsonReorderModifier) {
  const auto jsonStr = R"({
        "name": "reorder_modifier",
        "type": "Reorder",
        "filter": {
            "direction": "Rx",
            "packet_type": "Ack"
        },
        "modification_times": 5,
        "distance": {
            "unit": "time_us",
            "value": 100
        }
    })";

  const auto modifierConfig =
      ModifierConfig::buildModifierConfigFromJson(folly::parseJson(jsonStr));
  const auto rawReorderModifierConfig =
      dynamic_cast<ReorderModifierConfig*>(modifierConfig.get());

  EXPECT_EQ(rawReorderModifierConfig->name, "reorder_modifier");
  EXPECT_EQ(
      rawReorderModifierConfig->type, ModifierConfig::ModifierType::Reorder);
  EXPECT_EQ(
      rawReorderModifierConfig->modificationTimes, std::optional<uint32_t>(5));

  EXPECT_EQ(
      rawReorderModifierConfig->distance,
      ReorderModifierConfig::Distance(
          100, ReorderModifierConfig::Distance::Unit::TimeUs));
  EXPECT_EQ(
      rawReorderModifierConfig->filter.direction,
      ModifierConfig::ModifierFilter::Direction::Rx);
  EXPECT_EQ(
      rawReorderModifierConfig->filter.packetType,
      ModifierConfig::ModifierFilter::PacketType::Ack);
}

TEST(ModifierConfigTest, TestDataPacketMatchesFilter) {
  auto filter = createDataPacketFilter();
  DropModifierConfig config(filter, 1);

  const auto baseHeader = createBaseHeader();
  const auto dataHeader = createDataHeader();
  const auto dataPacket = DataPacket(baseHeader, dataHeader, nullptr, 0);

  EXPECT_TRUE(config.matchesFilter(
      dataPacket,
      ModifierConfig::ModifierFilter::Direction::Tx,
      kDefaultRemoteAddr));
}

TEST(ModifierConfigTest, TestDataPacketDoesNotMatchFilter) {
  auto filter = createDataPacketFilter(
      ModifierConfig::ModifierFilter::Direction::Tx,
      {{"src_qp", FieldConditions(FieldCondition(2))},
       {"msn", FieldConditions(FieldCondition(1))},
       {"fsn", FieldConditions(FieldCondition(5))},
       {"flowId", FieldConditions(FieldCondition(1))}});
  DropModifierConfig config(filter, 1);

  const auto baseHeader = createBaseHeader();
  const auto dataHeader = createDataHeader(6); // fsn != 5
  const auto dataPacket = DataPacket(baseHeader, dataHeader, nullptr, 0);

  EXPECT_FALSE(config.matchesFilter(
      dataPacket,
      ModifierConfig::ModifierFilter::Direction::Tx,
      kDefaultRemoteAddr));
}

TEST(ModifierConfigTest, TestAckPacketMatchFilter) {
  ModifierConfig::ModifierFilter filter(
      ModifierConfig::ModifierFilter::Direction::Rx,
      ModifierConfig::ModifierFilter::PacketType::Ack,
      {{"dst_qp", FieldConditions(FieldCondition(kDefaultDstQp))},
       {"cumulative_fsn", FieldConditions(FieldCondition(1))},
       {"flowId", FieldConditions(FieldCondition(1))}});
  DropModifierConfig config(filter, 1);

  BitVector<AckHeader::kAckTrackerBits>::BitVectorArray words;
  for (int i = 0; i < 8; i++) {
    words[i] = folly::Random::rand32(std::numeric_limits<uint32_t>::max());
    config.filter.fields["fsn_bit_vector_" + std::to_string(i)] =
        FieldConditions(FieldCondition(words[i]));
  }

  const auto baseHeader = createBaseHeader(
      BaseHeader::Opcode::OperationType::SEL_ACK,
      kDefaultSrcQp,
      kDefaultDstQp,
      kDefaultFlowId);

  BitVector<AckHeader::kAckTrackerBits> fsnBitVector;
  fsnBitVector.setArray(words);
  const auto ackHeader = createAckHeader(1, fsnBitVector);
  const auto ackPacket = AckPacket(baseHeader, ackHeader);

  EXPECT_TRUE(config.matchesFilter(
      ackPacket,
      ModifierConfig::ModifierFilter::Direction::Rx,
      kDefaultRemoteAddr));
}

TEST(ModifierTest, DropModifierWithCount) {
  constexpr int kExpectedModificationTimes = 5;
  auto filter = createDataPacketFilter(
      ModifierConfig::ModifierFilter::Direction::Tx,
      {{"src_qp", FieldConditions(FieldCondition(2))},
       {"msn", FieldConditions(FieldCondition(1))},
       {"fsn", FieldConditions(FieldCondition(5))},
       {"flowId", FieldConditions(FieldCondition(1))}});
  auto config =
      std::make_unique<DropModifierConfig>(filter, kExpectedModificationTimes);

  const auto baseHeader = createBaseHeader();
  const auto dataHeader = createDataHeader();

  EXPECT_TRUE(config->matchesFilter(
      DataPacket(baseHeader, dataHeader, nullptr, 0),
      ModifierConfig::ModifierFilter::Direction::Tx,
      kDefaultRemoteAddr));

  auto modifier = DropModifier(std::move(config));
  int timesNotModified = 0;
  auto fn = createCountingSendFn(timesNotModified);

  // Packet should be dropped kExpectedModificationTimes times
  for (int i = 0; i < kExpectedModificationTimes; i++) {
    modifier.modifyTxPacketIfMatches(
        fn,
        std::make_unique<Modifier::TxPacketWithMetadata>(
            kDefaultRemoteAddr,
            std::make_unique<DataPacket>(baseHeader, dataHeader, nullptr, 0)));
    EXPECT_EQ(timesNotModified, 0);
  }

  // After modification limit, packet should be sent
  modifier.modifyTxPacketIfMatches(
      fn,
      std::make_unique<Modifier::TxPacketWithMetadata>(
          kDefaultRemoteAddr,
          std::make_unique<DataPacket>(baseHeader, dataHeader, nullptr, 0)));
  EXPECT_EQ(timesNotModified, 1);
}

// modificationTimes is a global cap when Modifiers share appliedCount.
TEST(ModifierTest, GlobalCapAcrossSharedAppliedCount) {
  constexpr int kNumFlows = 4;
  constexpr int kGlobalCap = 5;
  constexpr int kPacketsPerFlow = 3;
  auto filter = createDataPacketFilter();
  const auto baseHeader = createBaseHeader();
  const auto dataHeader = createDataHeader();

  auto sharedCounter = std::make_shared<std::atomic<uint32_t>>(0);

  std::vector<std::unique_ptr<DropModifier>> modifiers;
  std::vector<int> sendCountPerFlow(kNumFlows, 0);
  std::vector<SendPacketFn> sendFns;
  sendFns.reserve(kNumFlows);
  for (int p = 0; p < kNumFlows; ++p) {
    auto config = std::make_unique<DropModifierConfig>(filter, kGlobalCap);
    config->appliedCount = sharedCounter; // share across all 4 Modifiers
    modifiers.push_back(std::make_unique<DropModifier>(std::move(config)));
    sendFns.push_back(createCountingSendFn(sendCountPerFlow[p]));
  }

  for (int p = 0; p < kNumFlows; ++p) {
    for (int i = 0; i < kPacketsPerFlow; ++i) {
      modifiers[p]->modifyTxPacketIfMatches(
          sendFns[p],
          std::make_unique<Modifier::TxPacketWithMetadata>(
              kDefaultRemoteAddr,
              std::make_unique<DataPacket>(
                  baseHeader, dataHeader, nullptr, 0)));
    }
  }

  int totalSent = 0;
  for (int p = 0; p < kNumFlows; ++p) {
    totalSent += sendCountPerFlow[p];
  }
  const int totalPackets = kNumFlows * kPacketsPerFlow;
  EXPECT_EQ(sharedCounter->load(), kGlobalCap)
      << "global cap should stop further modifications after " << kGlobalCap
      << " applications";
  EXPECT_EQ(totalSent, totalPackets - kGlobalCap)
      << "packets beyond the global cap should fall through to send";
}

TEST(HeaderEditModifierTest, RejectsAConfigOfAnotherType) {
  const auto jsonStr = R"({
        "name": "test_modifier",
        "type": "drop",
        "filter": {}
    })";

  auto modifierConfig =
      ModifierConfig::buildModifierConfigFromJson(folly::parseJson(jsonStr));

  EXPECT_THROW(
      (void)HeaderEditModifier(std::move(modifierConfig)), std::runtime_error)
      << "the config is resolved once at construction, so a modifier built on "
         "the wrong config type fails to build";
}

TEST(CorruptModifierTest, RejectsAConfigOfAnotherType) {
  const auto jsonStr = R"({
        "name": "test_modifier",
        "type": "drop",
        "filter": {}
    })";

  auto modifierConfig =
      ModifierConfig::buildModifierConfigFromJson(folly::parseJson(jsonStr));

  EXPECT_THROW(
      (void)CorruptModifier(std::move(modifierConfig)), std::runtime_error)
      << "the config is resolved once at construction, so a modifier built on "
         "the wrong config type fails to build";
}

TEST(WriteHeaderEditModifierTest, RejectsAConfigOfAnotherType) {
  const auto jsonStr = R"({
        "name": "test_modifier",
        "type": "drop",
        "filter": {}
    })";

  auto modifierConfig =
      ModifierConfig::buildModifierConfigFromJson(folly::parseJson(jsonStr));

  EXPECT_THROW(
      (void)WriteHeaderEditModifier(std::move(modifierConfig)),
      std::runtime_error)
      << "the config is resolved once at construction, so a modifier built on "
         "the wrong config type fails to build";
}

TEST(HeaderEditModifierTest, CanUpdateDstQp) {
  const auto jsonStr = R"({
        "name": "test_header_edit_modifier",
        "type": "header_edit",
        "filter": {
            "direction": "tx",
            "packet_type": "data"
        },
        "modification_times": 1,
        "header_type": "base",
        "field_name": "dst_qp",
        "value_to_set": 12345
    })";

  auto modifierConfig =
      ModifierConfig::buildModifierConfigFromJson(folly::parseJson(jsonStr));

  const auto* headerEditConfig =
      dynamic_cast<HeaderEditModifierConfig*>(modifierConfig.get());
  ASSERT_NE(headerEditConfig, nullptr)
      << "Config should be HeaderEditModifierConfig";
  EXPECT_EQ(
      headerEditConfig->headerType, HeaderEditModifierConfig::HeaderType::Base)
      << "Header type should be Base";
  EXPECT_EQ(headerEditConfig->fieldName, "dst_qp")
      << "Field name should be dst_qp";
  EXPECT_TRUE(headerEditConfig->valueToSet.has_value())
      << "valueToSet should have a value";
  EXPECT_EQ(headerEditConfig->valueToSet.value(), 12345)
      << "valueToSet should be 12345";

  constexpr uint32_t kOriginalDstQp = 100;
  constexpr uint32_t kExpectedDstQp = 12345;

  const auto baseHeader = createBaseHeader(
      BaseHeader::Opcode::OperationType::SEND,
      200,
      kOriginalDstQp,
      kDefaultFlowId);
  const auto dataHeader = createDataHeader();

  auto modifier = HeaderEditModifier(std::move(modifierConfig));

  // Track whether the packet was sent and capture the modified packet
  bool packetSent = false;
  uint32_t modifiedDstQp = 0;

  SendPacketFn fn = [&](const folly::SocketAddress& /*addr*/,
                        std::unique_ptr<Packet> packet,
                        std::optional<uint32_t> /*originalIcrc*/) {
    packetSent = true;
    // Verify the dst_qp was modified
    modifiedDstQp = packet->getBaseHeader().getDstQp();
    return 1L;
  };

  // Apply the modifier
  modifier.modifyTxPacketIfMatches(
      fn,
      std::make_unique<Modifier::TxPacketWithMetadata>(
          kDefaultRemoteAddr,
          std::make_unique<DataPacket>(baseHeader, dataHeader, nullptr, 0)));

  EXPECT_TRUE(packetSent);
  EXPECT_EQ(modifiedDstQp, kExpectedDstQp);
}

namespace {
std::string headerEditJson(
    const std::string& headerType,
    const std::string& fieldName,
    int valueToSet) {
  return std::string(R"({
        "name": "alias_under_test",
        "type": "header_edit",
        "filter": {"direction": "tx", "packet_type": "data"},
        "modification_times": 1,
        "header_type": ")") +
      headerType + R"(",
        "field_name": ")" +
      fieldName + R"(",
        "value_to_set": )" +
      std::to_string(valueToSet) + R"(
    })";
}

// Runs a base-header edit under whichever spelling of the field name the
// caller passes, and reports the flow id that reached the wire.
uint32_t flowIdAfterEdit(const std::string& fieldName) {
  auto modifierConfig = ModifierConfig::buildModifierConfigFromJson(
      folly::parseJson(headerEditJson("base", fieldName, 7)));
  auto modifier = HeaderEditModifier(std::move(modifierConfig));

  uint32_t seen = 0;
  SendPacketFn fn = [&](const folly::SocketAddress&,
                        std::unique_ptr<Packet> packet,
                        std::optional<uint32_t>) {
    seen = packet->getBaseHeader().getFlowId();
    return 1L;
  };
  modifier.modifyTxPacketIfMatches(
      fn,
      std::make_unique<Modifier::TxPacketWithMetadata>(
          kDefaultRemoteAddr,
          std::make_unique<DataPacket>(
              createBaseHeader(), createDataHeader(), nullptr, 0)));
  return seen;
}

// The same for the RNR cancel field. matchesFilter inspects only data and ack
// packets, so an RNR cancel reaches the modifier whatever packet type the
// filter declares.
uint8_t flowsAvailableAfterEdit(const std::string& fieldName) {
  auto modifierConfig = ModifierConfig::buildModifierConfigFromJson(
      folly::parseJson(headerEditJson("rnr_cancel", fieldName, 9)));
  auto modifier = HeaderEditModifier(std::move(modifierConfig));

  uint8_t seen = 0;
  SendPacketFn fn = [&](const folly::SocketAddress&,
                        std::unique_ptr<Packet> packet,
                        std::optional<uint32_t>) {
    auto* rnrCancel = dynamic_cast<RnrCancelPacket*>(packet.get());
    EXPECT_NE(rnrCancel, nullptr)
        << "the modifier should hand back the RNR cancel it was given, so a "
           "null here means the edit rebuilt the packet as something else";
    if (rnrCancel != nullptr) {
      seen = rnrCancel->getRnrCancelHeader().getFlowsAvailable();
    }
    return 1L;
  };
  modifier.modifyTxPacketIfMatches(
      fn,
      std::make_unique<Modifier::TxPacketWithMetadata>(
          kDefaultRemoteAddr,
          std::make_unique<RnrCancelPacket>(
              createBaseHeader(BaseHeader::Opcode::OperationType::RNR_CANCEL),
              RnrCancelHeader::RnrCancelHeaderBuilder()
                  .setFlowsAvailable(2)
                  .setPostedCsn(0)
                  .build())));
  return seen;
}
} // namespace

TEST(HeaderEditModifierTest, FlowIdAnswersToTheFormerPathSpellings) {
  for (const auto& spelling : {"flow_id", "flowId", "path_id", "pathId"}) {
    EXPECT_EQ(flowIdAfterEdit(spelling), 7u)
        << "a config that sets the field '" << spelling
        << "' should reach the same edit; a spelling that resolves to nothing "
           "leaves the packet untouched and the test silently unmodified";
  }
}

TEST(HeaderEditModifierTest, FlowsAvailableAnswersToTheFormerPathSpellings) {
  for (const auto& spelling :
       {"flows_available",
        "flowsAvailable",
        "paths_available",
        "pathsAvailable"}) {
    EXPECT_EQ(flowsAvailableAfterEdit(spelling), 9)
        << "a config that sets the field '" << spelling
        << "' should reach the same edit on the RNR cancel header";
  }
}

TEST(ModifierConfigTest, FlowIdFilterSelectsTheSamePacketsAsPathId) {
  for (const auto& spelling : {"flow_id", "path_id"}) {
    auto filter = createDataPacketFilter(
        ModifierConfig::ModifierFilter::Direction::Tx,
        {{spelling, FieldConditions(FieldCondition(kDefaultFlowId + 1))}});
    DropModifierConfig config(filter, 1);

    const auto dataPacket =
        DataPacket(createBaseHeader(), createDataHeader(), nullptr, 0);
    EXPECT_FALSE(config.matchesFilter(
        dataPacket,
        ModifierConfig::ModifierFilter::Direction::Tx,
        kDefaultRemoteAddr))
        << "a filter written as '" << spelling
        << "' must reject a flow id it does not name; a spelling the matcher "
           "does not know is skipped, which accepts every packet instead";
  }
}

TEST(HeaderEditModifierTest, CanXorModifyPosnInDataHeader) {
  const auto jsonStr = R"({
        "name": "test_header_edit_modifier_xor",
        "type": "header_edit",
        "filter": {
            "direction": "tx",
            "packet_type": "data"
        },
        "modification_times": 1,
        "header_type": "data",
        "field_name": "posn"
    })";

  auto modifierConfig =
      ModifierConfig::buildModifierConfigFromJson(folly::parseJson(jsonStr));

  const auto* headerEditConfig =
      dynamic_cast<HeaderEditModifierConfig*>(modifierConfig.get());
  ASSERT_NE(headerEditConfig, nullptr)
      << "Config should be HeaderEditModifierConfig";
  EXPECT_EQ(
      headerEditConfig->headerType, HeaderEditModifierConfig::HeaderType::Data)
      << "Header type should be Data";
  EXPECT_EQ(headerEditConfig->fieldName, "posn") << "Field name should be posn";
  EXPECT_FALSE(headerEditConfig->valueToSet.has_value())
      << "valueToSet should not have a value for XOR modification";

  constexpr uint16_t kOriginalPosn = 0x1234;
  constexpr uint16_t kExpectedPosn = kOriginalPosn ^ 0xFF;

  const auto baseHeader =
      createBaseHeader(BaseHeader::Opcode::OperationType::SEND, 200, 100, 1);
  const auto dataHeader =
      createDataHeader(kDefaultFsn, kDefaultMsn, kOriginalPosn);

  auto modifier = HeaderEditModifier(std::move(modifierConfig));

  // Track whether the packet was sent and capture the modified packet
  bool packetSent = false;
  uint16_t modifiedPosn = 0;

  SendPacketFn fn = [&](const folly::SocketAddress& /*addr*/,
                        std::unique_ptr<Packet> packet,
                        std::optional<uint32_t> /*originalIcrc*/) {
    packetSent = true;
    // Verify the posn was modified
    auto* dataPacket = dynamic_cast<DataPacket*>(packet.get());
    EXPECT_NE(dataPacket, nullptr);
    if (dataPacket) {
      modifiedPosn = dataPacket->getDataHeader().getPosn();
    }
    return 1L;
  };

  // Apply the modifier
  modifier.modifyTxPacketIfMatches(
      fn,
      std::make_unique<Modifier::TxPacketWithMetadata>(
          kDefaultRemoteAddr,
          std::make_unique<DataPacket>(baseHeader, dataHeader, nullptr, 0)));

  EXPECT_TRUE(packetSent);
  EXPECT_EQ(modifiedPosn, kExpectedPosn);
  EXPECT_NE(modifiedPosn, kOriginalPosn);
}

namespace {

// The una a tx data packet carries after the modifier described by jsonStr runs
// over a packet that was built carrying originalUna.
uint16_t editedUnaOf(const std::string& jsonStr, uint16_t originalUna) {
  auto modifierConfig =
      ModifierConfig::buildModifierConfigFromJson(folly::parseJson(jsonStr));
  auto modifier = HeaderEditModifier(std::move(modifierConfig));

  const auto dataHeader = DataHeader::DataHeaderBuilder()
                              .setFsn(kDefaultFsn)
                              .setUna(originalUna)
                              .setAckRequest(true)
                              .setPosn(kDefaultPosn)
                              .setMsn(kDefaultMsn)
                              .setCsn(kDefaultCsn)
                              .setTimestamp(kDefaultTimestamp)
                              .build();

  uint16_t editedUna = 0;
  SendPacketFn fn = [&](const folly::SocketAddress& /*addr*/,
                        std::unique_ptr<Packet> packet,
                        std::optional<uint32_t> /*originalIcrc*/) {
    auto* dataPacket = dynamic_cast<DataPacket*>(packet.get());
    EXPECT_NE(dataPacket, nullptr);
    if (dataPacket) {
      editedUna = dataPacket->getDataHeader().getUna();
    }
    return 1L;
  };

  modifier.modifyTxPacketIfMatches(
      fn,
      std::make_unique<Modifier::TxPacketWithMetadata>(
          kDefaultRemoteAddr,
          std::make_unique<DataPacket>(
              createBaseHeader(), dataHeader, nullptr, 0)));
  return editedUna;
}

constexpr auto* kSubtractUnaJson = R"({
        "name": "slow_una",
        "type": "header_edit",
        "filter": {
            "direction": "tx",
            "packet_type": "data"
        },
        "header_type": "data",
        "field_name": "una",
        "value_to_subtract": 120
    })";

} // namespace

TEST(HeaderEditModifierTest, SubtractHoldsUnaBehindTheSendersOwnValue) {
  EXPECT_EQ(editedUnaOf(kSubtractUnaJson, 200), 80)
      << "A subtract edit must be relative to the value the sender chose, so "
         "that una stays a fixed distance behind a field that advances from "
         "packet to packet; a set edit could only pin it to one constant.";
}

TEST(HeaderEditModifierTest, SubtractWrapsUnaWithinTheSequenceSpace) {
  // Early in a connection una is smaller than the lag, which is the case that
  // distinguishes modular arithmetic from a clamp at zero.
  EXPECT_EQ(editedUnaOf(kSubtractUnaJson, 40), 65456)
      << "Subtracting past zero must wrap within the 16-bit sequence space, "
         "because that is the arithmetic the receiver compares una under; "
         "saturating at 0 would instead look like a legitimate session start.";
}

TEST(HeaderEditModifierTest, SetAndSubtractTogetherAreRejected) {
  const auto jsonStr = R"({
        "name": "set_and_subtract",
        "type": "header_edit",
        "filter": {
            "direction": "tx",
            "packet_type": "data"
        },
        "header_type": "data",
        "field_name": "una",
        "value_to_set": 5,
        "value_to_subtract": 120
    })";

  EXPECT_THROW(
      (void)ModifierConfig::buildModifierConfigFromJson(
          folly::parseJson(jsonStr)),
      std::runtime_error)
      << "Naming both operations must be rejected at parse time rather than "
         "silently honoring one, which would edit the field in a way the test "
         "author did not ask for.";
}

TEST(HeaderEditModifierTest, SubtractOnAFlagFieldIsRejected) {
  const auto jsonStr = R"({
        "name": "subtract_from_a_flag",
        "type": "header_edit",
        "filter": {
            "direction": "tx",
            "packet_type": "data"
        },
        "header_type": "data",
        "field_name": "ack_request",
        "value_to_subtract": 1
    })";

  EXPECT_THROW(
      (void)ModifierConfig::buildModifierConfigFromJson(
          folly::parseJson(jsonStr)),
      std::runtime_error)
      << "A flag carries no magnitude, so subtracting from one must be "
         "rejected at parse time; the edit path would otherwise toggle the "
         "flag and report the edit as a subtract that never happened.";
}

TEST(ModifierConfigTest, ParseDelayAckModifierWithLargeTimeValue) {
  // This test verifies that the DELAY_ACK modifier from modifiers.py
  // is parsed correctly with full precision for the 1000000 microseconds value.
  // The DELAY_ACK modifier uses a reorder type with time_us distance.
  const auto jsonStr = R"({
        "name": "delay_ack",
        "type": "reorder",
        "filter": {
            "direction": "tx",
            "packet_type": "ack"
        },
        "distance": {
            "unit": "time_us",
            "value": 1000000
        },
        "modification_times": 9999
    })";

  const auto modifierConfig =
      ModifierConfig::buildModifierConfigFromJson(folly::parseJson(jsonStr));
  const auto* reorderConfig =
      dynamic_cast<ReorderModifierConfig*>(modifierConfig.get());

  ASSERT_NE(reorderConfig, nullptr) << "Config should be ReorderModifierConfig";
  EXPECT_EQ(reorderConfig->name, "delay_ack");
  EXPECT_EQ(reorderConfig->type, ModifierConfig::ModifierType::Reorder);
  EXPECT_EQ(reorderConfig->modificationTimes, std::optional<uint32_t>(9999));

  EXPECT_EQ(
      reorderConfig->filter.direction,
      ModifierConfig::ModifierFilter::Direction::Tx);
  EXPECT_EQ(
      reorderConfig->filter.packetType,
      ModifierConfig::ModifierFilter::PacketType::Ack);

  // Verify that the distance value is parsed with full precision (1000000 us)
  EXPECT_EQ(
      reorderConfig->distance,
      ReorderModifierConfig::Distance(
          1000000, ReorderModifierConfig::Distance::Unit::TimeUs));
}

TEST(ModifierConfigTest, ParseUnlimitedModificationTimes) {
  const auto jsonStr = R"({
        "name": "test_modifier",
        "type": "drop",
        "filter": {
            "direction": "tx",
            "packet_type": "data"
        },
        "modification_times": "unlimited"
    })";

  const auto modifierConfig =
      ModifierConfig::buildModifierConfigFromJson(folly::parseJson(jsonStr));
  ASSERT_TRUE(modifierConfig);

  EXPECT_FALSE(modifierConfig->modificationTimes.has_value())
      << "modification_times should be nullopt for 'unlimited'";
}

TEST(ModifierConfigTest, RejectInvalidStringModificationTimes) {
  const auto jsonStr = R"({
        "name": "test_modifier",
        "type": "drop",
        "filter": {
            "direction": "tx",
            "packet_type": "data"
        },
        "modification_times": "invalid_string"
    })";

  EXPECT_THROW(
      (void)ModifierConfig::buildModifierConfigFromJson(
          folly::parseJson(jsonStr)),
      std::runtime_error)
      << "Should reject non-numeric string that is not 'unlimited'";
}

TEST(ModifierConfigTest, DefaultMissingModificationTimes) {
  const auto jsonStr = R"({
        "name": "test_modifier",
        "type": "drop",
        "filter": {
            "direction": "tx",
            "packet_type": "data"
        }
    })";

  const auto modifierConfig =
      ModifierConfig::buildModifierConfigFromJson(folly::parseJson(jsonStr));
  ASSERT_TRUE(modifierConfig);

  EXPECT_FALSE(modifierConfig->modificationTimes.has_value())
      << "modification_times should default to unlimited (nullopt) when not specified";
}

TEST(ModifierConfigTest, RejectMissingFilter) {
  const auto jsonStr = R"({
        "name": "test_modifier",
        "type": "drop"
    })";

  EXPECT_THROW(
      (void)ModifierConfig::buildModifierConfigFromJson(
          folly::parseJson(jsonStr)),
      std::runtime_error)
      << "a drop modifier with no filter discards every tx data packet, which "
         "is likelier an omission than a request, so it is refused at parse "
         "time rather than silently applied to the whole flow";
}

TEST(ModifierConfigTest, AnEmptyFilterAsksForEveryTxDataPacket) {
  const auto jsonStr = R"({
        "name": "test_modifier",
        "type": "drop",
        "filter": {}
    })";

  const auto modifierConfig =
      ModifierConfig::buildModifierConfigFromJson(folly::parseJson(jsonStr));
  ASSERT_TRUE(modifierConfig);

  const auto& filter = modifierConfig->filter;
  EXPECT_EQ(filter.direction, ModifierConfig::ModifierFilter::Direction::Tx)
      << "an empty filter is how a config spells out that it means every "
         "packet, so it must be accepted where an omitted one is refused";
  EXPECT_EQ(filter.packetType, ModifierConfig::ModifierFilter::PacketType::Data)
      << "an empty filter picks the default packet type rather than none";
  EXPECT_DOUBLE_EQ(filter.randomPercent, 100.0)
      << "an empty filter reaches every packet it matches, so it must not "
         "silently sample a fraction of them";
  EXPECT_TRUE(filter.fields.empty())
      << "an empty filter constrains no packet field";
}

TEST(ModifierTest, DropModifierWithUnlimitedCount) {
  auto filter = createDataPacketFilter(
      ModifierConfig::ModifierFilter::Direction::Tx,
      {{"src_qp", FieldConditions(FieldCondition(2))},
       {"msn", FieldConditions(FieldCondition(1))},
       {"fsn", FieldConditions(FieldCondition(5))},
       {"flowId", FieldConditions(FieldCondition(1))}});
  auto config = std::make_unique<DropModifierConfig>(filter, std::nullopt);

  const auto baseHeader = createBaseHeader();
  const auto dataHeader = createDataHeader();

  auto modifier = DropModifier(std::move(config));
  int timesNotModified = 0;
  auto fn = createCountingSendFn(timesNotModified);

  constexpr int kTestIterations = 100;
  for (int i = 0; i < kTestIterations; i++) {
    modifier.modifyTxPacketIfMatches(
        fn,
        std::make_unique<Modifier::TxPacketWithMetadata>(
            kDefaultRemoteAddr,
            std::make_unique<DataPacket>(baseHeader, dataHeader, nullptr, 0)));
  }

  // Verify that all packets were dropped (fn was never called)
  EXPECT_EQ(timesNotModified, 0)
      << "All " << kTestIterations
      << " packets should be dropped with unlimited modification_times";
}

TEST(ModifierTest, WarnsWhenModifierAppliesZeroTimes) {
  // Create a modifier with Rx direction but send Tx packets — none match
  auto filter =
      createDataPacketFilter(ModifierConfig::ModifierFilter::Direction::Rx);
  auto config =
      std::make_unique<DropModifierConfig>(filter, 5, "test_zero_match");

  const auto baseHeader = createBaseHeader();
  const auto dataHeader = createDataHeader();

  ::testing::internal::CaptureStderr();
  {
    auto modifier = DropModifier(std::move(config));
    int sendCount = 0;
    auto fn = createCountingSendFn(sendCount);

    modifier.modifyTxPacketIfMatches(
        fn,
        std::make_unique<Modifier::TxPacketWithMetadata>(
            folly::SocketAddress("fc00::1", 10000),
            std::make_unique<DataPacket>(baseHeader, dataHeader, nullptr, 0)));
    EXPECT_EQ(sendCount, 1)
        << "Packet should pass through unmodified when direction doesn't match";
  }
  std::string output = ::testing::internal::GetCapturedStderr();

  EXPECT_NE(output.find("test_zero_match"), std::string::npos)
      << "Warning should name the modifier. Got: " << output;
  EXPECT_NE(output.find("applied 0 times"), std::string::npos)
      << "Warning should report zero applications. Got: " << output;
  EXPECT_NE(output.find("random_percent"), std::string::npos)
      << "Warning should mention random filter sampling. Got: " << output;
  EXPECT_EQ(output.find("matched 0 packets"), std::string::npos)
      << "Warning should not claim that the content filter matched no packets";
  // Should NOT also emit an INFO stats line (warning replaces it)
  EXPECT_EQ(output.find("stats:"), std::string::npos)
      << "Zero-match case should only emit WARNING, not INFO. Got: " << output;
}

TEST(ModifierTest, WarnsWhenUnlimitedModifierAppliesZeroTimes) {
  // Unlimited modifier with Rx direction but only Tx packets — none match
  auto filter =
      createDataPacketFilter(ModifierConfig::ModifierFilter::Direction::Rx);
  auto config = std::make_unique<DropModifierConfig>(
      filter, std::nullopt, "test_unlimited_zero");

  const auto baseHeader = createBaseHeader();
  const auto dataHeader = createDataHeader();

  ::testing::internal::CaptureStderr();
  {
    auto modifier = DropModifier(std::move(config));
    int sendCount = 0;
    auto fn = createCountingSendFn(sendCount);

    modifier.modifyTxPacketIfMatches(
        fn,
        std::make_unique<Modifier::TxPacketWithMetadata>(
            folly::SocketAddress("fc00::1", 10000),
            std::make_unique<DataPacket>(baseHeader, dataHeader, nullptr, 0)));
    EXPECT_EQ(sendCount, 1)
        << "Packet should pass through unmodified when direction doesn't match";
  }
  std::string output = ::testing::internal::GetCapturedStderr();

  EXPECT_NE(output.find("test_unlimited_zero"), std::string::npos)
      << "Warning should name the modifier. Got: " << output;
  EXPECT_NE(output.find("applied 0 times"), std::string::npos)
      << "Unlimited modifier with zero applications should warn. Got: "
      << output;
  EXPECT_NE(output.find("configured limit: unlimited"), std::string::npos)
      << "Warning should show unlimited limit. Got: " << output;
  EXPECT_NE(output.find("random_percent"), std::string::npos)
      << "Warning should mention random filter sampling. Got: " << output;
  EXPECT_EQ(output.find("matched 0 packets"), std::string::npos)
      << "Warning should not claim that the content filter matched no packets";
}

TEST(ModifierTest, StatsLoggedOnDestruction) {
  auto filter = createDataPacketFilter();
  auto config = std::make_unique<DropModifierConfig>(filter, 5, "test_stats");

  const auto baseHeader = createBaseHeader();
  const auto dataHeader = createDataHeader();

  ::testing::internal::CaptureStderr();
  {
    auto modifier = DropModifier(std::move(config));
    int sendCount = 0;
    auto fn = createCountingSendFn(sendCount);

    // Apply modifier 3 times (matching Tx packets with Tx filter)
    for (int i = 0; i < 3; i++) {
      modifier.modifyTxPacketIfMatches(
          fn,
          std::make_unique<Modifier::TxPacketWithMetadata>(
              folly::SocketAddress("fc00::1", 10000),
              std::make_unique<DataPacket>(
                  baseHeader, dataHeader, nullptr, 0)));
    }
  }
  std::string output = ::testing::internal::GetCapturedStderr();

  EXPECT_NE(output.find("test_stats"), std::string::npos)
      << "Stats log should include modifier name. Got: " << output;
  EXPECT_NE(output.find("applied 3 times"), std::string::npos)
      << "Stats log should show actual match count. Got: " << output;
  EXPECT_NE(output.find("configured limit: 5"), std::string::npos)
      << "Stats log should show configured limit. Got: " << output;
  // No warning expected since modifier matched packets
  EXPECT_EQ(output.find("matched 0 packets"), std::string::npos)
      << "No zero-match warning expected when modifier matched packets";
}

TEST(FieldConditionTest, EvaluateAllOperators) {
  EXPECT_TRUE(FieldCondition(ComparisonOperator::Eq, 5).evaluate(5));
  EXPECT_FALSE(FieldCondition(ComparisonOperator::Eq, 5).evaluate(4));

  EXPECT_FALSE(FieldCondition(ComparisonOperator::Ne, 5).evaluate(5));
  EXPECT_TRUE(FieldCondition(ComparisonOperator::Ne, 5).evaluate(4));

  EXPECT_TRUE(FieldCondition(ComparisonOperator::Gt, 5).evaluate(6));
  EXPECT_FALSE(FieldCondition(ComparisonOperator::Gt, 5).evaluate(5));

  EXPECT_TRUE(FieldCondition(ComparisonOperator::Lt, 5).evaluate(4));
  EXPECT_FALSE(FieldCondition(ComparisonOperator::Lt, 5).evaluate(5));

  EXPECT_TRUE(FieldCondition(ComparisonOperator::Ge, 5).evaluate(5));
  EXPECT_TRUE(FieldCondition(ComparisonOperator::Ge, 5).evaluate(6));
  EXPECT_FALSE(FieldCondition(ComparisonOperator::Ge, 5).evaluate(4));

  EXPECT_TRUE(FieldCondition(ComparisonOperator::Le, 5).evaluate(5));
  EXPECT_TRUE(FieldCondition(ComparisonOperator::Le, 5).evaluate(4));
  EXPECT_FALSE(FieldCondition(ComparisonOperator::Le, 5).evaluate(6));

  // Mod operator: value % 8 == 0
  EXPECT_TRUE(FieldCondition(ComparisonOperator::Mod, 8, 0).evaluate(0))
      << "0 % 8 == 0";
  EXPECT_TRUE(FieldCondition(ComparisonOperator::Mod, 8, 0).evaluate(16))
      << "16 % 8 == 0";
  EXPECT_FALSE(FieldCondition(ComparisonOperator::Mod, 8, 0).evaluate(5))
      << "5 % 8 != 0";

  // Mod operator with non-zero remainder: value % 8 == 3
  EXPECT_TRUE(FieldCondition(ComparisonOperator::Mod, 8, 3).evaluate(3))
      << "3 % 8 == 3";
  EXPECT_TRUE(FieldCondition(ComparisonOperator::Mod, 8, 3).evaluate(11))
      << "11 % 8 == 3";
  EXPECT_FALSE(FieldCondition(ComparisonOperator::Mod, 8, 3).evaluate(16))
      << "16 % 8 != 3";

  // Mod operator with divisor of 0 should always return false
  EXPECT_FALSE(FieldCondition(ComparisonOperator::Mod, 0, 0).evaluate(0))
      << "Divisor 0 should never match";
}

TEST(ModifierConfigTest, ParseFieldConditionAllFormats) {
  const auto jsonStr = R"({
    "name": "test_all_operators",
    "type": "drop",
    "filter": {
      "direction": "tx",
      "packet_type": "data",
      "fields": {
        "fsn": {"op": ">", "value": 10},
        "msn": {"op": "<=", "value": 100},
        "path_id": {"op": "!=", "value": 0},
        "src_qp": {"op": "=", "value": 5},
        "dst_qp": {"op": "==", "value": 6}
      }
    }
  })";

  const auto modifierConfig =
      ModifierConfig::buildModifierConfigFromJson(folly::parseJson(jsonStr));
  ASSERT_TRUE(modifierConfig);

  EXPECT_EQ(
      modifierConfig->filter.fields.at("fsn").op(), ComparisonOperator::Gt);
  EXPECT_EQ(modifierConfig->filter.fields.at("fsn").value(), 10);

  EXPECT_EQ(
      modifierConfig->filter.fields.at("msn").op(), ComparisonOperator::Le);
  EXPECT_EQ(modifierConfig->filter.fields.at("msn").value(), 100);

  EXPECT_EQ(
      modifierConfig->filter.fields.at("path_id").op(), ComparisonOperator::Ne);
  EXPECT_EQ(modifierConfig->filter.fields.at("path_id").value(), 0);

  EXPECT_EQ(
      modifierConfig->filter.fields.at("src_qp").op(), ComparisonOperator::Eq);
  EXPECT_EQ(
      modifierConfig->filter.fields.at("dst_qp").op(), ComparisonOperator::Eq);
}

TEST(ModifierConfigTest, ParseFieldConditionBackwardCompatible) {
  const auto backwardCompatJson = R"({
    "name": "test_backward_compat",
    "type": "drop",
    "filter": {
      "direction": "tx",
      "packet_type": "data",
      "fields": { "fsn": 5 }
    }
  })";

  const auto modifierConfig = ModifierConfig::buildModifierConfigFromJson(
      folly::parseJson(backwardCompatJson));
  ASSERT_TRUE(modifierConfig);
  EXPECT_EQ(
      modifierConfig->filter.fields.at("fsn").op(), ComparisonOperator::Eq);
  EXPECT_EQ(modifierConfig->filter.fields.at("fsn").value(), 5);
}

TEST(ModifierConfigTest, DataPacketMatchesWithOperators) {
  const auto baseHeader = createBaseHeader();
  const auto dataHeader = createDataHeader(5); // fsn = 5

  // Test Ne operator: fsn != 10 should match (5 != 10 is true)
  {
    auto filter = createDataPacketFilter(
        ModifierConfig::ModifierFilter::Direction::Tx,
        {{"fsn", FieldConditions(FieldCondition(ComparisonOperator::Ne, 10))}});
    DropModifierConfig config(filter, 1);
    EXPECT_TRUE(config.matchesFilter(
        DataPacket(baseHeader, dataHeader, nullptr, 0),
        ModifierConfig::ModifierFilter::Direction::Tx,
        kDefaultRemoteAddr));
  }

  // Test Ne operator: fsn != 5 should not match (5 != 5 is false)
  {
    auto filter = createDataPacketFilter(
        ModifierConfig::ModifierFilter::Direction::Tx,
        {{"fsn", FieldConditions(FieldCondition(ComparisonOperator::Ne, 5))}});
    DropModifierConfig config(filter, 1);
    EXPECT_FALSE(config.matchesFilter(
        DataPacket(baseHeader, dataHeader, nullptr, 0),
        ModifierConfig::ModifierFilter::Direction::Tx,
        kDefaultRemoteAddr));
  }

  // Test Gt operator: fsn > 3 should match (5 > 3 is true)
  {
    auto filter = createDataPacketFilter(
        ModifierConfig::ModifierFilter::Direction::Tx,
        {{"fsn", FieldConditions(FieldCondition(ComparisonOperator::Gt, 3))}});
    DropModifierConfig config(filter, 1);
    EXPECT_TRUE(config.matchesFilter(
        DataPacket(baseHeader, dataHeader, nullptr, 0),
        ModifierConfig::ModifierFilter::Direction::Tx,
        kDefaultRemoteAddr));
  }

  // Test multiple operators (AND semantics): fsn >= 3 AND path_id == 1
  {
    auto filter = createDataPacketFilter(
        ModifierConfig::ModifierFilter::Direction::Tx,
        {{"fsn", FieldConditions(FieldCondition(ComparisonOperator::Ge, 3))},
         {"path_id",
          FieldConditions(
              FieldCondition(ComparisonOperator::Eq, kDefaultFlowId))}});
    DropModifierConfig config(filter, 1);
    EXPECT_TRUE(config.matchesFilter(
        DataPacket(baseHeader, dataHeader, nullptr, 0),
        ModifierConfig::ModifierFilter::Direction::Tx,
        kDefaultRemoteAddr));
  }

  // Test Mod operator: fsn % 3 == 2 should match (5 % 3 == 2)
  {
    auto filter = createDataPacketFilter(
        ModifierConfig::ModifierFilter::Direction::Tx,
        {{"fsn",
          FieldConditions(FieldCondition(ComparisonOperator::Mod, 3, 2))}});
    DropModifierConfig config(filter, 1);
    EXPECT_TRUE(config.matchesFilter(
        DataPacket(baseHeader, dataHeader, nullptr, 0),
        ModifierConfig::ModifierFilter::Direction::Tx,
        kNonMatchingRemoteAddr))
        << "5 % 3 == 2 should match";
  }

  // Test Mod operator: fsn % 3 == 0 should not match (5 % 3 == 2, not 0)
  {
    auto filter = createDataPacketFilter(
        ModifierConfig::ModifierFilter::Direction::Tx,
        {{"fsn",
          FieldConditions(FieldCondition(ComparisonOperator::Mod, 3, 0))}});
    DropModifierConfig config(filter, 1);
    EXPECT_FALSE(config.matchesFilter(
        DataPacket(baseHeader, dataHeader, nullptr, 0),
        ModifierConfig::ModifierFilter::Direction::Tx,
        kNonMatchingRemoteAddr))
        << "5 % 3 != 0 should not match";
  }
}

TEST(ModifierConfigTest, DataPacketMatchesRemoteAddrFilter) {
  const auto baseHeader = createBaseHeader();
  const auto dataHeader = createDataHeader();

  // Matching address: filter expects fc00::1, and remoteAddr is fc00::1
  {
    auto filter = createDataPacketFilter();
    filter.remoteAddr = folly::IPAddress(kDefaultRemoteAddrStr);
    DropModifierConfig config(filter, 1);
    EXPECT_TRUE(config.matchesFilter(
        DataPacket(baseHeader, dataHeader, nullptr, 0),
        ModifierConfig::ModifierFilter::Direction::Tx,
        kDefaultRemoteAddr))
        << "Should match when remoteAddr IP equals remote_addr filter";
  }

  // Non-matching address: filter expects fc00::2, but remoteAddr is fc00::1
  {
    auto filter = createDataPacketFilter();
    filter.remoteAddr = folly::IPAddress(kNonMatchingRemoteAddrStr);
    DropModifierConfig config(filter, 1);
    EXPECT_FALSE(config.matchesFilter(
        DataPacket(baseHeader, dataHeader, nullptr, 0),
        ModifierConfig::ModifierFilter::Direction::Tx,
        kDefaultRemoteAddr))
        << "Should not match when remoteAddr IP differs from remote_addr filter";
  }

  // Port should be ignored: same IP but different ports should still match
  {
    auto filter = createDataPacketFilter();
    filter.remoteAddr = folly::IPAddress(kDefaultRemoteAddrStr);
    DropModifierConfig config(filter, 1);
    EXPECT_TRUE(config.matchesFilter(
        DataPacket(baseHeader, dataHeader, nullptr, 0),
        ModifierConfig::ModifierFilter::Direction::Tx,
        folly::SocketAddress(kDefaultRemoteAddrStr, 9999)))
        << "Should match regardless of port number";
  }

  // No remote_addr in filter: should match any address
  {
    auto filter = createDataPacketFilter();
    DropModifierConfig config(filter, 1);
    EXPECT_TRUE(config.matchesFilter(
        DataPacket(baseHeader, dataHeader, nullptr, 0),
        ModifierConfig::ModifierFilter::Direction::Tx,
        folly::SocketAddress("fc00::99", 10000)))
        << "Should match any address when remote_addr filter is not set";
  }
}

TEST(ModifierConfigTest, AckPacketMatchesRemoteAddrFilter) {
  const auto baseHeader = createBaseHeader(
      BaseHeader::Opcode::OperationType::SEL_ACK,
      kDefaultSrcQp,
      kDefaultDstQp,
      kDefaultFlowId);
  BitVector<AckHeader::kAckTrackerBits> fsnBitVector;
  const auto ackHeader = createAckHeader(1, fsnBitVector);

  // Matching address on ack packet
  {
    ModifierConfig::ModifierFilter filter(
        ModifierConfig::ModifierFilter::Direction::Rx,
        ModifierConfig::ModifierFilter::PacketType::Ack,
        {{"dst_qp", FieldConditions(FieldCondition(kDefaultDstQp))}});
    filter.remoteAddr = folly::IPAddress(kDefaultRemoteAddrStr);
    DropModifierConfig config(filter, 1);
    EXPECT_TRUE(config.matchesFilter(
        AckPacket(baseHeader, ackHeader),
        ModifierConfig::ModifierFilter::Direction::Rx,
        kDefaultRemoteAddr))
        << "Ack packet should match when remoteAddr IP equals remote_addr filter";
  }

  // Non-matching address on ack packet
  {
    ModifierConfig::ModifierFilter filter(
        ModifierConfig::ModifierFilter::Direction::Rx,
        ModifierConfig::ModifierFilter::PacketType::Ack,
        {{"dst_qp", FieldConditions(FieldCondition(kDefaultDstQp))}});
    filter.remoteAddr = folly::IPAddress(kDefaultRemoteAddrStr);
    DropModifierConfig config(filter, 1);
    EXPECT_FALSE(config.matchesFilter(
        AckPacket(baseHeader, ackHeader),
        ModifierConfig::ModifierFilter::Direction::Rx,
        kNonMatchingRemoteAddr))
        << "Ack packet should not match when remoteAddr IP differs from "
           "remote_addr filter";
  }
}

TEST(ModifierConfigTest, ParseModOperator) {
  // Test parsing % operator with default remainder (0)
  const auto jsonStr = R"({
    "name": "test_mod_default_remainder",
    "type": "drop",
    "filter": {
      "direction": "tx",
      "packet_type": "data",
      "fields": {
        "fsn": {"op": "%", "value": 8}
      }
    }
  })";

  const auto config =
      ModifierConfig::buildModifierConfigFromJson(folly::parseJson(jsonStr));
  EXPECT_EQ(config->filter.fields.at("fsn").op(), ComparisonOperator::Mod);
  EXPECT_EQ(config->filter.fields.at("fsn").value(), 8);

  EXPECT_TRUE(config->filter.fields.at("fsn").evaluate(0)) << "0 % 8 == 0";
  EXPECT_TRUE(config->filter.fields.at("fsn").evaluate(8)) << "8 % 8 == 0";
  EXPECT_TRUE(config->filter.fields.at("fsn").evaluate(16)) << "16 % 8 == 0";
  EXPECT_FALSE(config->filter.fields.at("fsn").evaluate(1)) << "1 % 8 != 0";
  EXPECT_FALSE(config->filter.fields.at("fsn").evaluate(7)) << "7 % 8 != 0";
}

TEST(ModifierConfigTest, ParseModOperatorWithRemainder) {
  // Test parsing Mod operator with explicit remainder
  const auto jsonStr = R"({
    "name": "test_mod_with_remainder",
    "type": "drop",
    "filter": {
      "direction": "tx",
      "packet_type": "data",
      "fields": {
        "fsn": {"op": "Mod", "value": 8, "remainder": 3}
      }
    }
  })";

  const auto config =
      ModifierConfig::buildModifierConfigFromJson(folly::parseJson(jsonStr));
  EXPECT_EQ(config->filter.fields.at("fsn").op(), ComparisonOperator::Mod);

  EXPECT_TRUE(config->filter.fields.at("fsn").evaluate(3)) << "3 % 8 == 3";
  EXPECT_TRUE(config->filter.fields.at("fsn").evaluate(11)) << "11 % 8 == 3";
  EXPECT_FALSE(config->filter.fields.at("fsn").evaluate(0)) << "0 % 8 != 3";
  EXPECT_FALSE(config->filter.fields.at("fsn").evaluate(8)) << "8 % 8 != 3";
}

TEST(ModifierConfigTest, ParseModOperatorZeroDivisorThrows) {
  const auto jsonStr = R"({
    "name": "test_mod_zero",
    "type": "drop",
    "filter": {
      "direction": "tx",
      "packet_type": "data",
      "fields": {
        "fsn": {"op": "%", "value": 0}
      }
    }
  })";

  EXPECT_THROW(
      {
        [[maybe_unused]] auto config =
            ModifierConfig::buildModifierConfigFromJson(
                folly::parseJson(jsonStr));
      },
      std::runtime_error)
      << "Mod with divisor 0 should throw";
}

TEST(ModifierConfigTest, ParseModOperatorRemainderGeDivisorThrows) {
  const auto jsonStr = R"({
    "name": "test_mod_bad_remainder",
    "type": "drop",
    "filter": {
      "direction": "tx",
      "packet_type": "data",
      "fields": {
        "fsn": {"op": "%", "value": 3, "remainder": 5}
      }
    }
  })";

  EXPECT_THROW(
      {
        [[maybe_unused]] auto config =
            ModifierConfig::buildModifierConfigFromJson(
                folly::parseJson(jsonStr));
      },
      std::runtime_error)
      << "Mod with remainder >= divisor should throw";
}

TEST(FieldConditionsTest, SingleCondition) {
  FieldConditions cond(FieldCondition(ComparisonOperator::Eq, 5));

  EXPECT_TRUE(cond.evaluate(5));
  EXPECT_FALSE(cond.evaluate(4));
  EXPECT_FALSE(cond.evaluate(6));

  EXPECT_EQ(cond.op(), ComparisonOperator::Eq);
  EXPECT_EQ(cond.value(), 5);
}

TEST(FieldConditionsTest, MultipleConditionsAndLogic) {
  // Test multiple conditions with AND logic: 5 <= value < 10
  std::vector<FieldCondition> conditions;
  conditions.emplace_back(ComparisonOperator::Ge, 5);
  conditions.emplace_back(ComparisonOperator::Lt, 10);
  FieldConditions cond(std::move(conditions));

  // Values within range [5, 10) should pass
  EXPECT_TRUE(cond.evaluate(5));
  EXPECT_TRUE(cond.evaluate(7));
  EXPECT_TRUE(cond.evaluate(9));

  // Values outside range should fail
  EXPECT_FALSE(cond.evaluate(4));
  EXPECT_FALSE(cond.evaluate(10));
  EXPECT_FALSE(cond.evaluate(15));
}

TEST(FieldConditionsTest, ParseInOperatorExplicitSyntax) {
  // Test parsing explicit In operator: {"op": "In", "value": [0, 1, 2, 4, 8]}
  const auto jsonStr = R"({
        "name": "test_modifier",
        "type": "drop",
        "filter": {
            "direction": "tx",
            "packet_type": "data",
            "fields": {
                "fsn": {"op": "In", "value": [0, 1, 2, 4, 8]}
            }
        },
        "modification_times": 1
    })";

  const auto config =
      ModifierConfig::buildModifierConfigFromJson(folly::parseJson(jsonStr));
  EXPECT_EQ(config->filter.fields.at("fsn").conditions.size(), 1);
  EXPECT_EQ(config->filter.fields.at("fsn").op(), ComparisonOperator::In);

  // Values in the set should match
  EXPECT_TRUE(config->filter.fields.at("fsn").evaluate(0));
  EXPECT_TRUE(config->filter.fields.at("fsn").evaluate(1));
  EXPECT_TRUE(config->filter.fields.at("fsn").evaluate(2));
  EXPECT_TRUE(config->filter.fields.at("fsn").evaluate(4));
  EXPECT_TRUE(config->filter.fields.at("fsn").evaluate(8));

  // Values not in the set should not match
  EXPECT_FALSE(config->filter.fields.at("fsn").evaluate(3));
  EXPECT_FALSE(config->filter.fields.at("fsn").evaluate(5));
  EXPECT_FALSE(config->filter.fields.at("fsn").evaluate(6));
  EXPECT_FALSE(config->filter.fields.at("fsn").evaluate(7));
  EXPECT_FALSE(config->filter.fields.at("fsn").evaluate(9));
  EXPECT_FALSE(config->filter.fields.at("fsn").evaluate(10));
}

TEST(FieldConditionsTest, ParseInOperatorArrayShorthand) {
  // Test parsing array shorthand for In operator: [5, 6, 7, 8, 9, 10]
  const auto jsonStr = R"({
        "name": "test_modifier",
        "type": "drop",
        "filter": {
            "direction": "tx",
            "packet_type": "data",
            "fields": {
                "fsn": [5, 6, 7, 8, 9, 10]
            }
        },
        "modification_times": 1
    })";

  const auto config =
      ModifierConfig::buildModifierConfigFromJson(folly::parseJson(jsonStr));
  EXPECT_EQ(config->filter.fields.at("fsn").conditions.size(), 1);
  EXPECT_EQ(config->filter.fields.at("fsn").op(), ComparisonOperator::In);

  // Values in the set should match
  EXPECT_TRUE(config->filter.fields.at("fsn").evaluate(5));
  EXPECT_TRUE(config->filter.fields.at("fsn").evaluate(6));
  EXPECT_TRUE(config->filter.fields.at("fsn").evaluate(7));
  EXPECT_TRUE(config->filter.fields.at("fsn").evaluate(8));
  EXPECT_TRUE(config->filter.fields.at("fsn").evaluate(9));
  EXPECT_TRUE(config->filter.fields.at("fsn").evaluate(10));

  // Values not in the set should not match
  EXPECT_FALSE(config->filter.fields.at("fsn").evaluate(4));
  EXPECT_FALSE(config->filter.fields.at("fsn").evaluate(11));
  EXPECT_FALSE(config->filter.fields.at("fsn").evaluate(0));
}

TEST(ModifierConfigTest, ParseJsonPacingModifier) {
  const auto jsonStr = R"({
        "name": "pace_rx_path0",
        "type": "pacing",
        "filter": {
            "direction": "rx",
            "packet_type": "data",
            "fields": {"path_id": 0}
        },
        "interval_us": 1000
    })";

  const auto modifierConfig =
      ModifierConfig::buildModifierConfigFromJson(folly::parseJson(jsonStr));
  const auto* pacingConfig =
      dynamic_cast<PacingModifierConfig*>(modifierConfig.get());

  ASSERT_NE(pacingConfig, nullptr)
      << "buildModifierConfigFromJson should produce a PacingModifierConfig for type=pacing";
  EXPECT_EQ(pacingConfig->name, "pace_rx_path0");
  EXPECT_EQ(pacingConfig->type, ModifierConfig::ModifierType::Pacing);
  EXPECT_EQ(pacingConfig->intervalUs, 1000)
      << "interval_us from JSON must be propagated to the config";
  EXPECT_EQ(
      pacingConfig->filter.direction,
      ModifierConfig::ModifierFilter::Direction::Rx);
  EXPECT_EQ(
      pacingConfig->filter.packetType,
      ModifierConfig::ModifierFilter::PacketType::Data);

  // Verify missing interval_us throws
  const auto missingIntervalJsonStr = R"({
        "name": "pace_missing",
        "type": "pacing",
        "filter": {
            "direction": "rx",
            "packet_type": "data"
        }
    })";

  EXPECT_THROW(
      (void)ModifierConfig::buildModifierConfigFromJson(
          folly::parseJson(missingIntervalJsonStr)),
      std::runtime_error)
      << "pacing modifier requires interval_us to be specified";
}

TEST(PacingModifierTest, FirstPacketSentImmediately) {
  auto config = std::make_unique<PacingModifierConfig>(
      createDataPacketFilter(), std::nullopt, 100000);
  PacingModifier modifier(std::move(config));

  int sendCount = 0;
  auto fn = createCountingSendFn(sendCount);
  const auto baseHeader = createBaseHeader();
  const auto dataHeader = createDataHeader();

  modifier.modifyTxPacketIfMatches(
      fn,
      std::make_unique<Modifier::TxPacketWithMetadata>(
          kDefaultRemoteAddr,
          std::make_unique<DataPacket>(baseHeader, dataHeader, nullptr, 0)));

  EXPECT_EQ(sendCount, 1)
      << "First packet should bypass pacing since no prior packet was sent";
}

TEST(PacingModifierTest, SpacesPacketsAtAnIntervalBelowTheWheelTick) {
  // A quarter of folly's 200us high-resolution wheel tick.
  constexpr uint32_t kIntervalUs = 50;
  constexpr size_t kPackets = 200;

  auto config = std::make_unique<PacingModifierConfig>(
      createDataPacketFilter(), std::nullopt, kIntervalUs);
  PacingModifier modifier(std::move(config));

  std::vector<uint64_t> sentAtNs;
  sentAtNs.reserve(kPackets);
  std::mutex sentAtMutex;
  folly::Baton<> allSent;
  SendPacketFn fn = [&](const folly::SocketAddress&,
                        std::unique_ptr<Packet>,
                        std::optional<uint32_t>) {
    std::lock_guard<std::mutex> guard(sentAtMutex);
    sentAtNs.push_back(getNowNs64());
    if (sentAtNs.size() == kPackets) {
      allSent.post();
    }
    return 1L;
  };

  const auto baseHeader = createBaseHeader();
  const auto dataHeader = createDataHeader();
  for (size_t i = 0; i < kPackets; ++i) {
    modifier.modifyTxPacketIfMatches(
        fn,
        std::make_unique<Modifier::TxPacketWithMetadata>(
            kDefaultRemoteAddr,
            std::make_unique<DataPacket>(baseHeader, dataHeader, nullptr, 0)));
  }
  ASSERT_TRUE(allSent.try_wait_for(std::chrono::seconds(30)))
      << "every paced packet must be released";

  std::vector<int64_t> gapUs;
  gapUs.reserve(kPackets - 1);
  for (size_t i = 1; i < sentAtNs.size(); ++i) {
    gapUs.push_back(static_cast<int64_t>(sentAtNs[i] - sentAtNs[i - 1]) / 1000);
  }
  std::sort(gapUs.begin(), gapUs.end());
  const auto at = [&](double p) {
    return gapUs[std::min<size_t>(gapUs.size() * p, gapUs.size() - 1)];
  };
  // The upper quartile, because a scheduler stall that releases a batch late
  // compresses the gaps below it.
  const int64_t upperQuartileGapUs = at(0.75);
  LOG(INFO) << "PACINGGAP interval_us=" << kIntervalUs << " n=" << gapUs.size()
            << " min_us=" << gapUs.front() << " p25_us=" << at(0.25)
            << " p50_us=" << at(0.50) << " p75_us=" << upperQuartileGapUs
            << " max_us=" << gapUs.back();

  EXPECT_GE(upperQuartileGapUs, kIntervalUs / 2)
      << "at least a quarter of the gaps between paced packets must be near "
         "the configured interval; quantizing delivery to a coarser tick "
         "releases packets in bunches, leaving almost every neighboring pair "
         "with no gap at all, which is not pacing";
}

TEST(PacingModifierTest, SecondPacketIsDelayed) {
  constexpr uint32_t kIntervalUs = 50000; // 50ms
  auto config = std::make_unique<PacingModifierConfig>(
      createDataPacketFilter(), std::nullopt, kIntervalUs);
  PacingModifier modifier(std::move(config));

  std::atomic<int> sendCount{0};
  folly::Baton<> secondPacketSent;
  SendPacketFn fn = [&](const folly::SocketAddress&,
                        std::unique_ptr<Packet>,
                        std::optional<uint32_t>) {
    if (++sendCount == 2) {
      secondPacketSent.post();
    }
    return 1L;
  };
  const auto baseHeader = createBaseHeader();
  const auto dataHeader = createDataHeader();

  modifier.modifyTxPacketIfMatches(
      fn,
      std::make_unique<Modifier::TxPacketWithMetadata>(
          kDefaultRemoteAddr,
          std::make_unique<DataPacket>(baseHeader, dataHeader, nullptr, 0)));
  EXPECT_EQ(sendCount.load(), 1);

  // Second packet sent back-to-back should be scheduled, not immediate
  modifier.modifyTxPacketIfMatches(
      fn,
      std::make_unique<Modifier::TxPacketWithMetadata>(
          kDefaultRemoteAddr,
          std::make_unique<DataPacket>(baseHeader, dataHeader, nullptr, 0)));
  EXPECT_EQ(sendCount.load(), 1)
      << "Second packet should be held until the pacing interval elapses";

  ASSERT_TRUE(secondPacketSent.try_wait_for(std::chrono::seconds(5)))
      << "Timed out waiting for paced packet delivery";
  EXPECT_EQ(sendCount.load(), 2);
}

TEST(PacingModifierTest, EachHeldPacketUsesItsOwnSendCallback) {
  // Long enough that both packets after the first are held, not sent inline.
  constexpr uint32_t kIntervalUs = 50000;
  auto config = std::make_unique<PacingModifierConfig>(
      createDataPacketFilter(), std::nullopt, kIntervalUs);
  PacingModifier modifier(std::move(config));

  int immediateSends = 0;
  auto opener = createCountingSendFn(immediateSends);
  std::atomic<int> viaA{0};
  std::atomic<int> viaB{0};
  folly::Baton<> bothReleased;
  const auto countAndSignal = [&](std::atomic<int>& via) {
    ++via;
    if (viaA.load() + viaB.load() == 2) {
      bothReleased.post();
    }
  };
  SendPacketFn sendA = [&](const folly::SocketAddress&,
                           std::unique_ptr<Packet>,
                           std::optional<uint32_t>) {
    countAndSignal(viaA);
    return 1L;
  };
  SendPacketFn sendB = [&](const folly::SocketAddress&,
                           std::unique_ptr<Packet>,
                           std::optional<uint32_t>) {
    countAndSignal(viaB);
    return 1L;
  };
  const auto baseHeader = createBaseHeader();
  const auto dataHeader = createDataHeader();
  const auto post = [&](SendPacketFn& fn) {
    modifier.modifyTxPacketIfMatches(
        fn,
        std::make_unique<Modifier::TxPacketWithMetadata>(
            kDefaultRemoteAddr,
            std::make_unique<DataPacket>(baseHeader, dataHeader, nullptr, 0)));
  };

  post(opener);
  ASSERT_EQ(immediateSends, 1)
      << "precondition: the first packet starts the interval the next two are "
         "held behind";
  post(sendA);
  post(sendB);
  ASSERT_EQ(viaA.load() + viaB.load(), 0)
      << "precondition: both packets are held, so each is released later by "
         "the timer rather than sent by the caller";

  ASSERT_TRUE(bothReleased.try_wait_for(std::chrono::seconds(5)))
      << "both held packets must be released";
  EXPECT_EQ(viaA.load(), 1)
      << "a held packet must leave through the callback it was handed, not "
         "one handed for a later packet";
  EXPECT_EQ(viaB.load(), 1)
      << "a held packet must leave through the callback it was handed";
}

TEST(PacingModifierTest, EachHeldPacketUsesItsOwnReceiveCallback) {
  // Long enough that both packets after the first are held, not delivered
  // inline.
  constexpr uint32_t kIntervalUs = 50000;
  auto config = std::make_unique<PacingModifierConfig>(
      createDataPacketFilter(ModifierConfig::ModifierFilter::Direction::Rx),
      std::nullopt,
      kIntervalUs);
  PacingModifier modifier(std::move(config));

  int immediateDeliveries = 0;
  ReceivePacketFn opener = [&](std::unique_ptr<IngressPacket>) {
    ++immediateDeliveries;
  };
  std::atomic<int> viaA{0};
  std::atomic<int> viaB{0};
  folly::Baton<> bothReleased;
  const auto countAndSignal = [&](std::atomic<int>& via) {
    ++via;
    if (viaA.load() + viaB.load() == 2) {
      bothReleased.post();
    }
  };
  ReceivePacketFn receiveA = [&](std::unique_ptr<IngressPacket>) {
    countAndSignal(viaA);
  };
  ReceivePacketFn receiveB = [&](std::unique_ptr<IngressPacket>) {
    countAndSignal(viaB);
  };

  modifier.modifyRxPacketIfMatches(
      opener, createIngressDataPacket(kDefaultFsn));
  ASSERT_EQ(immediateDeliveries, 1)
      << "precondition: the first packet starts the interval the next two are "
         "held behind";
  modifier.modifyRxPacketIfMatches(
      receiveA, createIngressDataPacket(kDefaultFsn));
  modifier.modifyRxPacketIfMatches(
      receiveB, createIngressDataPacket(kDefaultFsn));
  ASSERT_EQ(viaA.load() + viaB.load(), 0)
      << "precondition: both packets are held, so each is released later by "
         "the timer rather than delivered by the caller";

  ASSERT_TRUE(bothReleased.try_wait_for(std::chrono::seconds(5)))
      << "both held packets must be released";
  EXPECT_EQ(viaA.load(), 1)
      << "a held packet must be delivered through the callback it was handed, "
         "not one handed for a later packet";
  EXPECT_EQ(viaB.load(), 1)
      << "a held packet must be delivered through the callback it was handed";
}

TEST(PacingModifierTest, DelayedPacketKeepsItsPayloadAfterTheSourceIsFreed) {
  constexpr uint32_t kIntervalUs = 50000; // 50ms
  auto config = std::make_unique<PacingModifierConfig>(
      createDataPacketFilter(), std::nullopt, kIntervalUs);
  PacingModifier modifier(std::move(config));

  std::vector<uint8_t> wqeBuffer = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02};
  const std::vector<uint8_t> original = wqeBuffer;

  std::atomic<int> sendCount{0};
  std::vector<uint8_t> delivered(original.size());
  folly::Baton<> pacedPacketSent;
  SendPacketFn fn = [&](const folly::SocketAddress&,
                        std::unique_ptr<Packet> packet,
                        std::optional<uint32_t>) {
    if (++sendCount == 2) {
      auto* dataPacket = dynamic_cast<DataPacket*>(packet.get());
      EXPECT_NE(dataPacket, nullptr)
          << "the paced packet must arrive as the type that carries a payload, "
             "or the comparison at the end reports a mismatch when the real "
             "fault is that nothing was delivered";
      if (dataPacket != nullptr) {
        dataPacket->deliverPayload(delivered.data(), delivered.size());
      }
      pacedPacketSent.post();
    }
    return 1L;
  };
  const auto baseHeader = createBaseHeader();
  const auto dataHeader = createDataHeader();

  for (int i = 0; i < 2; ++i) {
    modifier.modifyTxPacketIfMatches(
        fn,
        std::make_unique<Modifier::TxPacketWithMetadata>(
            kDefaultRemoteAddr,
            std::make_unique<DataPacket>(
                baseHeader, dataHeader, wqeBuffer.data(), wqeBuffer.size())));
  }
  ASSERT_EQ(sendCount.load(), 1)
      << "precondition: the second packet must still be held, or the zeroing "
         "below reaches a buffer the modifier has already sent from";

  // The WQE buffer a real caller reuses once modifyTxPacket returns.
  for (auto& byte : wqeBuffer) {
    byte = 0;
  }

  ASSERT_TRUE(pacedPacketSent.try_wait_for(std::chrono::seconds(5)))
      << "precondition: the paced packet must arrive, or the comparison below "
         "reads a buffer nothing filled";
  EXPECT_EQ(delivered, original)
      << "a paced packet must carry the bytes it was handed; holding a view "
         "into the caller's buffer sends whatever landed there in the "
         "meantime";
}

TEST(PacingModifierTest, DestroyWithPendingCallbacks) {
  // Verify that destroying a PacingModifier with a packet still held does not
  // crash. ASAN/UBSAN will flag any use-after-free or undefined behavior.
  std::atomic<int> sendCount{0};
  SendPacketFn fn = [&](const folly::SocketAddress&,
                        std::unique_ptr<Packet>,
                        std::optional<uint32_t>) {
    sendCount++;
    return 1L;
  };
  const auto baseHeader = createBaseHeader();
  const auto dataHeader = createDataHeader();

  {
    auto config = std::make_unique<PacingModifierConfig>(
        createDataPacketFilter(), std::nullopt, 1000000); // 1 second
    PacingModifier modifier(std::move(config));

    modifier.modifyTxPacketIfMatches(
        fn,
        std::make_unique<Modifier::TxPacketWithMetadata>(
            kDefaultRemoteAddr,
            std::make_unique<DataPacket>(baseHeader, dataHeader, nullptr, 0)));
    EXPECT_EQ(sendCount.load(), 1);

    // Schedule a second packet that will be pending for ~1 second
    modifier.modifyTxPacketIfMatches(
        fn,
        std::make_unique<Modifier::TxPacketWithMetadata>(
            kDefaultRemoteAddr,
            std::make_unique<DataPacket>(baseHeader, dataHeader, nullptr, 0)));

    // modifier destroyed here with a packet still held
  }

  EXPECT_EQ(sendCount.load(), 1)
      << "the held packet must be discarded on destruction, not sent: it is "
         "released through a send function the caller is free to destroy once "
         "the modifier is gone";
}

TEST(CompositeModifierTest, DestroyWithPendingTimerCallbacks) {
  // Verify that destroying a CompositeModifier containing a PacingModifier
  // with a pending timer callback does not cause use-after-free.
  //
  // The bug: old CompositeModifier built chain functions as stack-local
  // folly::Functions in a recursive call. PacingModifier's timer callback
  // captured &sendPacketFn (= the stack-local chain function). After the
  // call returned, the timer fired and dereferenced the destroyed function.
  //
  // The fix stores chain functions as CompositeModifier members so they
  // outlive any timer callbacks. This test exercises that lifetime by
  // placing a MarkEcnModifier before a PacingModifier: the pacing timer
  // callback captures &txChainFns_[1] (the chain function that calls
  // the terminal send), and we destroy the composite while that timer
  // is still pending. ASAN/UBSAN will flag any use-after-free.
  std::atomic<int> sendCount{0};
  SendPacketFn fn = [&](const folly::SocketAddress&,
                        std::unique_ptr<Packet>,
                        std::optional<uint32_t>) {
    sendCount++;
    return 1L;
  };
  const auto baseHeader = createBaseHeader();
  const auto dataHeader = createDataHeader();

  {
    std::vector<std::unique_ptr<Modifier>> mods;
    mods.push_back(
        std::make_unique<MarkEcnModifier>(std::make_unique<ModifierConfig>(
            ModifierConfig::ModifierType::MarkEcn,
            createDataPacketFilter(),
            std::nullopt,
            "mark_ecn")));
    mods.push_back(
        std::make_unique<PacingModifier>(std::make_unique<PacingModifierConfig>(
            createDataPacketFilter(), std::nullopt, 1000000))); // 1 second
    CompositeModifier composite(std::move(mods));

    // First packet: passes through MarkEcn then PacingModifier sends
    // immediately (no prior packet).
    composite.modifyTxPacketIfMatches(
        fn,
        std::make_unique<Modifier::TxPacketWithMetadata>(
            kDefaultRemoteAddr,
            std::make_unique<DataPacket>(baseHeader, dataHeader, nullptr, 0)));
    EXPECT_EQ(sendCount.load(), 1);

    // Second packet: passes through MarkEcn, then PacingModifier schedules
    // a timer callback that captures the chain function reference.
    composite.modifyTxPacketIfMatches(
        fn,
        std::make_unique<Modifier::TxPacketWithMetadata>(
            kDefaultRemoteAddr,
            std::make_unique<DataPacket>(baseHeader, dataHeader, nullptr, 0)));

    // composite destroyed here with a packet still held by the pacing
    // modifier
  }

  // If we reach here without ASAN/UBSAN errors, the chain function lifetime
  // is correct: the pending work was discarded before the chain functions
  // were destroyed.
  EXPECT_EQ(sendCount.load(), 1)
      << "the held packet must be discarded on destruction, not pushed "
         "through chain functions that are themselves being destroyed";
}

TEST(CompositeModifierTest, EmptyModifiersPassThrough) {
  // Verify that a CompositeModifier with no child modifiers passes packets
  // straight through to the terminal send/receive functions without crashing.
  std::vector<std::unique_ptr<Modifier>> emptyMods;
  CompositeModifier composite(std::move(emptyMods));

  const auto baseHeader = createBaseHeader();
  const auto dataHeader = createDataHeader();

  // TX path: packet should be forwarded to the send function
  int sendCount = 0;
  auto sendFn = createCountingSendFn(sendCount);
  composite.modifyTxPacketIfMatches(
      sendFn,
      std::make_unique<Modifier::TxPacketWithMetadata>(
          kDefaultRemoteAddr,
          std::make_unique<DataPacket>(baseHeader, dataHeader, nullptr, 0)));
  EXPECT_EQ(sendCount, 1)
      << "Empty composite should forward TX packet to terminal send function";

  // RX path: packet should be forwarded to the receive function
  int receiveCount = 0;
  ReceivePacketFn receiveFn = [&](std::unique_ptr<IngressPacket>) {
    receiveCount++;
  };
  auto ingressPacket = std::make_unique<IngressPacket>(IngressPacket{
      std::make_unique<DataPacket>(baseHeader, dataHeader, nullptr, 0),
      kDefaultRemoteAddr,
      0});
  composite.modifyRxPacketIfMatches(receiveFn, std::move(ingressPacket));
  EXPECT_EQ(receiveCount, 1)
      << "Empty composite should forward RX packet to terminal receive function";
}

TEST(ModifierTest, CompositeReorderDelaysAcksByFsn) {
  constexpr int kFirstDelayUs = 5000;
  constexpr int kSecondDelayUs = 20000;

  // Parse the full JSON array using the same entry point as production
  // (MetaRoceVerbs.cpp:90)
  const auto jsonArrayStr = R"([
      {
          "name": "delay_first_ack_5ms",
          "type": "reorder",
          "filter": {
              "direction": "tx",
              "packet_type": "ack",
              "fields": {"cumulative_fsn": 0}
          },
          "modification_times": 1,
          "distance": {"unit": "time_us", "value": 5000}
      },
      {
          "name": "delay_acks_20ms",
          "type": "reorder",
          "filter": {
              "direction": "tx",
              "packet_type": "ack",
              "fields": {
                  "cumulative_fsn": {"op": "!=", "value": 0}
              }
          },
          "modification_times": 9999,
          "distance": {"unit": "time_us", "value": 20000}
      }
  ])";

  auto configs = ModifierConfig::buildModifierConfigsFromJson(jsonArrayStr);
  ASSERT_EQ(configs.size(), 2)
      << "JSON array should produce two modifier configs";

  // Build concrete modifiers using the same factory pattern as
  // createModifierFromConfig in MetaRoceVerbs.cpp:55-74
  std::vector<std::unique_ptr<Modifier>> modifiers;
  for (auto& config : configs) {
    ASSERT_EQ(config->type, ModifierConfig::ModifierType::Reorder)
        << "Both configs should be reorder type";
    modifiers.push_back(std::make_unique<ReorderModifier>(std::move(config)));
  }

  auto composite = CompositeModifier(std::move(modifiers));

  const auto baseHeader =
      createBaseHeader(BaseHeader::Opcode::OperationType::SEL_ACK);
  BitVector<AckHeader::kAckTrackerBits> fsnBitVector;

  std::mutex mtx;
  std::condition_variable cv;
  int packetsReceived = 0;
  std::chrono::high_resolution_clock::time_point lastReceiveTime;

  SendPacketFn sendFn = [&](const folly::SocketAddress&,
                            std::unique_ptr<Packet> packet,
                            std::optional<uint32_t>) {
    std::lock_guard<std::mutex> lock(mtx);
    auto* ackPacket = dynamic_cast<AckPacket*>(packet.get());
    EXPECT_NE(ackPacket, nullptr) << "Delivered packet should be an AckPacket";
    if (ackPacket == nullptr) {
      // Don't count a non-ACK delivery; doing so would corrupt the measured
      // delays. The wait below then times out, failing the test loudly.
      return 1L;
    }
    lastReceiveTime = std::chrono::high_resolution_clock::now();
    packetsReceived++;
    cv.notify_all();
    return 1L;
  };

  // Deliver one ACK at a time: the modifier defers delivery to a timer thread,
  // so overlapping submissions would race the first packet's in-flight timer.
  const auto submitAndMeasureDelayUs =
      [&](std::unique_ptr<AckPacket> ack) -> int64_t {
    const auto submitTime = std::chrono::high_resolution_clock::now();
    composite.modifyTxPacketIfMatches(
        sendFn,
        std::make_unique<Modifier::TxPacketWithMetadata>(
            kDefaultRemoteAddr, std::move(ack)));
    std::chrono::high_resolution_clock::time_point seen;
    {
      std::unique_lock<std::mutex> lock(mtx);
      const int expected = packetsReceived + 1;
      EXPECT_TRUE(cv.wait_for(lock, std::chrono::seconds(10), [&] {
        return packetsReceived >= expected;
      })) << "delayed ACK was not delivered";
      seen = lastReceiveTime;
    }
    return std::chrono::duration_cast<std::chrono::microseconds>(
               seen - submitTime)
        .count();
  };

  const auto firstDelayUs = submitAndMeasureDelayUs(
      std::make_unique<AckPacket>(
          baseHeader, createAckHeader(0, fsnBitVector)));
  const auto secondDelayUs = submitAndMeasureDelayUs(
      std::make_unique<AckPacket>(
          baseHeader, createAckHeader(5, fsnBitVector)));

  EXPECT_GE(firstDelayUs, 0.8 * kFirstDelayUs)
      << "cumulative_fsn=0 ACK should be delayed at least ~5ms";
  EXPECT_GE(secondDelayUs, 0.8 * kSecondDelayUs)
      << "cumulative_fsn!=0 ACK should be delayed at least ~20ms";
  EXPECT_GT(secondDelayUs, firstDelayUs)
      << "cumulative_fsn!=0 ACK delay should be larger than cumulative_fsn=0 ACK delay";
}
TEST(ReorderModifierTest, TimeDelayLandsCloseToConfigured) {
  // One packet at a time, so each sample is an independent
  // intercept-to-delivery rather than a batch release.
  constexpr uint32_t kConfiguredDelayUs = 10'000;
  constexpr size_t kPackets = 100;

  auto config = std::make_unique<ReorderModifierConfig>(
      ModifierConfig::ModifierFilter(
          ModifierConfig::ModifierFilter::Direction::Rx,
          ModifierConfig::ModifierFilter::PacketType::Data,
          folly::F14FastMap<std::string, FieldConditions>{}),
      std::nullopt,
      ReorderModifierConfig::Distance(
          kConfiguredDelayUs, ReorderModifierConfig::Distance::Unit::TimeUs),
      "reorder_modifier_rx");
  ReorderModifier modifier(std::move(config));

  std::atomic<uint64_t> deliveredAtNs{0};
  folly::Baton<> delivered;
  ReceivePacketFn receiveFn = [&](std::unique_ptr<IngressPacket>) {
    deliveredAtNs = getNowNs64();
    delivered.post();
  };

  std::vector<int64_t> overshootUs;
  overshootUs.reserve(kPackets);
  int64_t smallestOvershootNs = std::numeric_limits<int64_t>::max();
  for (size_t i = 0; i < kPackets; ++i) {
    delivered.reset();
    const uint64_t interceptedAtNs = getNowNs64();
    modifier.modifyRxPacketIfMatches(
        receiveFn, createIngressDataPacket(static_cast<uint32_t>(i)));
    ASSERT_TRUE(delivered.try_wait_for(std::chrono::seconds(30)))
        << "packet " << i << " was never delivered";
    const int64_t overshootNs =
        static_cast<int64_t>(deliveredAtNs.load() - interceptedAtNs) -
        static_cast<int64_t>(kConfiguredDelayUs) * 1000;
    smallestOvershootNs = std::min(smallestOvershootNs, overshootNs);
    overshootUs.push_back(overshootNs / 1000);
  }

  std::sort(overshootUs.begin(), overshootUs.end());
  reportOvershoot(
      "reorder_rx",
      OvershootMeasurement{
          .overshootUs = std::move(overshootUs),
          .delayUs = kConfiguredDelayUs});
  EXPECT_GE(smallestOvershootNs, 0)
      << "a reorder modifier must hold a packet for at least the delay it was "
         "configured with; delivering early understates the delay the caller "
         "asked to inject";
}

TEST(ReorderModifierTest, RxPacketBasedReorderWithModFilterTrueReorder) {
  // Filter matches every even FSN (fsn % 2 == 0) and delays by distance=1.
  // Unmatched packets pass through immediately, arriving before the held one,
  // producing true out-of-order delivery.
  auto config = std::make_unique<ReorderModifierConfig>(
      ModifierConfig::ModifierFilter(
          ModifierConfig::ModifierFilter::Direction::Rx,
          ModifierConfig::ModifierFilter::PacketType::Data,
          folly::F14FastMap<std::string, FieldConditions>{
              {"fsn",
               FieldConditions(FieldCondition(
                   ComparisonOperator::Mod, /*divisor=*/2, /*remainder=*/0))}}),
      std::nullopt,
      ReorderModifierConfig::Distance(
          1, ReorderModifierConfig::Distance::Unit::Packet),
      "reorder_modifier_rx");

  ReorderModifier modifier(std::move(config));

  std::vector<uint32_t> deliveredFsns;
  auto receiveFn = createDataFsnTracker(deliveredFsns);

  // Send 8 packets with FSNs 1-8.

  for (uint32_t i = 1; i <= 8; ++i) {
    modifier.modifyRxPacketIfMatches(receiveFn, createIngressDataPacket(i));
  }

  const std::vector<uint32_t> expectedOrder = {1, 3, 2, 5, 4, 7, 6};
  ASSERT_EQ(deliveredFsns.size(), expectedOrder.size())
      << "7 of 8 packets delivered; last matched packet (P8) stays queued";
  EXPECT_EQ(deliveredFsns, expectedOrder)
      << "Each even-FSN packet reordered after the next odd-FSN packet "
         "(true out-of-order delivery, distance=1 means 1 subsequent packet)";
}

TEST(ReorderModifierTest, RxPacketBasedReorderWithoutModFilterNoReorder) {
  // No filter — all packets match. distance=1, unlimited modification_times.
  // Without a filter, reorder modifier only induces delay: each subsequent
  // matched packet drains the previous one, so delivery order equals arrival
  // order. The last matched packet stays queued (no subsequent packet to
  // drain).
  auto config = std::make_unique<ReorderModifierConfig>(
      ModifierConfig::ModifierFilter(
          ModifierConfig::ModifierFilter::Direction::Rx,
          ModifierConfig::ModifierFilter::PacketType::Data,
          folly::F14FastMap<std::string, FieldConditions>{}),
      std::nullopt,
      ReorderModifierConfig::Distance(
          1, ReorderModifierConfig::Distance::Unit::Packet),
      "reorder_modifier_rx");

  ReorderModifier modifier(std::move(config));

  std::vector<uint32_t> deliveredFsns;
  auto receiveFn = createDataFsnTracker(deliveredFsns);

  // Send 8 packets with FSNs 1-8. All match (no filter).
  for (uint32_t i = 1; i <= 8; ++i) {
    modifier.modifyRxPacketIfMatches(receiveFn, createIngressDataPacket(i));
  }

  const std::vector<uint32_t> expectedOrder = {1, 2, 3, 4, 5, 6, 7};
  ASSERT_EQ(deliveredFsns.size(), expectedOrder.size())
      << "7 of 8 packets delivered; last matched packet (P8) stays queued";
  EXPECT_EQ(deliveredFsns, expectedOrder)
      << "Without a filter, all packets match and are delivered in order "
         "(delay, not reorder); last packet stays queued";
}

TEST(CorruptModifierTest, ParseJsonCorruptModifier) {
  const auto jsonStr = R"({
        "name": "corrupt_data",
        "type": "corrupt",
        "filter": {
            "direction": "tx",
            "packet_type": "data",
            "fields": {"fsn": 1, "path_id": 0}
        },
        "modification_times": 1,
        "position_to_corrupt": 5,
        "value_to_set": 171
    })";

  const auto modifierConfig =
      ModifierConfig::buildModifierConfigFromJson(folly::parseJson(jsonStr));
  const auto* corruptConfig =
      dynamic_cast<CorruptModifierConfig*>(modifierConfig.get());

  ASSERT_NE(corruptConfig, nullptr)
      << "buildModifierConfigFromJson should produce a CorruptModifierConfig";
  EXPECT_EQ(corruptConfig->name, "corrupt_data");
  EXPECT_EQ(corruptConfig->type, ModifierConfig::ModifierType::Corrupt);
  EXPECT_EQ(corruptConfig->modificationTimes, std::optional<uint32_t>(1));
  EXPECT_EQ(corruptConfig->positionToCorrupt, 5);
  ASSERT_TRUE(corruptConfig->valueToSet.has_value());
  EXPECT_EQ(*corruptConfig->valueToSet, 0xAB);
  EXPECT_EQ(
      corruptConfig->filter.direction,
      ModifierConfig::ModifierFilter::Direction::Tx);
  EXPECT_EQ(
      corruptConfig->filter.packetType,
      ModifierConfig::ModifierFilter::PacketType::Data);
}

TEST(CorruptModifierTest, CorruptModifierXorsFirstByte) {
  auto filter = createDataPacketFilter();
  auto config = std::make_unique<CorruptModifierConfig>(
      filter, std::nullopt /* unlimited */);
  CorruptModifier modifier(std::move(config));

  const auto baseHeader = createBaseHeader();
  const auto dataHeader = createDataHeader();

  // Serialize the original packet to capture its bytes
  auto originalPacket =
      std::make_unique<DataPacket>(baseHeader, dataHeader, nullptr, 0);
  auto origSize = originalPacket->getSizeBytes();
  auto origBuf = folly::IOBuf::create(origSize);
  folly::IOBufQueue origQueue;
  origQueue.append(std::move(origBuf));
  folly::io::QueueAppender origAppender(&origQueue, 0);
  originalPacket->serialize(origAppender);
  auto origFront = origQueue.front();
  std::vector<uint8_t> originalBytes(
      origFront->data(), origFront->data() + origFront->length());

  // Send through modifier and capture the corrupted packet
  bool packetSent = false;
  std::vector<uint8_t> corruptedBytes;
  std::optional<uint32_t> capturedIcrc;

  SendPacketFn fn = [&](const folly::SocketAddress&,
                        std::unique_ptr<Packet> packet,
                        std::optional<uint32_t> originalIcrc) {
    packetSent = true;
    capturedIcrc = originalIcrc;
    auto size = packet->getSizeBytes();
    auto buf = folly::IOBuf::create(size);
    folly::IOBufQueue queue;
    queue.append(std::move(buf));
    folly::io::QueueAppender appender(&queue, 0);
    packet->serialize(appender);
    auto front = queue.front();
    corruptedBytes.assign(front->data(), front->data() + front->length());
    return 1L;
  };

  modifier.modifyTxPacketIfMatches(
      fn,
      std::make_unique<Modifier::TxPacketWithMetadata>(
          folly::SocketAddress("fc00::1", 10000),
          std::make_unique<DataPacket>(baseHeader, dataHeader, nullptr, 0)));

  ASSERT_TRUE(packetSent) << "Packet should have been sent";
  ASSERT_EQ(corruptedBytes.size(), originalBytes.size())
      << "Corrupted packet should have the same size as the original";
  EXPECT_EQ(corruptedBytes[0], static_cast<uint8_t>(originalBytes[0] ^ 0xFF))
      << "First byte should be XOR'd with 0xFF";
  for (size_t i = 1; i < originalBytes.size(); ++i) {
    EXPECT_EQ(corruptedBytes[i], originalBytes[i])
        << "Byte at offset " << i << " should not be modified";
  }
  EXPECT_TRUE(capturedIcrc.has_value())
      << "Original ICRC should be provided with the corrupted packet";
}

TEST(WriteHeaderEditModifierTest, RxModifiesRkey) {
  constexpr uint32_t kOriginalRkey = 0xDEADBEEF;
  constexpr uint32_t kExpectedRkey = 0x12345678;
  constexpr uint64_t kMessageAddr = 0x1000;
  constexpr uint32_t kMessageLength = 64;

  auto filter =
      createDataPacketFilter(ModifierConfig::ModifierFilter::Direction::Rx, {});
  auto config = std::make_unique<WriteHeaderEditModifierConfig>(
      filter, 1, "rkey", kExpectedRkey);
  WriteHeaderEditModifier modifier(std::move(config));

  const auto baseHeader =
      createBaseHeader(BaseHeader::Opcode::OperationType::WRITE);
  const auto dataHeader = createDataHeader();
  const ReadWriteHeader rwHeader(kMessageAddr, kOriginalRkey, kMessageLength);

  auto ingressPacket = std::make_unique<IngressPacket>(IngressPacket{
      std::make_unique<ReadWritePacket>(
          baseHeader, dataHeader, rwHeader, nullptr, 0),
      kDefaultRemoteAddr,
      0});

  bool packetReceived = false;
  uint32_t receivedRkey = 0;
  uint64_t receivedAddr = 0;
  ReceivePacketFn receiveFn = [&](std::unique_ptr<IngressPacket> pkt) {
    packetReceived = true;
    auto* rwPacket = dynamic_cast<ReadWritePacket*>(pkt->packet.get());
    ASSERT_NE(rwPacket, nullptr)
        << "Received packet should be a ReadWritePacket";
    receivedRkey = rwPacket->getReadWriteHeader().getKey();
    receivedAddr = rwPacket->getReadWriteHeader().getMessageAddr();
  };

  modifier.modifyRxPacketIfMatches(receiveFn, std::move(ingressPacket));

  EXPECT_TRUE(packetReceived)
      << "Packet should be forwarded to receivePacketFn";
  EXPECT_EQ(receivedRkey, kExpectedRkey)
      << "RX modifier should corrupt rkey to the configured value";
  EXPECT_EQ(receivedAddr, kMessageAddr)
      << "Address should remain unchanged when target is rkey";
}

TEST(HeaderEditModifierTest, CanZeroMessageLengthInReadWriteHeader) {
  const auto jsonStr = R"({
        "name": "test_header_edit_message_length",
        "type": "header_edit",
        "filter": {
            "direction": "tx",
            "packet_type": "data"
        },
        "modification_times": 1,
        "header_type": "readwrite",
        "field_name": "message_length",
        "value_to_set": 0
    })";

  auto modifierConfig =
      ModifierConfig::buildModifierConfigFromJson(folly::parseJson(jsonStr));
  auto modifier = HeaderEditModifier(std::move(modifierConfig));

  constexpr uint64_t kMessageAddr = 0x1000;
  constexpr uint32_t kRkey = 0xABCD;
  constexpr uint32_t kOriginalLength = 64;

  const auto baseHeader =
      createBaseHeader(BaseHeader::Opcode::OperationType::WRITE);
  const auto dataHeader = createDataHeader();
  const ReadWriteHeader rwHeader(kMessageAddr, kRkey, kOriginalLength);

  bool packetSent = false;
  uint32_t modifiedLength = kOriginalLength;
  uint64_t modifiedAddr = 0;
  uint32_t modifiedKey = 0;
  SendPacketFn fn = [&](const folly::SocketAddress& /*addr*/,
                        std::unique_ptr<Packet> packet,
                        std::optional<uint32_t> /*originalIcrc*/) {
    packetSent = true;
    auto* rwPacket = dynamic_cast<ReadWritePacket*>(packet.get());
    EXPECT_NE(rwPacket, nullptr)
        << "header_edit must preserve the packet type so the RETH is inspectable";
    if (rwPacket) {
      modifiedLength = rwPacket->getReadWriteHeader().getMessageLength();
      modifiedAddr = rwPacket->getReadWriteHeader().getMessageAddr();
      modifiedKey = rwPacket->getReadWriteHeader().getKey();
    }
    return 1L;
  };

  modifier.modifyTxPacketIfMatches(
      fn,
      std::make_unique<Modifier::TxPacketWithMetadata>(
          kDefaultRemoteAddr,
          std::make_unique<ReadWritePacket>(
              baseHeader, dataHeader, rwHeader, nullptr, 0)));

  EXPECT_TRUE(packetSent) << "Packet should be forwarded to sendPacketFn";
  EXPECT_EQ(modifiedLength, 0u)
      << "header_edit message_length should zero the RETH DMA length so an "
         "out-of-window zero-byte write can be exercised";
  EXPECT_EQ(modifiedAddr, kMessageAddr)
      << "Editing message_length must leave the RETH virtual address intact";
  EXPECT_EQ(modifiedKey, kRkey)
      << "Editing message_length must leave the RETH rkey intact";
}

TEST(TruncateModifierTest, TruncatesDataPacketPayloadToZero) {
  const auto jsonStr = R"({
        "name": "test_truncate_payload",
        "type": "truncate",
        "filter": {
            "direction": "tx",
            "packet_type": "data"
        },
        "modification_times": 1
    })";

  auto modifierConfig =
      ModifierConfig::buildModifierConfigFromJson(folly::parseJson(jsonStr));
  auto modifier = TruncateModifier(std::move(modifierConfig));

  constexpr uint64_t kMessageAddr = 0x1000;
  constexpr uint32_t kRkey = 0xABCD;
  constexpr uint32_t kPayloadLen = 64;
  const std::array<char, kPayloadLen> payload{};

  const auto baseHeader =
      createBaseHeader(BaseHeader::Opcode::OperationType::WRITE);
  const auto dataHeader = createDataHeader();
  const ReadWriteHeader rwHeader(kMessageAddr, kRkey, kPayloadLen);

  bool packetSent = false;
  uint32_t sentPayloadSize = kPayloadLen;
  SendPacketFn fn = [&](const folly::SocketAddress& /*addr*/,
                        std::unique_ptr<Packet> packet,
                        std::optional<uint32_t> /*originalIcrc*/) {
    packetSent = true;
    auto* dataPacket = dynamic_cast<DataPacket*>(packet.get());
    EXPECT_NE(dataPacket, nullptr)
        << "truncate must preserve the packet type so the payload is inspectable";
    if (dataPacket) {
      sentPayloadSize = dataPacket->getPayloadSizeBytes();
    }
    return 1L;
  };

  modifier.modifyTxPacketIfMatches(
      fn,
      std::make_unique<Modifier::TxPacketWithMetadata>(
          kDefaultRemoteAddr,
          std::make_unique<ReadWritePacket>(
              baseHeader, dataHeader, rwHeader, payload.data(), kPayloadLen)));

  EXPECT_TRUE(packetSent) << "Packet should be forwarded to sendPacketFn";
  EXPECT_EQ(sentPayloadSize, 0u)
      << "truncate must drop the data payload so a header-only zero-byte write "
         "goes on the wire";
}

TEST(TruncateModifierTest, TruncatesOwnedPayloadToConfiguredSize) {
  const auto jsonStr = R"({
        "name": "test_truncate_payload_nonzero",
        "type": "truncate",
        "filter": {
            "direction": "tx",
            "packet_type": "data"
        },
        "modification_times": 1,
        "payload_size": 16
    })";

  auto modifierConfig =
      ModifierConfig::buildModifierConfigFromJson(folly::parseJson(jsonStr));
  auto modifier = TruncateModifier(std::move(modifierConfig));

  constexpr uint64_t kMessageAddr = 0x1000;
  constexpr uint32_t kRkey = 0xABCD;
  constexpr uint32_t kPayloadLen = 64;
  constexpr uint32_t kTruncatedLen = 16;
  const std::array<char, kPayloadLen> payload{};

  const auto baseHeader =
      createBaseHeader(BaseHeader::Opcode::OperationType::WRITE);
  const auto dataHeader = createDataHeader();
  const ReadWriteHeader rwHeader(kMessageAddr, kRkey, kPayloadLen);

  bool packetSent = false;
  uint32_t sentPayloadSize = kPayloadLen;
  SendPacketFn fn = [&](const folly::SocketAddress& /*addr*/,
                        std::unique_ptr<Packet> packet,
                        std::optional<uint32_t> /*originalIcrc*/) {
    packetSent = true;
    auto* dataPacket = dynamic_cast<DataPacket*>(packet.get());
    EXPECT_NE(dataPacket, nullptr)
        << "truncate must preserve the packet type so the payload is inspectable";
    if (dataPacket) {
      sentPayloadSize = dataPacket->getPayloadSizeBytes();
    }
    return 1L;
  };

  // An owned payload buffer exercises the buffer-trim path, not just the
  // borrowed-pointer path.
  modifier.modifyTxPacketIfMatches(
      fn,
      std::make_unique<Modifier::TxPacketWithMetadata>(
          kDefaultRemoteAddr,
          std::make_unique<ReadWritePacket>(
              baseHeader,
              dataHeader,
              rwHeader,
              folly::IOBuf::copyBuffer(payload.data(), kPayloadLen))));

  EXPECT_TRUE(packetSent) << "Packet should be forwarded to sendPacketFn";
  EXPECT_EQ(sentPayloadSize, kTruncatedLen)
      << "truncate must shrink the owned payload to the configured size so a "
         "shorter packet goes on the wire";
}

namespace {
ModifierConfig::ModifierFilter filterFor(
    ModifierConfig::ModifierFilter::Direction direction) {
  return ModifierConfig::ModifierFilter(
      direction,
      ModifierConfig::ModifierFilter::PacketType::Data,
      folly::F14FastMap<std::string, FieldConditions>{});
}

std::unique_ptr<PacingModifier> pacingModifierFor(
    ModifierConfig::ModifierFilter::Direction direction) {
  return std::make_unique<PacingModifier>(
      std::make_unique<PacingModifierConfig>(
          filterFor(direction),
          /*modificationTimes=*/std::nullopt,
          /*intervalUs=*/1000));
}

std::unique_ptr<ReorderModifier> reorderModifierFor(
    ModifierConfig::ModifierFilter::Direction direction,
    ReorderModifierConfig::Distance::Unit unit) {
  return std::make_unique<ReorderModifier>(
      std::make_unique<ReorderModifierConfig>(
          filterFor(direction),
          /*modificationTimes=*/std::nullopt,
          ReorderModifierConfig::Distance(1000, unit)));
}
} // namespace

// The receive path drops its single-reader assumption on this answer, so a
// modifier that can deliver from its timer thread has to say so.
TEST(ModifierDeferredReceiveTest, OnlyAnRxTimerModifierReportsDeferredReceive) {
  using Direction = ModifierConfig::ModifierFilter::Direction;
  using Unit = ReorderModifierConfig::Distance::Unit;

  EXPECT_TRUE(pacingModifierFor(Direction::Rx)->mayDeferReceive())
      << "a pacing modifier filtering rx delivers from its timer thread";
  EXPECT_TRUE(
      reorderModifierFor(Direction::Rx, Unit::TimeUs)->mayDeferReceive())
      << "a time-distance reorder modifier filtering rx delivers from its "
         "timer thread";
  EXPECT_TRUE(
      reorderModifierFor(Direction::Rx, Unit::Packet)->mayDeferReceive())
      << "a packet-distance reorder modifier drains on the caller's thread, but "
         "is reported anyway: over-reporting costs one uncontended lock, and "
         "the unit is configuration rather than a property of the type";

  EXPECT_FALSE(pacingModifierFor(Direction::Tx)->mayDeferReceive())
      << "a tx filter never reaches the receive path";
  EXPECT_FALSE(
      reorderModifierFor(Direction::Tx, Unit::TimeUs)->mayDeferReceive())
      << "a tx filter never reaches the receive path";

  auto drop = std::make_unique<DropModifier>(std::make_unique<ModifierConfig>(
      ModifierConfig::ModifierType::Drop,
      filterFor(Direction::Rx),
      /*modificationTimes=*/std::nullopt));
  EXPECT_FALSE(drop->mayDeferReceive())
      << "a modifier that owns no timer always delivers on the caller's thread";
}

TEST(ModifierDeferredReceiveTest, ACompositeReportsWhateverAnyLinkReports) {
  using Direction = ModifierConfig::ModifierFilter::Direction;
  using Unit = ReorderModifierConfig::Distance::Unit;

  std::vector<std::unique_ptr<Modifier>> deferring;
  deferring.push_back(
      std::make_unique<DropModifier>(std::make_unique<ModifierConfig>(
          ModifierConfig::ModifierType::Drop,
          filterFor(Direction::Tx),
          /*modificationTimes=*/std::nullopt)));
  deferring.push_back(reorderModifierFor(Direction::Rx, Unit::TimeUs));
  EXPECT_TRUE(CompositeModifier(std::move(deferring)).mayDeferReceive())
      << "one deferring link is enough to cost the reader thread its exclusion";

  std::vector<std::unique_ptr<Modifier>> notDeferring;
  notDeferring.push_back(pacingModifierFor(Direction::Tx));
  notDeferring.push_back(reorderModifierFor(Direction::Tx, Unit::TimeUs));
  EXPECT_FALSE(CompositeModifier(std::move(notDeferring)).mayDeferReceive())
      << "a chain that only defers sends leaves the receive path alone";
}

namespace {
// Counts stops and records, at each destruction, how many links had already
// been stopped. A chain runs tx forward and rx backward, so a link destroyed
// while any sibling's timer can still fire is reachable from that timer.
struct TeardownLog {
  int stops{0};
  std::vector<int> stopsSeenAtDestruction;
};

class TeardownOrderModifier : public Modifier {
 public:
  TeardownOrderModifier(
      std::unique_ptr<ModifierConfig> config,
      TeardownLog& log)
      : Modifier(std::move(config)), log_(log) {}

  ~TeardownOrderModifier() override {
    log_.stopsSeenAtDestruction.push_back(log_.stops);
  }

  ssize_t modifyTxPacket(
      SendPacketFn&,
      std::unique_ptr<TxPacketWithMetadata> txPacketData) override {
    const auto& data = *CHECK_NOTNULL(txPacketData.get());
    return CHECK_NOTNULL(data.packet.get())->getSizeBytes();
  }
  void modifyRxPacket(ReceivePacketFn&, std::unique_ptr<IngressPacket>)
      override {}
  void onPacketSent() override {}
  void stopTimers() override {
    ++log_.stops;
  }

 private:
  TeardownLog& log_;
};
} // namespace

TEST(CompositeModifierTest, EveryTimerStopsBeforeAnyLinkIsDestroyed) {
  TeardownLog log;
  {
    std::vector<std::unique_ptr<Modifier>> mods;
    mods.reserve(3);
    for (int i = 0; i < 3; ++i) {
      mods.push_back(
          std::make_unique<TeardownOrderModifier>(
              std::make_unique<ModifierConfig>(
                  ModifierConfig::ModifierType::Drop,
                  createDataPacketFilter(),
                  std::nullopt),
              log));
    }
    CompositeModifier composite(std::move(mods));
  }

  EXPECT_EQ(log.stops, 3) << "every link's timers must be stopped";
  ASSERT_EQ(log.stopsSeenAtDestruction.size(), 3u)
      << "precondition: every link recorded what it saw at destruction";
  for (int seen : log.stopsSeenAtDestruction) {
    EXPECT_EQ(seen, 3)
        << "a link was destroyed while a sibling's timer could still fire; the "
           "rx chain calls modifiers_[i - 1], which by then is freed";
  }
}

// The rx chain is the direction element order cannot save: it runs backward, so
// a deferring link reaches a sibling destroyed before it. Exercises the real
// modifiers rather than the counters above; ASAN is the assertion.
TEST(CompositeModifierTest, DestroyWithPendingRxTimerCallbacks) {
  ReceivePacketFn receiveFn = [](std::unique_ptr<IngressPacket>) {};
  const auto baseHeader = createBaseHeader();
  const auto dataHeader = createDataHeader();

  std::vector<std::unique_ptr<Modifier>> mods;
  mods.push_back(
      std::make_unique<MarkEcnModifier>(std::make_unique<ModifierConfig>(
          ModifierConfig::ModifierType::MarkEcn,
          createDataPacketFilter(ModifierConfig::ModifierFilter::Direction::Rx),
          std::nullopt,
          "mark_ecn")));
  mods.push_back(
      std::make_unique<ReorderModifier>(std::make_unique<ReorderModifierConfig>(
          createDataPacketFilter(ModifierConfig::ModifierFilter::Direction::Rx),
          std::nullopt,
          ReorderModifierConfig::Distance(
              1000000, ReorderModifierConfig::Distance::Unit::TimeUs))));
  auto composite = std::make_unique<CompositeModifier>(std::move(mods));

  // Enters at the last link, which defers onto its timer; the callback it
  // leaves pending chains back to the MarkEcn link at index 0.
  for (int i = 0; i < 4; ++i) {
    composite->modifyRxPacketIfMatches(
        receiveFn,
        std::make_unique<IngressPacket>(IngressPacket{
            std::make_unique<DataPacket>(baseHeader, dataHeader, nullptr, 0),
            kDefaultRemoteAddr,
            0}));
  }

  composite.reset();
  SUCCEED();
}

} // namespace libsoftmetaroce
