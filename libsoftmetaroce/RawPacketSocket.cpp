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

#include <libsoftmetaroce/RawPacketSocket.h>

#include <fmt/core.h>
#include <folly/String.h>
#include <linux/if_packet.h>
#include <libsoftmetaroce/Ecn.h>

namespace libsoftmetaroce {

RawPacketSocket::RawPacketSocket(
    std::unique_ptr<RawEthernetSocket> rawSocket,
    RawEncapsulation* rawEncap,
    uint16_t ethertype)
    : rawSocket_(std::move(rawSocket)),
      rawEncap_(rawEncap),
      ethertype_(ethertype) {}

void RawPacketSocket::resumeRead(PacketSocket::ReadCallback* cb) {
  readCallback_ = cb;
  rawSocket_->startReading(this);
}

void RawPacketSocket::pauseRead() {
  rawSocket_->stopReading();
}

ssize_t RawPacketSocket::write(
    const folly::SocketAddress& /* dest */,
    std::unique_ptr<folly::IOBuf> data) {
  // Destination MAC resolution and Ethernet header construction are
  // handled by PacketModifierSocket before the buffer reaches here.
  struct sockaddr_ll ll = {};
  ll.sll_family = AF_PACKET;
  ll.sll_protocol = htons(ethertype_);
  ll.sll_ifindex = rawSocket_->ifIndex();
  // sll_addr is not set: for SOCK_RAW the kernel sends the frame as-is
  // from the buffer, which already contains the Ethernet header with
  // the destination MAC set by PacketModifierSocket before framing.

  return rawSocket_->write(ll, std::move(data));
}

void RawPacketSocket::bind(const folly::SocketAddress& /* address */) {
  // Raw sockets are bound to their interface in the RawEthernetSocket
  // constructor, so this override has no work.
}

void RawPacketSocket::close() {
  rawSocket_.reset();
}

int RawPacketSocket::fd() const {
  return rawSocket_->fd();
}

void RawPacketSocket::setRcvBuf(int bytes) {
  if (bytes > 0) {
    int value = bytes;
    if (::setsockopt(
            rawSocket_->fd(), SOL_SOCKET, SO_RCVBUF, &value, sizeof(value)) !=
        0) {
      throw std::runtime_error(
          fmt::format(
              "failed to set SO_RCVBUF on raw socket: {}",
              folly::errnoStr(errno)));
    }
  }
}

int RawPacketSocket::getRcvBuf() {
  int value;
  socklen_t optLen = sizeof(value);
  if (::getsockopt(rawSocket_->fd(), SOL_SOCKET, SO_RCVBUF, &value, &optLen) !=
      0) {
    throw std::runtime_error(
        fmt::format(
            "failed to get SO_RCVBUF on raw socket: {}",
            folly::errnoStr(errno)));
  }
  return value;
}

void RawPacketSocket::onRawDataAvailable(
    const struct sockaddr_ll& source,
    std::unique_ptr<folly::IOBuf> data) noexcept {
  folly::MacAddress srcMac = folly::MacAddress::fromBinary(
      folly::ByteRange(source.sll_addr, folly::MacAddress::SIZE));
  auto srcIp = rawEncap_->resolveSrcAddr(srcMac);
  folly::SocketAddress sourceAddress(srcIp, 0);

  // Ethernet frames have no IP TOS field; assume ECT(0) so the
  // receiver does not reject the packet for missing ECN capability.
  readCallback_->onPacketData(std::move(data), sourceAddress, kEcnECT0);
}

} // namespace libsoftmetaroce
