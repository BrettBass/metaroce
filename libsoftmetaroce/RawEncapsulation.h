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

#pragma once

#include <folly/IPAddress.h>
#include <folly/MacAddress.h>
#include <libsoftmetaroce/Encapsulation.h>
#include <stdexcept>

namespace libsoftmetaroce {

// Encapsulation for raw (AF_PACKET) sockets. Raw sockets deliver MAC
// addresses, while the rest of the stack uses folly::SocketAddress (IPv6).
// The default EUI-64 mapping is bijective; an encapsulation may override it,
// but every mapping must satisfy:
//
//      resolveDstMac(resolveSrcAddr(mac)) == mac   for all valid MACs
//      resolveSrcAddr(resolveDstMac(ip))  == ip    for all valid IPs
class RawEncapsulation : public Encapsulation {
 public:
  // Update the destination MAC for per-packet addressing.
  virtual void setDstMac(folly::MacAddress dstMac) noexcept = 0;

  // Derive the destination MAC from an EUI-64 link-local address.
  [[nodiscard]] virtual folly::MacAddress resolveDstMac(
      const folly::IPAddressV6& remoteAddr) const {
    return macFromLinkLocal(remoteAddr);
  }

  // Derive a source EUI-64 link-local address from an incoming source MAC.
  [[nodiscard]] virtual folly::IPAddressV6 resolveSrcAddr(
      folly::MacAddress srcMac) const noexcept {
    return linkLocalFromMac(srcMac);
  }

  static folly::MacAddress macFromLinkLocal(const folly::IPAddressV6& address) {
    auto maybeMac = address.getMacAddressFromLinkLocal();
    if (!maybeMac) {
      throw std::runtime_error(
          address.str() + " is not a link-local address with an embedded MAC");
    }
    return *maybeMac;
  }

  static folly::IPAddressV6 linkLocalFromMac(folly::MacAddress mac) noexcept {
    const auto bytes = mac.bytes();
    const folly::ByteArray16 address = {
        {0xfe,
         0x80,
         0,
         0,
         0,
         0,
         0,
         0,
         static_cast<uint8_t>(bytes[0] ^ 0x02),
         bytes[1],
         bytes[2],
         0xff,
         0xfe,
         bytes[3],
         bytes[4],
         bytes[5]}};
    return folly::IPAddressV6(address);
  }
};

} // namespace libsoftmetaroce
