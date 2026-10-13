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

#include <libsoftmetaroce/Encapsulation.h>
#include <memory>
#include <optional>

namespace libsoftmetaroce {

// UDP encapsulation: L2 + IPv6 + UDP + MetaRoCE + mCRC.
// Appends a 4-byte mCRC on TX and validates it on RX.
class UdpEncapsulation : public Encapsulation {
 public:
  size_t frameOutgoing(
      folly::IOBufQueue& bufferQueue,
      std::optional<uint32_t> originalIcrc) noexcept override;

  std::optional<IngressMetadata> deframeIncoming(
      std::unique_ptr<folly::IOBuf>& buffer) noexcept override;

  [[nodiscard]] size_t payloadSize(
      const folly::io::Cursor& cursor) const noexcept override {
    auto remaining = cursor.totalLength();
    if (remaining < sizeof(uint32_t)) {
      return 0;
    }
    return remaining - sizeof(uint32_t);
  }
};

} // namespace libsoftmetaroce
