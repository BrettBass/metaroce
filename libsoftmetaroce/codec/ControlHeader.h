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

#include <libsoftmetaroce/codec/Header.h>
#include <cstdint>
#include <optional>

namespace libsoftmetaroce {

enum class ControlPacketType : uint16_t {
  PING = 0x0000,
  PING_RESPONSE = 0x0001,
};

class ControlHeader : public Header {
 public:
  constexpr static size_t kControlHeaderSizeBytes = 8;
  ControlHeader() = default;
  explicit ControlHeader(folly::io::Cursor& cursor);
  ControlHeader(uint16_t type, uint16_t reserved, uint32_t timestamp)
      : type_(type), reserved_(reserved), timestamp_(timestamp) {}

  ControlPacketType getType() const {
    return static_cast<ControlPacketType>(type_);
  }
  uint16_t getReserved() const {
    return reserved_;
  }
  uint16_t getPingId() const {
    return reserved_;
  }
  uint32_t getTimestamp() const {
    return timestamp_;
  }

  // Setters for packet modifiers
  void setType(uint16_t type) {
    type_ = type;
  }
  void setReserved(uint16_t reserved) {
    reserved_ = reserved;
  }
  void setPingId(uint16_t pingId) {
    reserved_ = pingId;
  }
  void setTimestamp(uint32_t timestamp) {
    timestamp_ = timestamp;
  }

  void write(folly::io::QueueAppender& appender) const override;

  class ControlHeaderBuilder {
   public:
    ControlHeaderBuilder&& setType(const uint16_t& type);
    ControlHeaderBuilder&& setReserved(const uint16_t& reserved);
    ControlHeaderBuilder&& setPingId(const uint16_t& pingId);
    ControlHeaderBuilder&& setTimestamp(const uint32_t& timestamp);
    ControlHeader build() &&;
    explicit ControlHeaderBuilder() = default;

   private:
    std::optional<uint16_t> maybeType_;
    std::optional<uint16_t> maybeReserved_;
    std::optional<uint32_t> maybeTimestamp_;
  };

 private:
  uint16_t type_{0};
  uint16_t reserved_{0};
  uint32_t timestamp_{0};
};

} // namespace libsoftmetaroce
