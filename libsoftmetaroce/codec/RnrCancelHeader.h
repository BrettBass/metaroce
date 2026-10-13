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

class RnrCancelHeader : public Header {
 public:
  static constexpr size_t kRnrCancelHeaderSizeBytes = 8;
  RnrCancelHeader() = default;
  explicit RnrCancelHeader(folly::io::Cursor& cursor);
  explicit RnrCancelHeader(
      uint8_t flowsAvailable,
      uint8_t tag,
      uint16_t postedCsn,
      uint32_t rateHints)
      : flowsAvailable_(flowsAvailable),
        tag_(tag),
        postedCsn_(postedCsn),
        rateHints_(rateHints) {}

  bool operator==(const RnrCancelHeader& other) const {
    return flowsAvailable_ == other.flowsAvailable_ && tag_ == other.tag_ &&
        postedCsn_ == other.postedCsn_ && rateHints_ == other.rateHints_;
  }

  [[nodiscard]] uint8_t getFlowsAvailable() const {
    return flowsAvailable_;
  }
  [[nodiscard]] uint8_t getTag() const {
    return tag_;
  }
  [[nodiscard]] uint16_t getPostedCsn() const {
    return postedCsn_;
  }
  [[nodiscard]] uint32_t getRateHints() const {
    return rateHints_;
  }

  // Setters for packet modifiers
  void setFlowsAvailable(uint8_t flowsAvailable) {
    flowsAvailable_ = flowsAvailable;
  }
  void setTag(uint8_t tag) {
    tag_ = tag;
  }
  void setPostedCsn(uint16_t postedCsn) {
    postedCsn_ = postedCsn;
  }
  void setRateHints(uint32_t rateHints) {
    rateHints_ = rateHints;
  }

  void write(folly::io::QueueAppender& appender) const override;

  class RnrCancelHeaderBuilder {
   public:
    RnrCancelHeaderBuilder&& setFlowsAvailable(uint8_t flowsAvailable);
    RnrCancelHeaderBuilder&& setTag(uint8_t tag);
    RnrCancelHeaderBuilder&& setPostedCsn(uint16_t postedCsn);
    RnrCancelHeaderBuilder&& setRateHints(uint32_t rateHints);
    RnrCancelHeader build() &&;
    explicit RnrCancelHeaderBuilder() = default;

   private:
    std::optional<uint8_t> maybeFlowsAvailable_;
    std::optional<uint8_t> maybeTag_;
    std::optional<uint16_t> maybePostedCsn_;
    std::optional<uint32_t> maybeRateHints_;
  };

 private:
  uint8_t flowsAvailable_{0};
  uint8_t tag_{0};
  uint16_t postedCsn_{0};
  uint32_t rateHints_{kDefaultRateHintGbps};
};

} // namespace libsoftmetaroce
