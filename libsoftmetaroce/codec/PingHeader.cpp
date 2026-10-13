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

#include "libsoftmetaroce/codec/PingHeader.h"

namespace libsoftmetaroce {

PingHeader::PingHeader(folly::io::Cursor& cursor)
    : tag_(cursor.read<uint8_t>()),
      errorState_(cursor.read<uint8_t>()),
      supportedFlowCount_(cursor.readBE<uint16_t>()),
      reserved_(cursor.readBE<uint16_t>()),
      activePortSet_(cursor.readBE<uint16_t>()) {}

PingHeader::PingHeaderBuilder&& PingHeader::PingHeaderBuilder::setTag(
    const uint8_t& tag) {
  maybeTag_ = tag;
  return std::move(*this);
}
PingHeader::PingHeaderBuilder&& PingHeader::PingHeaderBuilder::setErrorState(
    const PingErrorState& errorState) {
  maybeErrorState_ = static_cast<uint8_t>(errorState);
  return std::move(*this);
}
PingHeader::PingHeaderBuilder&&
PingHeader::PingHeaderBuilder::setSupportedFlowCount(
    const uint16_t& supportedFlowCount) {
  maybeSupportedFlowCount_ = supportedFlowCount;
  return std::move(*this);
}
PingHeader::PingHeaderBuilder&& PingHeader::PingHeaderBuilder::setActivePortSet(
    uint16_t activePortSet) {
  maybeActivePortSet_ = activePortSet;
  return std::move(*this);
}
PingHeader PingHeader::PingHeaderBuilder::build() && {
  return PingHeader(
      maybeTag_.value_or(0),
      maybeErrorState_.value_or(0),
      maybeSupportedFlowCount_.value_or(0),
      maybeActivePortSet_.value_or(0));
}

void PingHeader::write(folly::io::QueueAppender& appender) const {
  appender.write(tag_);
  appender.write(errorState_);
  appender.writeBE(supportedFlowCount_);
  appender.writeBE<uint16_t>(0); // reserved
  appender.writeBE(activePortSet_);
}

} // namespace libsoftmetaroce
