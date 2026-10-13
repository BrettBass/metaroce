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

#include <fmt/core.h>
#include <net/if.h>
#include <libsoftmetaroce/EgressProfile.h>
#include <libsoftmetaroce/PacketSocketFactory.h>
#include <libsoftmetaroce/RawPacketSocket.h>
#include <libsoftmetaroce/UdpPacketSocket.h>
#include <stdexcept>

namespace libsoftmetaroce {

std::unique_ptr<PacketModifierSocket> makePacketModifierSocket(
    folly::EventBase* evb,
    const ibv_gid_entry& entry,
    const std::function<std::pair<
        std::unique_ptr<folly::AsyncUDPSocket>,
        std::unique_ptr<Modifier>>(folly::EventBase*)>& socketMaker) {
  const auto profile = defaultEgressRegistry().getProfile(entry.gid_type);

  char ifName[IF_NAMESIZE]{};
  if (profile.socketKind == SocketKind::RAW) {
    if (!if_indextoname(entry.ndev_ifindex, ifName)) {
      throw std::runtime_error(
          fmt::format(
              "if_indextoname failed for ifindex {}", entry.ndev_ifindex));
    }
  } else if (profile.socketKind != SocketKind::UDP) {
    // Guards a SocketKind added later that this factory does not yet assemble.
    throw std::runtime_error(
        fmt::format(
            "unhandled SocketKind {} for gid_type {}",
            static_cast<int>(profile.socketKind),
            entry.gid_type));
  }

  auto encapsulation = profile.makeEncapsulation(entry);
  auto codec = profile.makeCodec();
  if (!encapsulation) {
    throw std::runtime_error(
        fmt::format("no encapsulation for gid_type {}", entry.gid_type));
  }
  if (!codec) {
    throw std::runtime_error(
        fmt::format("no codec for gid_type {}", entry.gid_type));
  }

  if (profile.socketKind == SocketKind::RAW) {
    auto* rawEncapsulation =
        dynamic_cast<RawEncapsulation*>(encapsulation.get());
    if (!rawEncapsulation) {
      throw std::runtime_error("raw profile requires a RawEncapsulation");
    }
    if (!profile.rawEthertype) {
      throw std::runtime_error(
          fmt::format(
              "raw profile for gid_type {} has no EtherType", entry.gid_type));
    }
    const auto ethertype = *profile.rawEthertype;
    // The socket borrows the encapsulation moved in below; member order
    // destroys the socket first.
    auto packetSocket = std::make_unique<RawPacketSocket>(
        std::make_unique<RawEthernetSocket>(evb, ifName, ethertype),
        rawEncapsulation,
        ethertype);
    return std::make_unique<PacketModifierSocket>(
        evb,
        std::move(packetSocket),
        std::move(encapsulation),
        std::move(codec));
  }

  auto [udpSocket, modifier] = socketMaker(evb);
  auto packetSocket = std::make_unique<UdpPacketSocket>(std::move(udpSocket));
  return std::make_unique<PacketModifierSocket>(
      evb,
      std::move(packetSocket),
      std::move(encapsulation),
      std::move(codec),
      std::move(modifier));
}

bool validateMtuForEntry(const ibv_gid_entry& entry, uint16_t mtu) {
  return defaultEgressRegistry().validateMtu(entry, mtu);
}

} // namespace libsoftmetaroce
