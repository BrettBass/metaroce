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

#include <libsoftmetaroce/Icrc.h>
#include <libsoftmetaroce/ProtocolCompatibility.h>
#include <libsoftmetaroce/UdpEncapsulation.h>

namespace libsoftmetaroce {

size_t UdpEncapsulation::frameOutgoing(
    folly::IOBufQueue& bufferQueue,
    std::optional<uint32_t> originalIcrc) noexcept {
  folly::io::Cursor cursor{&*bufferQueue.front()};
  uint32_t icrcToUse;

  if (originalIcrc.has_value()) {
    icrcToUse = originalIcrc.value();
    VLOG(1) << fmt::format("Using original mcrc: 0x{:08X}", icrcToUse);
  } else {
    icrcToUse = Icrc::computeICRC(cursor);
    VLOG(1) << fmt::format("Computed mcrc: 0x{:08X}", icrcToUse);
  }

  auto icrcBuf = folly::IOBuf::create(kIcrcSize);
  icrcBuf->append(kIcrcSize);
  folly::io::RWPrivateCursor icrcCursor{icrcBuf.get()};
  icrcCursor.write<uint32_t>(icrcToUse);
  bufferQueue.append(std::move(icrcBuf));

  return kIcrcSize;
}

std::optional<Encapsulation::IngressMetadata> UdpEncapsulation::deframeIncoming(
    std::unique_ptr<folly::IOBuf>& buffer) noexcept {
  // A runt cannot carry the trailing mCRC.
  if (buffer->computeChainDataLength() < kIcrcSize) {
    return std::nullopt;
  }
  uint32_t receivedICRC;
  uint32_t computedICRC;
  bool isIcrcValid =
      Icrc::extractAndValidateICRC(buffer, receivedICRC, computedICRC);
  if (!isIcrcValid) {
    LOG_FIRST_N(ERROR, 5) << fmt::format(
        "mCRC mismatch: received=0x{:08X}, recomputed=0x{:08X}, packet_len={}",
        receivedICRC,
        computedICRC,
        buffer->length());
    if (FLAGS_libsoftmetaroce_validate_crc) {
      return std::nullopt;
    }
  }
  return IngressMetadata{};
}

} // namespace libsoftmetaroce
