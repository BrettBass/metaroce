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

#include <libsoftmetaroce/codec/ReadWriteHeader.h>

namespace libsoftmetaroce {

ReadWriteHeader::ReadWriteHeader(folly::io::Cursor& cursor) {
  messageAddr_ = cursor.readBE<uint64_t>();
  key_ = cursor.readBE<uint32_t>();
  messageLength_ = cursor.readBE<uint32_t>();
}

ReadWriteHeader::ReadWriteHeaderBuilder&&
ReadWriteHeader::ReadWriteHeaderBuilder::setMessageAddr(uint64_t messageAddr) {
  maybeMessageAddr_ = messageAddr;
  return std::move(*this);
}

ReadWriteHeader::ReadWriteHeaderBuilder&&
ReadWriteHeader::ReadWriteHeaderBuilder::setKey(uint32_t key) {
  maybeKey_ = key;
  return std::move(*this);
}

ReadWriteHeader::ReadWriteHeaderBuilder&&
ReadWriteHeader::ReadWriteHeaderBuilder::setMessageLength(
    uint32_t messageLength) {
  maybeMessageLength_ = messageLength;
  return std::move(*this);
}

ReadWriteHeader ReadWriteHeader::ReadWriteHeaderBuilder::build() && {
  CHECK(maybeMessageAddr_.has_value())
      << "ReadWriteHeader.messageAddr must be set";
  CHECK(maybeKey_.has_value()) << "ReadWriteHeader.key must be set";
  CHECK(maybeMessageLength_.has_value())
      << "ReadWriteHeader.messageLength must be set";
  CHECK_LE(maybeMessageLength_.value(), 1 << 28)
      << "Max message length is 256MB";
  // Only enforce non-null address / non-zero key when the message
  // actually carries bytes.
  if (maybeMessageLength_.value() > 0) {
    LOG_IF(WARNING, maybeMessageAddr_.value() == 0u)
        << "attempting to read/write bytes to nullptr";
    LOG_IF(WARNING, maybeKey_.value() == 0u)
        << "attempting to use a zero rkey with nonempty payload";
  }
  return ReadWriteHeader(
      maybeMessageAddr_.value(),
      maybeKey_.value(),
      maybeMessageLength_.value());
}

void ReadWriteHeader::write(folly::io::QueueAppender& appender) const {
  appender.writeBE<uint64_t>(messageAddr_);
  appender.writeBE<uint32_t>(key_);
  appender.writeBE<uint32_t>(messageLength_);
}

} // namespace libsoftmetaroce
