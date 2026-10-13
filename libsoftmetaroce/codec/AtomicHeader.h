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

class AtomicHeader : public Header {
 public:
  static constexpr size_t kAtomicHeaderSizeBytes = 28;
  AtomicHeader() = default;
  explicit AtomicHeader(folly::io::Cursor& cursor);
  AtomicHeader(
      uint64_t remoteAddr,
      uint32_t rkey,
      uint64_t swapOrAddData,
      uint64_t compareData)
      : remoteAddr_(remoteAddr),
        rkey_(rkey),
        swapOrAddData_(swapOrAddData),
        compareData_(compareData) {}
  bool operator==(const AtomicHeader& other) const {
    bool eq = (remoteAddr_ == other.remoteAddr_);
    eq &= (rkey_ == other.rkey_);
    eq &= (swapOrAddData_ == other.swapOrAddData_);
    eq &= (compareData_ == other.compareData_);
    return eq;
  }

  [[nodiscard]] uint64_t getRemoteAddr() const {
    return remoteAddr_;
  }
  [[nodiscard]] uint32_t getRkey() const {
    return rkey_;
  }
  [[nodiscard]] uint64_t getSwapOrAddData() const {
    return swapOrAddData_;
  }
  [[nodiscard]] uint64_t getCompareData() const {
    return compareData_;
  }

  void write(folly::io::QueueAppender& appender) const override;

  class AtomicHeaderBuilder {
   public:
    AtomicHeaderBuilder&& setRemoteAddr(uint64_t remoteAddr);
    AtomicHeaderBuilder&& setRkey(uint32_t rkey);
    AtomicHeaderBuilder&& setSwapOrAddData(uint64_t swapOrAddData);
    AtomicHeaderBuilder&& setCompareData(uint64_t compareData);
    AtomicHeader build() &&;
    explicit AtomicHeaderBuilder() = default;

   private:
    std::optional<uint64_t> maybeRemoteAddr_;
    std::optional<uint32_t> maybeRkey_;
    std::optional<uint64_t> maybeSwapOrAddData_;
    std::optional<uint64_t> maybeCompareData_;
  };

 private:
  uint64_t remoteAddr_{0};
  uint32_t rkey_{0};
  uint64_t swapOrAddData_{0};
  uint64_t compareData_{0};
};

class AtomicAckHeader : public Header {
 public:
  static constexpr size_t kAtomicAckHeaderSizeBytes = 8;
  AtomicAckHeader() = default;
  explicit AtomicAckHeader(folly::io::Cursor& cursor);
  explicit AtomicAckHeader(uint64_t origRemoteData)
      : origRemoteData_(origRemoteData) {}
  bool operator==(const AtomicAckHeader& other) const {
    return origRemoteData_ == other.origRemoteData_;
  }

  [[nodiscard]] uint64_t getOrigRemoteData() const {
    return origRemoteData_;
  }

  void write(folly::io::QueueAppender& appender) const override;

  class AtomicAckHeaderBuilder {
   public:
    AtomicAckHeaderBuilder&& setOrigRemoteData(uint64_t origRemoteData);
    AtomicAckHeader build() &&;
    explicit AtomicAckHeaderBuilder() = default;

   private:
    std::optional<uint64_t> maybeOrigRemoteData_;
  };

 private:
  uint64_t origRemoteData_{0};
};

} // namespace libsoftmetaroce
