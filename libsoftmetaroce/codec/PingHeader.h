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
#include <ostream>

namespace libsoftmetaroce {

enum class PingErrorState : uint8_t {
  NO_ERROR = 0x00,
  /// Destination QP in Base Header of Ping Packet not found
  QP_NOT_PRESENT_OR_UNKNOWN = 0x01,
  /// Tag in Ping Header of Ping Packet not found for the Destination QP
  TAG_NOT_PRESENT_OR_UNKNOWN = 0x02,
  /// Unsupported in LSMR
  TAG_NOT_ASSOCIATED_WITH_QP = 0x03,
  /// Destination QP does not have the source IP address in its list of Remote
  /// Addresses
  SOURCE_ADDRESS_NOT_ASSOCIATED_WITH_QP = 0x04,
  /// Unsupported in LSMR
  DEST_ADDRESS_NOT_ASSOCIATED_WITH_PATH = 0x05,
  /// The flow id in the Base Header of a Ping Packet is not supported or
  /// too large
  FLOW_ID_UNSUPPORTED_OR_TOO_LARGE = 0x06,

};

inline std::ostream& operator<<(std::ostream& os, PingErrorState state) {
  const int stateInt = static_cast<int>(state);
  os << stateInt << ":";
  switch (state) {
    case PingErrorState::NO_ERROR:
      return os << "NO_ERROR";
    case PingErrorState::QP_NOT_PRESENT_OR_UNKNOWN:
      return os << "QP_NOT_PRESENT_OR_UNKNOWN";
    case PingErrorState::TAG_NOT_PRESENT_OR_UNKNOWN:
      return os << "TAG_NOT_PRESENT_OR_UNKNOWN";
    case PingErrorState::TAG_NOT_ASSOCIATED_WITH_QP:
      return os << "TAG_NOT_ASSOCIATED_WITH_QP";
    case PingErrorState::SOURCE_ADDRESS_NOT_ASSOCIATED_WITH_QP:
      return os << "SOURCE_ADDRESS_NOT_ASSOCIATED_WITH_QP";
    case PingErrorState::DEST_ADDRESS_NOT_ASSOCIATED_WITH_PATH:
      return os << "DEST_ADDRESS_NOT_ASSOCIATED_WITH_PATH";
    case PingErrorState::FLOW_ID_UNSUPPORTED_OR_TOO_LARGE:
      return os << "FLOW_ID_UNSUPPORTED_OR_TOO_LARGE";
  }
  return os << "UNKNOWN";
}

class PingHeader : public Header {
 public:
  constexpr static size_t kPingHeaderSizeBytes = 8;
  PingHeader() = default;
  explicit PingHeader(folly::io::Cursor& cursor);
  PingHeader(
      uint8_t tag,
      uint8_t errorState,
      uint16_t supportedFlowCount,
      uint16_t activePortSet = 0)
      : tag_(tag),
        errorState_(errorState),
        supportedFlowCount_(supportedFlowCount),
        activePortSet_(activePortSet) {}

  uint8_t getTag() const {
    return tag_;
  }
  PingErrorState getErrorState() const {
    return static_cast<PingErrorState>(errorState_);
  }
  uint16_t getSupportedFlowCount() const {
    return supportedFlowCount_;
  }
  uint16_t getActivePortSet() const {
    return activePortSet_;
  }

  void setTag(uint8_t tag) {
    tag_ = tag;
  }
  void setErrorState(PingErrorState errorState) {
    errorState_ = static_cast<uint8_t>(errorState);
  }
  void setSupportedFlowCount(uint16_t supportedFlowCount) {
    supportedFlowCount_ = supportedFlowCount;
  }
  void setActivePortSet(uint16_t activePortSet) {
    activePortSet_ = activePortSet;
  }

  void write(folly::io::QueueAppender& appender) const override;

  class PingHeaderBuilder {
   public:
    PingHeaderBuilder&& setTag(const uint8_t& tag);
    PingHeaderBuilder&& setErrorState(const PingErrorState& errorState);
    PingHeaderBuilder&& setSupportedFlowCount(const uint16_t& support);
    PingHeaderBuilder&& setActivePortSet(uint16_t activePortSet);
    PingHeader build() &&;
    explicit PingHeaderBuilder() = default;

   private:
    std::optional<uint8_t> maybeTag_;
    std::optional<uint8_t> maybeErrorState_;
    std::optional<uint16_t> maybeSupportedFlowCount_;
    std::optional<uint16_t> maybeActivePortSet_;
  };

 private:
  uint8_t tag_{0};
  uint8_t errorState_{0};
  uint16_t supportedFlowCount_{0};
  uint16_t reserved_{0};
  uint16_t activePortSet_{0};
};

} // namespace libsoftmetaroce
