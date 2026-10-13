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

#include "libsoftmetaroce/codec/ControlHeader.h"

namespace libsoftmetaroce {

ControlHeader::ControlHeader(folly::io::Cursor& cursor)
    : type_(cursor.readBE<uint16_t>()),
      reserved_(cursor.readBE<uint16_t>()),
      timestamp_(cursor.readBE<uint32_t>()) {}

ControlHeader::ControlHeaderBuilder&&
ControlHeader::ControlHeaderBuilder::setType(const uint16_t& type) {
  maybeType_ = type;
  return std::move(*this);
}
ControlHeader::ControlHeaderBuilder&&
ControlHeader::ControlHeaderBuilder::setReserved(const uint16_t& reserved) {
  maybeReserved_ = reserved;
  return std::move(*this);
}
ControlHeader::ControlHeaderBuilder&&
ControlHeader::ControlHeaderBuilder::setPingId(const uint16_t& pingId) {
  maybeReserved_ = pingId;
  return std::move(*this);
}
ControlHeader::ControlHeaderBuilder&&
ControlHeader::ControlHeaderBuilder::setTimestamp(const uint32_t& timestamp) {
  maybeTimestamp_ = timestamp;
  return std::move(*this);
}
ControlHeader ControlHeader::ControlHeaderBuilder::build() && {
  return ControlHeader(
      maybeType_.value_or(0),
      maybeReserved_.value_or(0),
      maybeTimestamp_.value_or(0));
}

void ControlHeader::write(folly::io::QueueAppender& appender) const {
  appender.writeBE(type_);
  appender.writeBE(reserved_);
  appender.writeBE(timestamp_);
}

} // namespace libsoftmetaroce
