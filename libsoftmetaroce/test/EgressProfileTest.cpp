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

#include <gtest/gtest.h>
#include <net/if.h>
#include <libsoftmetaroce/Constants.h>
#include <libsoftmetaroce/EgressProfile.h>
#include <libsoftmetaroce/EthernetEncapsulation.h>
#include <libsoftmetaroce/UdpEncapsulation.h>
#include <libsoftmetaroce/codec/BaseHeader.h>
#include <libsoftmetaroce/codec/DataHeader.h>
#include <libsoftmetaroce/codec/MetaRoceCodec.h>
#include <libsoftmetaroce/codec/Packet.h>
#include <libsoftmetaroce/codec/ReadWriteHeader.h>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace libsoftmetaroce {

namespace {

ibv_gid_entry makeEthernetEntry(const char* ifName) {
  ibv_gid_entry entry{};
  entry.gid_type = kGidTypeMetaRoceEthernet;
  entry.ndev_ifindex = if_nametoindex(ifName);
  return entry;
}

EgressProfile makeMetaRoceEthernetTestProfile() {
  return EgressProfile{
      .socketKind = SocketKind::RAW,
      .makeCodec = [] { return std::make_unique<MetaRoceCodec>(); },
      .makeEncapsulation =
          [](const ibv_gid_entry&) {
            return std::make_unique<EthernetEncapsulation>(
                folly::MacAddress("02:00:00:00:00:01"),
                folly::MacAddress("02:00:00:00:00:02"));
          },
      .validateMtu = [](const ibv_gid_entry&, uint16_t) { return false; },
      .supportsAlternateReceiver = false,
      .rawEthertype = kDefaultMetaRoceEthertype,
  };
}

EgressProfile makeMetaRoceUdpTestProfile() {
  return EgressProfile{
      .socketKind = SocketKind::UDP,
      .makeCodec = [] { return std::make_unique<MetaRoceCodec>(); },
      .makeEncapsulation =
          [](const ibv_gid_entry&) {
            return std::make_unique<UdpEncapsulation>();
          },
      .validateMtu = [](const ibv_gid_entry&, uint16_t) { return true; },
      .supportsAlternateReceiver = true,
  };
}

// Exceeds any interface MTU, so only a policy that ignores it accepts it.
constexpr uint16_t kMtuNoInterfaceCarries = 65535;

} // namespace

TEST(EgressRegistry, SeedsTheBuiltInUdpProfile) {
  const EgressRegistry registry;

  EXPECT_EQ(
      registry.getProfile(kGidTypeMetaRoceUdp).socketKind, SocketKind::UDP)
      << "the UDP profile should select a kernel UDP socket";
  EXPECT_TRUE(
      registry.getProfile(kGidTypeMetaRoceUdp).supportsAlternateReceiver)
      << "UDP demuxes by port, so a second receiver on an alternate port is allowed";
}

TEST(EgressRegistry, RegisteringTheEthernetProfileReplacesTheUdpFallback) {
  EgressRegistry registry;
  ASSERT_EQ(
      registry.getProfile(kGidTypeMetaRoceEthernet).socketKind, SocketKind::UDP)
      << "an unregistered gid type falls back to UDP";

  ASSERT_TRUE(registry.registerProfile(
      kGidTypeMetaRoceEthernet, makeMetaRoceEthernetProfile()))
      << "the Ethernet gid_type is unclaimed until this registers it";

  EXPECT_EQ(
      registry.getProfile(kGidTypeMetaRoceEthernet).socketKind, SocketKind::RAW)
      << "the Ethernet profile should select a raw AF_PACKET socket";
  EXPECT_FALSE(
      registry.getProfile(kGidTypeMetaRoceEthernet).supportsAlternateReceiver)
      << "raw AF_PACKET lacks port demux, so a second receiver would duplicate frames";
}

TEST(EgressRegistry, RegisteringTheEthernetProfileTwiceLeavesItInPlace) {
  EgressRegistry registry;
  ASSERT_TRUE(registry.registerProfile(
      kGidTypeMetaRoceEthernet, makeMetaRoceEthernetProfile()))
      << "the initial registration should succeed";

  EXPECT_FALSE(registry.registerProfile(
      kGidTypeMetaRoceEthernet, makeMetaRoceEthernetProfile()))
      << "duplicate registration should be rejected";
  EXPECT_EQ(
      registry.getProfile(kGidTypeMetaRoceEthernet).socketKind, SocketKind::RAW)
      << "and must leave the registered profile in place, not unseat it";
}

