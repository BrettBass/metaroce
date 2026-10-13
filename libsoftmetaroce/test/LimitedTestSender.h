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

#include <folly/Synchronized.h>
#include <folly/coro/AsyncScope.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/Task.h>
#include <folly/io/async/AsyncUDPSocket.h>
#include <folly/io/async/EventBase.h>
#include <libsoftmetaroce/Ecn.h>
#include <libsoftmetaroce/PacketModifierSocket.h>
#include <libsoftmetaroce/QpArbiter.h>
#include <libsoftmetaroce/Sender.h>
#include <libsoftmetaroce/codec/AckHeader.h>
#include <libsoftmetaroce/codec/BaseHeader.h>

namespace libsoftmetaroce {

/*
 * This class is used to provide some limits to the regular Sender class
 * for testing.  In particular, it can limit the socket count and avoid the
 * sendLoop in Socket.h.  To keep the sendLoop, provide the ENABLE_SENDLOOP
 * option to the constructor.  The default is DISABLE_SENDLOOP.
 */
class LimitedTestSender : public Sender {
 public:
  static constexpr unsigned int kMaxSocketsNoLimit = 0;
  static constexpr bool kDisableSendLoop = true;
  static constexpr bool kEnableSendLoop = false;

  // constructor using user-provided UDP socketmaker and modifier
  explicit LimitedTestSender(
      QpArbiterIface& arbiter,
      std::function<std::pair<
          std::unique_ptr<folly::AsyncUDPSocket>,
          std::unique_ptr<Modifier>>(folly::EventBase*)> socketMaker,
      uint16_t sendToPort = kDefaultSendToPort,
      bool disableSendLoop = true,
      uint32_t sendBatch = 1)
      : Sender(
            arbiter,
            std::move(socketMaker),
            sendToPort,
            !disableSendLoop,
            /*numSendingThreads=*/1,
            sendBatch) {}

  // constructor using default UDP socket from folly, implemented in terms of
  // the first
  explicit LimitedTestSender(
      QpArbiterIface& arbiter,
      uint16_t sendToPort = kDefaultSendToPort,
      bool disableSendLoop = true,
      uint32_t sendBatch = 1)
      : LimitedTestSender(
            arbiter,
            [](folly::EventBase* evb) {
              return std::make_pair(
                  std::make_unique<folly::AsyncUDPSocket>(evb), nullptr);
            },
            sendToPort,
            disableSendLoop,
            sendBatch) {}

  // set the (smaller) limit to the max socket cnt. Setup only: call it before
  // the sender is shared between threads.
  void setLimitNumSockets(unsigned int limit) {
    if (limit < kMaxNumSockets) {
      sockMaxLimit_ = limit; // zero = no limit
    }
  }

 protected:
  // Some unit tests expect no new sockets to be created, so the limit is
  // smaller than Sender's.
  size_t socketLimit() const override {
    return sockMaxLimit_ == kMaxSocketsNoLimit ? kMaxNumSockets : sockMaxLimit_;
  }

 private:
  unsigned int sockMaxLimit_{1};
}; // LimitedTestSender

} // namespace libsoftmetaroce
