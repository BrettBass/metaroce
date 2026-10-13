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

#include <folly/io/IOBuf.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/EventHandler.h>
#include <linux/if_packet.h>
#include <libsoftmetaroce/Constants.h>
#include <memory>
#include <string>

namespace libsoftmetaroce {

// Raw AF_PACKET socket filtered to a caller-selected EtherType.
// Integrates with an EventBase for async read via EventHandler.
class RawEthernetSocket : public folly::EventHandler {
 public:
  class ReadCallback {
   public:
    virtual void onRawDataAvailable(
        const struct sockaddr_ll& source,
        std::unique_ptr<folly::IOBuf> data) noexcept = 0;
    virtual ~ReadCallback() = default;
  };

  // Creates and binds an AF_PACKET socket to the named interface.
  // Throws on failure (missing CAP_NET_RAW, bad interface name, etc.).
  RawEthernetSocket(
      folly::EventBase* evb,
      const std::string& ifName,
      uint16_t ethertype);

  ~RawEthernetSocket() override;

  RawEthernetSocket(const RawEthernetSocket&) = delete;
  RawEthernetSocket& operator=(const RawEthernetSocket&) = delete;
  RawEthernetSocket(RawEthernetSocket&&) = delete;
  RawEthernetSocket& operator=(RawEthernetSocket&&) = delete;

  // Begin delivering received frames to cb. Must be called from the
  // EventBase thread.
  void startReading(ReadCallback* cb);

  // Stop delivering frames.
  void stopReading();

  // Send a raw Ethernet frame. The buffer must contain the complete
  // frame starting with the Ethernet header.
  ssize_t write(
      const struct sockaddr_ll& dest,
      std::unique_ptr<folly::IOBuf> data);

  [[nodiscard]] int fd() const {
    return fd_;
  }

  [[nodiscard]] int ifIndex() const {
    return ifIndex_;
  }

 private:
  void handlerReady(uint16_t events) noexcept override;

  int fd_{-1};
  int ifIndex_{0};
  int ifMtu_{1500};
  ReadCallback* readCallback_{nullptr};
};

} // namespace libsoftmetaroce
