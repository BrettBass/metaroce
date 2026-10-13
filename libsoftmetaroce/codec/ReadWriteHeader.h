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

class ReadWriteHeader : public Header {
 public:
  static constexpr size_t kReadWriteHeaderSizeBytes = 16;
  ReadWriteHeader() = default;
  explicit ReadWriteHeader(folly::io::Cursor& cursor);
  ReadWriteHeader(uint64_t messageAddr, uint32_t key, uint32_t messageLength)
      : messageAddr_(messageAddr), key_(key), messageLength_(messageLength) {}
  bool operator==(const ReadWriteHeader& other) const {
    bool eq = (messageAddr_ == other.messageAddr_);
    eq &= (key_ == other.key_);
    eq &= (messageLength_ == other.messageLength_);
    return eq;
  }

  [[nodiscard]] uint64_t getMessageAddr() const {
    return messageAddr_;
  }
  [[nodiscard]] uint32_t getKey() const {
    return key_;
  }
  [[nodiscard]] uint32_t getMessageLength() const {
    return messageLength_;
  }

  // These setters are for altering headers that were constructed using the
  // builder, to support testing of incorrect headers.
  void setMessageAddr(uint64_t messageAddr) {
    messageAddr_ = messageAddr;
  }
  void setKey(uint32_t key) {
    key_ = key;
  }
  void setMessageLength(uint32_t messageLength) {
    messageLength_ = messageLength;
  }

  void write(folly::io::QueueAppender& appender) const override;

  class ReadWriteHeaderBuilder {
   public:
    ReadWriteHeaderBuilder&& setMessageAddr(uint64_t messageAddr);
    ReadWriteHeaderBuilder&& setKey(uint32_t key);
    ReadWriteHeaderBuilder&& setMessageLength(uint32_t messageLength);
    ReadWriteHeader build() &&;
    explicit ReadWriteHeaderBuilder() = default;

   private:
    std::optional<uint64_t> maybeMessageAddr_;
    std::optional<uint32_t> maybeKey_;
    std::optional<uint32_t> maybeMessageLength_;
  };

 private:
  uint64_t messageAddr_{0};
  uint32_t key_{0};
  uint32_t messageLength_;
};

} // namespace libsoftmetaroce
