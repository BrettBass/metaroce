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

#include <libsoftmetaroce/codec/RnrCancelHeader.h>

namespace libsoftmetaroce {

RnrCancelHeader::RnrCancelHeader(folly::io::Cursor& cursor) {
  flowsAvailable_ = cursor.read<uint8_t>();
  tag_ = cursor.read<uint8_t>();
  postedCsn_ = cursor.readBE<uint16_t>();
  rateHints_ = cursor.readBE<uint32_t>();
}

RnrCancelHeader::RnrCancelHeaderBuilder&&
RnrCancelHeader::RnrCancelHeaderBuilder::setFlowsAvailable(
    uint8_t flowsAvailable) {
  maybeFlowsAvailable_ = flowsAvailable;
  return std::move(*this);
}
RnrCancelHeader::RnrCancelHeaderBuilder&&
RnrCancelHeader::RnrCancelHeaderBuilder::setTag(uint8_t tag) {
  maybeTag_ = tag;
  return std::move(*this);
}
RnrCancelHeader::RnrCancelHeaderBuilder&&
RnrCancelHeader::RnrCancelHeaderBuilder::setPostedCsn(uint16_t postedCsn) {
  maybePostedCsn_ = postedCsn;
  return std::move(*this);
}
RnrCancelHeader::RnrCancelHeaderBuilder&&
RnrCancelHeader::RnrCancelHeaderBuilder::setRateHints(uint32_t rateHints) {
  maybeRateHints_ = rateHints;
  return std::move(*this);
}
RnrCancelHeader RnrCancelHeader::RnrCancelHeaderBuilder::build() && {
  return RnrCancelHeader(
      maybeFlowsAvailable_.value_or(0),
      maybeTag_.value_or(0),
      maybePostedCsn_.value(),
      maybeRateHints_.value_or(kDefaultRateHintGbps));
}

void RnrCancelHeader::write(folly::io::QueueAppender& appender) const {
  appender.write<uint8_t>(flowsAvailable_);
  appender.write<uint8_t>(tag_);
  appender.writeBE<uint16_t>(postedCsn_);
  appender.writeBE<uint32_t>(rateHints_);
}

} // namespace libsoftmetaroce
