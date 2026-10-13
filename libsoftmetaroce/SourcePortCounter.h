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
#include <libsoftmetaroce/PacketModifierSocket.h>
#include <cstdint>
#include <limits>

namespace libsoftmetaroce {

// Not thread-safe; its owner serializes the binds.
class SourcePortCounter {
 public:
  static constexpr uint16_t kFirstPort = 0xC000;
  static constexpr uint32_t kPortCount =
      std::numeric_limits<uint16_t>::max() - kFirstPort + 1;

  // Starts at a random port, so processes on one host begin apart.
  SourcePortCounter();
  explicit SourcePortCounter(uint16_t firstPort);

  void bindNext(
      PacketModifierSocket& socket,
      const folly::IPAddress& localAddr);

 private:
  uint32_t next_;
};

} // namespace libsoftmetaroce
