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

#include <libsoftmetaroce/codec/AtomicHeader.h>

namespace libsoftmetaroce {

AtomicHeader::AtomicHeader(folly::io::Cursor& cursor) {
  remoteAddr_ = cursor.readBE<uint64_t>();
  rkey_ = cursor.readBE<uint32_t>();
  swapOrAddData_ = cursor.readBE<uint64_t>();
  compareData_ = cursor.readBE<uint64_t>();
}

AtomicHeader::AtomicHeaderBuilder&&
AtomicHeader::AtomicHeaderBuilder::setRemoteAddr(uint64_t remoteAddr) {
  maybeRemoteAddr_ = remoteAddr;
  return std::move(*this);
}

AtomicHeader::AtomicHeaderBuilder&& AtomicHeader::AtomicHeaderBuilder::setRkey(
    uint32_t rkey) {
  LOG_IF(ERROR, rkey == 0) << "Rkey should be non-zero";
  maybeRkey_ = rkey;
  return std::move(*this);
}

AtomicHeader::AtomicHeaderBuilder&&
AtomicHeader::AtomicHeaderBuilder::setSwapOrAddData(uint64_t swapOrAddData) {
  maybeSwapOrAddData_ = swapOrAddData;
  return std::move(*this);
}

AtomicHeader::AtomicHeaderBuilder&&
AtomicHeader::AtomicHeaderBuilder::setCompareData(uint64_t compareData) {
  maybeCompareData_ = compareData;
  return std::move(*this);
}

AtomicHeader AtomicHeader::AtomicHeaderBuilder::build() && {
  CHECK(maybeRemoteAddr_.has_value()) << "AtomicHeader.remoteAddr must be set";
  CHECK(maybeRkey_.has_value()) << "AtomicHeader.rkey must be set";
  CHECK(maybeSwapOrAddData_.has_value())
      << "AtomicHeader.swapOrAddData must be set";
  // Alignment is not enforced on encode: a misaligned address must reach the
  // responder, which validates it and NAKs the request as invalid.
  return AtomicHeader(
      maybeRemoteAddr_.value(),
      maybeRkey_.value(),
      maybeSwapOrAddData_.value(),
      maybeCompareData_.value_or(0));
}

void AtomicHeader::write(folly::io::QueueAppender& appender) const {
  appender.writeBE<uint64_t>(remoteAddr_);
  appender.writeBE<uint32_t>(rkey_);
  appender.writeBE<uint64_t>(swapOrAddData_);
  appender.writeBE<uint64_t>(compareData_);
}

AtomicAckHeader::AtomicAckHeader(folly::io::Cursor& cursor) {
  origRemoteData_ = cursor.readBE<uint64_t>();
}

AtomicAckHeader::AtomicAckHeaderBuilder&&
AtomicAckHeader::AtomicAckHeaderBuilder::setOrigRemoteData(
    uint64_t origRemoteData) {
  maybeOrigRemoteData_ = origRemoteData;
  return std::move(*this);
}

AtomicAckHeader AtomicAckHeader::AtomicAckHeaderBuilder::build() && {
  CHECK(maybeOrigRemoteData_.has_value())
      << "AtomicAckHeader.origRemoteData must be set";
  return AtomicAckHeader(maybeOrigRemoteData_.value());
}

void AtomicAckHeader::write(folly::io::QueueAppender& appender) const {
  appender.writeBE<uint64_t>(origRemoteData_);
}

} // namespace libsoftmetaroce
