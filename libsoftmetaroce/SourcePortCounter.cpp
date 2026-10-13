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

#include <libsoftmetaroce/SourcePortCounter.h>

#include <folly/Random.h>
#include <folly/SocketAddress.h>
#include <folly/io/async/AsyncSocketException.h>
#include <libsoftmetaroce/NextFreeId.h>
#include <cerrno>

namespace libsoftmetaroce {

SourcePortCounter::SourcePortCounter()
    : SourcePortCounter(
          static_cast<uint16_t>(
              kFirstPort + folly::Random::rand32(kPortCount))) {}

SourcePortCounter::SourcePortCounter(uint16_t firstPort) : next_{firstPort} {}

void SourcePortCounter::bindNext(
    PacketModifierSocket& socket,
    const folly::IPAddress& localAddr) {
  // A port is in use where the bind trying it fails with EADDRINUSE.
  const auto bound = nextFreeId(
      next_,
      kFirstPort,
      std::numeric_limits<uint16_t>::max(),
      [&socket, &localAddr](uint32_t port) {
        try {
          socket.bind(
              folly::SocketAddress(localAddr, static_cast<uint16_t>(port)));
          return false;
        } catch (const folly::AsyncSocketException& e) {
          if (e.getErrno() != EADDRINUSE) {
            throw;
          }
          return true;
        }
      });
  if (!bound.has_value()) {
    throw folly::AsyncSocketException(
        folly::AsyncSocketException::NOT_OPEN,
        "every source port from 49152 to 65535 is in use on " + localAddr.str(),
        EADDRINUSE);
  }
}

} // namespace libsoftmetaroce
