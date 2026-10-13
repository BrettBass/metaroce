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

#include <folly/MacAddress.h>
#include <libsoftmetaroce/PacketSocket.h>
#include <libsoftmetaroce/RawEncapsulation.h>
#include <libsoftmetaroce/RawEthernetSocket.h>
#include <memory>

namespace libsoftmetaroce {

// PacketSocket implementation backed by a RawEthernetSocket (AF_PACKET).
// Uses a RawEncapsulation for MAC <-> IPv6 address resolution.
class RawPacketSocket : public PacketSocket,
                        private RawEthernetSocket::ReadCallback {
 public:
  // rawEncap is a non-owning pointer to the encapsulation owned by
  // PacketModifierSocket. It must outlive this socket.
  RawPacketSocket(
      std::unique_ptr<RawEthernetSocket> rawSocket,
      RawEncapsulation* rawEncap,
      uint16_t ethertype);

  void resumeRead(PacketSocket::ReadCallback* cb) override;
  void pauseRead() override;
  ssize_t write(
      const folly::SocketAddress& dest,
      std::unique_ptr<folly::IOBuf> data) override;
  void bind(const folly::SocketAddress& address) override;
  void close() override;
  int fd() const override;

  void setRcvBuf(int bytes) override;
  int getRcvBuf() override;

 private:
  // RawEthernetSocket::ReadCallback
  void onRawDataAvailable(
      const struct sockaddr_ll& source,
      std::unique_ptr<folly::IOBuf> data) noexcept override;

  std::unique_ptr<RawEthernetSocket> rawSocket_;
  RawEncapsulation* rawEncap_;
  uint16_t ethertype_;
  PacketSocket::ReadCallback* readCallback_{nullptr};
};

} // namespace libsoftmetaroce