TEST(EgressRegistry, RegistersAndLooksUpACustomProfile) {
  EgressRegistry registry;
  constexpr uint32_t kCustomGidType = 4243;

  ASSERT_TRUE(registry.registerProfile(
      kCustomGidType, makeMetaRoceEthernetTestProfile()))
      << "a gid_type no built-in claims should register";
  EXPECT_EQ(registry.getProfile(kCustomGidType).socketKind, SocketKind::RAW)
      << "the custom profile should be looked up by its gid_type";
  EXPECT_FALSE(registry.getProfile(kCustomGidType).supportsAlternateReceiver)
      << "the custom profile leaves alternate receivers disabled";
}

TEST(EgressRegistry, KeepsTheProfileAGidTypeAlreadyHas) {
  EgressRegistry registry;
  constexpr uint32_t kCustomGidType = 4243;
  ASSERT_TRUE(registry.registerProfile(
      kCustomGidType, makeMetaRoceEthernetTestProfile()))
      << "the registry should accept a gid_type no built-in profile claims";

  EXPECT_FALSE(
      registry.registerProfile(kCustomGidType, makeMetaRoceUdpTestProfile()))
      << "a duplicate registration must not overwrite the existing profile";
  EXPECT_EQ(registry.getProfile(kCustomGidType).socketKind, SocketKind::RAW)
      << "the first profile should survive the rejected registration";
  EXPECT_FALSE(registry.registerProfile(
      kGidTypeMetaRoceUdp, makeMetaRoceUdpTestProfile()))
      << "a built-in gid_type is registered too, so it cannot be replaced";
}

TEST(EgressRegistry, RejectsAProfileMissingAnyFactory) {
  // Each of the three factories is required, and registration is the last place
  // an absent one can be reported instead of throwing from the socket path.
  const std::vector<std::pair<std::string, EgressProfile>> profiles = [] {
    std::vector<std::pair<std::string, EgressProfile>> result;
    auto withoutCodec = makeMetaRoceUdpTestProfile();
    withoutCodec.makeCodec = nullptr;
    result.emplace_back("codec", std::move(withoutCodec));
    auto withoutEncapsulation = makeMetaRoceUdpTestProfile();
    withoutEncapsulation.makeEncapsulation = nullptr;
    result.emplace_back("encapsulation", std::move(withoutEncapsulation));
    auto withoutValidator = makeMetaRoceUdpTestProfile();
    withoutValidator.validateMtu = nullptr;
    result.emplace_back("mtu validator", std::move(withoutValidator));
    return result;
  }();

  for (const auto& [absent, profile] : profiles) {
    EgressRegistry registry;
    EXPECT_FALSE(registry.registerProfile(4244, profile))
        << "a profile without a " << absent << " factory must be rejected";
  }
}

TEST(EgressRegistry, RejectsARawProfileClaimingAnAlternateReceiver) {
  EgressRegistry registry;
  EgressProfile rawWithAlternate = makeMetaRoceEthernetTestProfile();
  rawWithAlternate.supportsAlternateReceiver = true;

  EXPECT_FALSE(registry.registerProfile(4245, std::move(rawWithAlternate)))
      << "a raw profile cannot support an alternate receiver: AF_PACKET has no "
         "port demux, so both receivers would see every frame";
}

TEST(EgressRegistry, RejectsARawProfileMissingAnEthertype) {
  EgressRegistry registry;
  EgressProfile rawWithoutEthertype = makeMetaRoceEthernetTestProfile();
  rawWithoutEthertype.rawEthertype = std::nullopt;

  EXPECT_FALSE(registry.registerProfile(4246, std::move(rawWithoutEthertype)))
      << "a raw profile must supply the EtherType used to construct its socket";
}

TEST(EgressRegistryValidateMtu, UdpGidTypeAcceptsAnyMtu) {
  ibv_gid_entry entry{};
  entry.gid_type = kGidTypeMetaRoceUdp;
  EXPECT_TRUE(EgressRegistry().validateMtu(entry, kMtuNoInterfaceCarries))
      << "UDP entries delegate fragmentation to the kernel";
}

TEST(EgressRegistryValidateMtu, UnregisteredGidTypeAcceptsAnyMtu) {
  const ibv_gid_entry entry{};
  EXPECT_TRUE(EgressRegistry().validateMtu(entry, kMtuNoInterfaceCarries))
      << "a gid_type nothing registered should fall back to the UDP policy";
}

