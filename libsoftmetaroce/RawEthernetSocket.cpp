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

#include <libsoftmetaroce/RawEthernetSocket.h>

#include <fmt/core.h>
#include <folly/String.h>
#include <glog/logging.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace libsoftmetaroce {

RawEthernetSocket::RawEthernetSocket(
    folly::EventBase* evb,
    const std::string& ifName,
    uint16_t ethertype)
    : folly::EventHandler(evb) {
  fd_ = ::socket(AF_PACKET, SOCK_RAW, htons(ethertype));
  if (fd_ < 0) {
    throw std::runtime_error(
        fmt::format(
            "failed to create AF_PACKET socket: {} (missing CAP_NET_RAW?)",
            folly::errnoStr(errno)));
  }

  ifIndex_ = static_cast<int>(::if_nametoindex(ifName.c_str()));
  if (ifIndex_ == 0) {
    ::close(fd_);
    fd_ = -1;
    throw std::runtime_error(fmt::format("unknown interface: {}", ifName));
  }

  struct sockaddr_ll sll = {};
  sll.sll_family = AF_PACKET;
  sll.sll_protocol = htons(ethertype);
  sll.sll_ifindex = ifIndex_;

  int ret =
      ::bind(fd_, reinterpret_cast<const struct sockaddr*>(&sll), sizeof(sll));
  if (ret < 0) {
    auto err = errno;
    ::close(fd_);
    fd_ = -1;
    throw std::runtime_error(
        fmt::format(
            "failed to bind AF_PACKET socket to {}: {}",
            ifName,
            folly::errnoStr(err)));
  }

  struct ifreq ifr = {};
  strncpy(ifr.ifr_name, ifName.c_str(), IFNAMSIZ - 1);
  if (::ioctl(fd_, SIOCGIFMTU, &ifr) == 0) {
    ifMtu_ = ifr.ifr_mtu;
  }

  initHandler(evb, folly::NetworkSocket::fromFd(fd_));
  LOG(INFO) << "RawEthernetSocket bound to " << ifName
            << " (ifindex=" << ifIndex_ << ", mtu=" << ifMtu_
            << ", ethertype=0x" << std::hex << ethertype << std::dec << ")";
}

RawEthernetSocket::~RawEthernetSocket() {
  if (readCallback_) {
    unregisterHandler();
    readCallback_ = nullptr;
  }
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

void RawEthernetSocket::startReading(ReadCallback* cb) {
  CHECK(cb);
  readCallback_ = cb;
  registerHandler(folly::EventHandler::READ | folly::EventHandler::PERSIST);
}

void RawEthernetSocket::stopReading() {
  if (readCallback_) {
    unregisterHandler();
    readCallback_ = nullptr;
  }
}

ssize_t RawEthernetSocket::write(
    const struct sockaddr_ll& dest,
    std::unique_ptr<folly::IOBuf> data) {
  if (data == nullptr) {
    errno = EINVAL;
    return -1;
  }
  auto& frame = *data;
  const size_t maxFrameSize = static_cast<size_t>(ifMtu_) + kEthernetHeaderSize;
  if (frame.computeChainDataLength() > maxFrameSize) {
    LOG(ERROR) << "Frame too large for interface MTU: "
               << frame.computeChainDataLength() << " bytes exceeds "
               << maxFrameSize << " (mtu=" << ifMtu_ << ")";
    errno = EMSGSIZE;
    return -1;
  }
  frame.coalesce();
  return ::sendto(
      fd_,
      frame.data(),
      frame.length(),
      0,
      reinterpret_cast<const struct sockaddr*>(&dest),
      sizeof(dest));
}

void RawEthernetSocket::handlerReady(uint16_t events) noexcept {
  if (!(events & folly::EventHandler::READ)) {
    VLOG(4) << "RawEthernetSocket::handlerReady with non-READ event: "
            << events;
    return;
  }

  struct sockaddr_ll srcAddr = {};
  socklen_t addrLen = sizeof(srcAddr);
  auto buf = folly::IOBuf::create(kMaxPacketSizeBytes);
  // IOBuf::create throws rather than returning null.
  auto& frame = *CHECK_NOTNULL(buf.get());
  ssize_t bytesRead = ::recvfrom(
      fd_,
      frame.writableData(),
      frame.capacity(),
      MSG_DONTWAIT,
      reinterpret_cast<struct sockaddr*>(&srcAddr),
      &addrLen);

  if (bytesRead <= 0) {
    if (bytesRead < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
      LOG(ERROR) << "RawEthernetSocket recvfrom error: "
                 << folly::errnoStr(errno);
    }
    return;
  }

  frame.append(static_cast<size_t>(bytesRead));
  // The handler runs only while registered, and registration implies a
  // callback.
  CHECK_NOTNULL(readCallback_)->onRawDataAvailable(srcAddr, std::move(buf));
}

} // namespace libsoftmetaroce
