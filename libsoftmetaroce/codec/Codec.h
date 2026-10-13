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

#include <folly/io/Cursor.h>
#include <folly/io/IOBufQueue.h>
#include <memory>

namespace libsoftmetaroce {

class Encapsulation;
class Packet;

// Translates packets to and from one wire format, in both directions.
class Codec {
 public:
  virtual ~Codec() = default;

  // Bytes serialize() writes exactly; callers size their buffer from it.
  [[nodiscard]] virtual size_t getSerializedSize(
      const Packet& packet) const = 0;

  virtual void serialize(
      const Packet& packet,
      folly::io::QueueAppender& appender) const = 0;

  // Reads a packet, or nullptr to drop it; the encapsulation bounds the
  // payload.
  [[nodiscard]] virtual std::unique_ptr<Packet> parse(
      folly::io::Cursor& cursor,
      const Encapsulation& encapsulation) const = 0;
};

} // namespace libsoftmetaroce
