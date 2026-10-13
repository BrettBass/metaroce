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

#include <folly/io/async/AsyncSocketException.h>
#include <folly/io/async/AsyncUDPSocket.h>
#include <gmock/gmock.h>
#include <libsoftmetaroce/modifier/Modifier.h>
#include <libsoftmetaroce/test/MockUDPSocket.h>
#include <cerrno>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace libsoftmetaroce {

// Records the source port each socket it makes is bound to, refusing as
// already in use each port `inUse` names.
struct SourcePortRecorder {
  std::vector<uint16_t> bound;
  std::function<bool(uint16_t)> inUse = [](uint16_t) { return false; };

  std::function<std::pair<
      std::unique_ptr<folly::AsyncUDPSocket>,
      std::unique_ptr<Modifier>>(folly::EventBase*)>
  socketMaker() {
    return [this](folly::EventBase* evb) {
      auto socket = std::make_unique<testing::NiceMock<MockUDPSocket>>(evb);
      ON_CALL(*socket, bindImpl(testing::_))
          .WillByDefault([this](const folly::SocketAddress& address) {
            if (inUse(address.getPort())) {
              throw folly::AsyncSocketException(
                  folly::AsyncSocketException::NOT_OPEN,
                  "test: address already in use",
                  EADDRINUSE);
            }
            bound.push_back(address.getPort());
          });
      return std::make_pair(
          std::unique_ptr<folly::AsyncUDPSocket>(std::move(socket)),
          std::unique_ptr<Modifier>{});
    };
  }
};

} // namespace libsoftmetaroce