// Unregistered GID types fall back to the permissive UDP MTU policy, so this
// fixture registers Ethernet to exercise its MTU policy.
class EgressRegistryEthernetMtuTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(registry_.registerProfile(
        kGidTypeMetaRoceEthernet, makeMetaRoceEthernetProfile()))
        << "a fresh registry must accept the Ethernet profile";
  }

  EgressRegistry registry_;
};

TEST_F(EgressRegistryEthernetMtuTest, InvalidIfindexPasses) {
  ibv_gid_entry entry{};
  entry.gid_type = kGidTypeMetaRoceEthernet;
  entry.ndev_ifindex = 99999;
  EXPECT_TRUE(registry_.validateMtu(entry, kMtuNoInterfaceCarries))
      << "unresolvable ifindex should not block -- validated at send time";
}

TEST_F(EgressRegistryEthernetMtuTest, LoopbackSmallMtuPasses) {
  const auto entry = makeEthernetEntry("lo");
  ASSERT_NE(entry.ndev_ifindex, 0u) << "loopback interface should exist";
  constexpr uint16_t smallMtu = 256;
  EXPECT_TRUE(registry_.validateMtu(entry, smallMtu))
      << "256 bytes fits within loopback MTU (65536)";
}

TEST_F(EgressRegistryEthernetMtuTest, WriteImmPacketAndFramingBytesBoundMtu) {
  const auto entry = makeEthernetEntry("lo");
  ASSERT_NE(entry.ndev_ifindex, 0u) << "loopback interface should exist";
  const ReadWritePacket writeImmPacket(
      BaseHeader(
          BaseHeader::Opcode(BaseHeader::Opcode::OperationType::WRITE_IMM_P),
          /*version=*/1,
          /*flowId=*/0,
          /*dstQp=*/1,
          /*srcQp=*/1),
      DataHeader(
          /*fsn=*/0,
          /*una=*/0,
          /*tag=*/0,
          /*ackRequest=*/false,
          /*isRetransmit=*/false,
          /*isTimeout=*/false,
          /*posn=*/0,
          /*csn=*/0,
          /*msn=*/0,
          /*rateHints=*/0,
          /*timestamp=*/0),
      ReadWriteHeader(/*messageAddr=*/0, /*key=*/0, /*messageLength=*/0),
      /*payloadAddress=*/nullptr,
      /*payloadSizeBytes=*/0,
      /*maybeImmData=*/0);
  constexpr size_t kLoopbackMtuBytes = 65536;
  constexpr size_t kFramingBytesWithinMtu = 8;
  const size_t maximumDirectQpMtuBytes =
      kLoopbackMtuBytes - writeImmPacket.getSizeBytes();
  const size_t maximumFramedQpMtuBytes =
      maximumDirectQpMtuBytes - kFramingBytesWithinMtu;
  ASSERT_LT(maximumDirectQpMtuBytes, std::numeric_limits<uint16_t>::max())
      << "precondition: the first rejected MTU must not overflow uint16_t";

  EXPECT_TRUE(registry_.validateMtu(
      entry, static_cast<uint16_t>(maximumDirectQpMtuBytes)))
      << "the largest QP MTU whose WRITE_IMM packet fits must be accepted";
  EXPECT_FALSE(registry_.validateMtu(
      entry, static_cast<uint16_t>(maximumDirectQpMtuBytes + 1)))
      << "one byte beyond the WRITE_IMM boundary must be rejected";
  EXPECT_TRUE(validateRawMtu(
      entry,
      static_cast<uint16_t>(maximumFramedQpMtuBytes),
      kFramingBytesWithinMtu))
      << "the exact QP MTU boundary must include profile framing";
  EXPECT_FALSE(validateRawMtu(
      entry,
      static_cast<uint16_t>(maximumFramedQpMtuBytes + 1),
      kFramingBytesWithinMtu))
      << "one byte beyond the framed boundary must be rejected";
}

TEST_F(EgressRegistryEthernetMtuTest, LoopbackOversizedMtuFails) {
  const auto entry = makeEthernetEntry("lo");
  ASSERT_NE(entry.ndev_ifindex, 0u) << "loopback interface should exist";
  constexpr uint16_t oversizedMtu = 65535;
  EXPECT_FALSE(registry_.validateMtu(entry, oversizedMtu))
      << "65535 exceeds loopback capacity after subtracting headers";
}

} // namespace libsoftmetaroce
