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

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <libsoftmetaroce/Constants.h>
#include <libsoftmetaroce/PacketSocketFactory.h>
#include <libsoftmetaroce/WireProfiles.h>

namespace libsoftmetaroce {

namespace {

ibv_gid_entry makeUdpEntry() {
  ibv_gid_entry entry{};
  entry.gid_type = kGidTypeMetaRoceUdp;
  // Set a global IPv6 address (not link-local).
  inet_pton(AF_INET6, "2001:db8::1", entry.gid.raw);
  return entry;
}

auto defaultSocketMaker() {
  return [](folly::EventBase* evb) {
    return std::make_pair(
        std::make_unique<folly::AsyncUDPSocket>(evb),
        std::unique_ptr<Modifier>(nullptr));
  };
}

// No interface carries this, so passing it shows the UDP policy does not
// consult the value at all.
constexpr uint16_t kMtuNoInterfaceCarries = 65535;

} // namespace

TEST(MakePacketModifierSocket, UdpEntryCreatesSocket) {
  folly::EventBase evb;
  const auto entry = makeUdpEntry();
  auto socket = makePacketModifierSocket(&evb, entry, defaultSocketMaker());
  ASSERT_NE(socket, nullptr)
      << "UDP entry should produce a valid PacketModifierSocket";
  // UDP sockets expose the underlying AsyncUDPSocket via getSocket().
  EXPECT_NE(socket->getSocket(), nullptr)
      << "UDP-backed socket should have a non-null AsyncUDPSocket";
  socket->close();
}

TEST(MakePacketModifierSocket, DefaultEntryCreatesUdpSocket) {
  folly::EventBase evb;
  const ibv_gid_entry entry{};
  auto socket = makePacketModifierSocket(&evb, entry, defaultSocketMaker());
  ASSERT_NE(socket, nullptr) << "zero gid_type should fall through to UDP path";
  EXPECT_NE(socket->getSocket(), nullptr)
      << "default entry should create a UDP-backed socket";
  socket->close();
}

// The Ethernet path (kGidTypeMetaRoceEthernet) requires CAP_NET_RAW for
// AF_PACKET socket creation. It is tested end-to-end by the vmtests
// (lsmr_ethernet, lsmr_ethernet_multiplane) which run as root.

TEST(MakePacketModifierSocket, InvalidIfindexThrows) {
  // This test bypasses verbs construction, so install the Ethernet profile
  // explicitly; otherwise lookup falls back to UDP.
  installWireProfiles();
  folly::EventBase evb;
  ibv_gid_entry entry{};
  entry.gid_type = kGidTypeMetaRoceEthernet;
  // A well-formed link-local gid leaves the ifindex as the entry's only defect.
  inet_pton(AF_INET6, "fe80::211:22ff:fe33:4455", entry.gid.raw);
  entry.ndev_ifindex = 99999;
  EXPECT_THROW(
      makePacketModifierSocket(&evb, entry, defaultSocketMaker()),
      std::runtime_error)
      << "unresolvable ifindex should throw";
}

TEST(ValidateMtuForEntry, ReachesTheDefaultRegistrysUdpPolicy) {
  ibv_gid_entry entry{};
  entry.gid_type = kGidTypeMetaRoceUdp;

  EXPECT_TRUE(validateMtuForEntry(entry, kMtuNoInterfaceCarries))
      << "the production entry point should resolve the UDP policy through the "
         "default registry";
}

} // namespace libsoftmetaroce
