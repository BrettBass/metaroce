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

#include <folly/String.h>
#include <folly/hash/Checksum.h>
#include <folly/io/Cursor.h>
#include <memory>

namespace libsoftmetaroce {

inline constexpr size_t kIcrcSize = sizeof(uint32_t);

class Icrc {
 public:
  // folly::crc32 uses a right-shifting CRC-32 algorithm, resulting in a fixed
  // checksum value of 0x2144DF1C. Ref:
  // https://en.wikipedia.org/wiki/Ethernet_frame#Frame_check_sequence
  static constexpr uint32_t kExpectedIcrcValue = 0x2144DF1C;

  static uint32_t computeICRC(
      folly::io::Cursor& cursor,
      uint32_t icrc = 0xFFFFFFFF) {
    while (!cursor.isAtEnd()) {
      folly::ByteRange buf = cursor.peekBytes();
      VLOG(4) << "Buffer data (hex dump): "
              << folly::hexDump(buf.data(), buf.size());
      icrc = folly::crc32(buf.data(), buf.size(), icrc);
      cursor += buf.size();
    }
    return ~icrc;
  }

  // populates received / computed only if validation fails.
  static bool extractAndValidateICRC(
      const std::unique_ptr<folly::IOBuf>& buffer,
      uint32_t& receivedICRC,
      uint32_t& computedICRC) {
    // Too short to carry a trailing mCRC; cannot validate.
    if (buffer->computeChainDataLength() < sizeof(uint32_t)) {
      return false;
    }
    folly::io::Cursor cursor(buffer.get());

    // Compute the CRC over data+CRC, which should yield kExpectedIcrcValue
    uint32_t validatedCRC = Icrc::computeICRC(cursor);
    if (validatedCRC == kExpectedIcrcValue) {
      return true;
    }

    // We have a failure; place the received ICRC and a correct ICRC in
    // the reference parameters.
    cursor.reset(buffer.get());
    // The mCRC trails the chain, which may span buffers.
    cursor.skip(buffer->computeChainDataLength() - sizeof(uint32_t));
    receivedICRC = cursor.read<uint32_t>();

    auto truncated = buffer->clone();
    truncated->trimEnd(sizeof(uint32_t));
    cursor.reset(truncated.get());
    computedICRC = Icrc::computeICRC(cursor);

    VLOG(1) << fmt::format(
        "Check mCRC was 0x{:08X} instead of 0x{:08X}; received 0x{:08X} instead of correct 0x{:08X}",
        validatedCRC,
        kExpectedIcrcValue,
        receivedICRC,
        computedICRC);

    return false;
  }
};

} // namespace libsoftmetaroce
