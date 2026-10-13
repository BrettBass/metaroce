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

#include <fmt/core.h>
#include <libsoftmetaroce/EsunEncapsulation.h>

#include <glog/logging.h>
#include <libsoftmetaroce/Icrc.h>
#include <libsoftmetaroce/ProtocolCompatibility.h>
#include <algorithm>
#include <array>

namespace libsoftmetaroce {

EsunEncapsulation::EsunEncapsulation(
    folly::MacAddress srcMac,
    folly::MacAddress dstMac,
    EsunHeader header,
    uint16_t ethertype) noexcept
    : dstMac_(dstMac),
      srcMac_(srcMac),
      ethertype_(ethertype),
      header_(header) {}

size_t EsunEncapsulation::frameOutgoing(
    folly::IOBufQueue& bufferQueue,
    std::optional<uint32_t> originalIcrc) noexcept {
  DCHECK(!bufferQueue.empty()) << "bufferQueue must hold the serialized packet";
  constexpr size_t kOuterSize = kEthernetHeaderSize + EsunHeader::kSizeBytes;
  const size_t unpaddedFrameSize =
      kOuterSize + bufferQueue.chainLength() + kIcrcSize;
  const size_t paddingSize = unpaddedFrameSize < kMinimumEthernetFrameSizeBytes
      ? kMinimumEthernetFrameSizeBytes - unpaddedFrameSize
      : 0;
  if (paddingSize > 0) {
    constexpr std::array<uint8_t, kMinimumEthernetFrameSizeBytes>
        kZeroPadding{};
    folly::io::QueueAppender paddingAppender{&bufferQueue, paddingSize};
    paddingAppender.push(kZeroPadding.data(), paddingSize);
  }

  folly::io::Cursor cursor{&*bufferQueue.front()};
  const uint32_t icrc =
      originalIcrc.has_value() ? *originalIcrc : Icrc::computeICRC(cursor);
  folly::io::QueueAppender icrcAppender{&bufferQueue, kIcrcSize};
  icrcAppender.write<uint32_t>(icrc);

  folly::IOBufQueue outerQueue{folly::IOBufQueue::cacheChainLength()};
  folly::io::QueueAppender outerAppender{&outerQueue, kOuterSize};
  outerAppender.push(dstMac_.bytes(), folly::MacAddress::SIZE);
  outerAppender.push(srcMac_.bytes(), folly::MacAddress::SIZE);
  outerAppender.writeBE<uint16_t>(ethertype_);
  header_.write(outerAppender);

  auto outerBuffer = outerQueue.move();
  auto payload = bufferQueue.move();
  outerBuffer->appendToChain(std::move(payload));
  bufferQueue.append(std::move(outerBuffer));

  return framingOverheadBytes() + paddingSize;
}

std::optional<Encapsulation::IngressMetadata>
EsunEncapsulation::deframeIncoming(
    std::unique_ptr<folly::IOBuf>& buffer) noexcept {
  constexpr size_t kMinimumContentSize =
      kEthernetHeaderSize + EsunHeader::kSizeBytes + kIcrcSize;
  const size_t frameSize = buffer->computeChainDataLength();
  if (frameSize < kMinimumContentSize) {
    LOG_FIRST_N(ERROR, 5) << "received frame too short for ESUN framing: "
                          << frameSize;
    return std::nullopt;
  }

  buffer->coalesce();
  folly::io::Cursor cursor{buffer.get()};
  cursor.skip(2 * folly::MacAddress::SIZE);
  const uint16_t ethertype = cursor.readBE<uint16_t>();
  if (ethertype != ethertype_) {
    LOG_FIRST_N(WARNING, 5)
        << "unexpected ethertype 0x" << std::hex << ethertype << ", expected 0x"
        << ethertype_ << std::dec;
    return std::nullopt;
  }

  const EsunHeader rxHeader{cursor};
  buffer->trimStart(kEthernetHeaderSize + EsunHeader::kSizeBytes);

  uint32_t receivedIcrc = 0;
  uint32_t computedIcrc = 0;
  if (!Icrc::extractAndValidateICRC(buffer, receivedIcrc, computedIcrc)) {
    LOG_FIRST_N(ERROR, 5) << fmt::format(
        "invalid ESUN iCRC: received=0x{:08X}, recomputed=0x{:08X}, packet_len={}",
        receivedIcrc,
        computedIcrc,
        buffer->computeChainDataLength());
    if (FLAGS_libsoftmetaroce_validate_crc) {
      return std::nullopt;
    }
  }
  return IngressMetadata{.ecn = rxHeader.ecn()};
}

size_t EsunEncapsulation::payloadSize(
    const folly::io::Cursor& cursor) const noexcept {
  return cursor.totalLength() >= kIcrcSize ? cursor.totalLength() - kIcrcSize
                                           : 0;
}

} // namespace libsoftmetaroce
