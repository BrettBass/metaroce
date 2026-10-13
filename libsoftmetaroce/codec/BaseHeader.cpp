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

#include <libsoftmetaroce/codec/BaseHeader.h>

namespace libsoftmetaroce {

BaseHeader::BaseHeader(folly::io::Cursor& cursor) {
  opcode_ = Opcode(cursor.read<uint8_t>());
  reserved_ = cursor.read<uint8_t>();
  uint16_t versionAndFlowId = cursor.readBE<uint16_t>();
  version_ = versionAndFlowId >> kFlowIdBits;
  flowId_ = versionAndFlowId & kFlowIdMask;
  dstQp_ = cursor.readBE<uint32_t>();
  srcQp_ = cursor.readBE<uint32_t>();
}

BaseHeader::BaseHeaderBuilder&& BaseHeader::BaseHeaderBuilder::setOpcode(
    Opcode opcode) {
  maybeOpcode_ = opcode;
  return std::move(*this);
}

BaseHeader::BaseHeaderBuilder&& BaseHeader::BaseHeaderBuilder::setVersion(
    uint8_t version) {
  maybeVersion_ = version;
  return std::move(*this);
}

BaseHeader::BaseHeaderBuilder&& BaseHeader::BaseHeaderBuilder::setFlowId(
    uint16_t flowId) {
  DCHECK_LE(flowId, kFlowIdMask) << "BaseHeader.flowId must fit in 12 bits";
  maybeFlowId_ = flowId;
  return std::move(*this);
}

BaseHeader::BaseHeaderBuilder&& BaseHeader::BaseHeaderBuilder::setDstQp(
    uint32_t dstQp) {
  maybeDstQp_ = dstQp;
  return std::move(*this);
}

BaseHeader::BaseHeaderBuilder&& BaseHeader::BaseHeaderBuilder::setSrcQp(
    uint32_t srcQp) {
  maybeSrcQp_ = srcQp;
  return std::move(*this);
}

BaseHeader BaseHeader::BaseHeaderBuilder::build() && {
  CHECK(maybeOpcode_.has_value()) << "BaseHeader.opcode must be set";
  CHECK(maybeVersion_.has_value()) << "BaseHeader.version must be set";
  CHECK(maybeFlowId_.has_value()) << "BaseHeader.flowId must be set";
  CHECK(maybeDstQp_.has_value()) << "BaseHeader.dstQp must be set";
  CHECK(maybeSrcQp_.has_value()) << "BaseHeader.srcQp must be set";
  return BaseHeader(
      maybeOpcode_.value(),
      maybeVersion_.value(),
      maybeFlowId_.value(),
      maybeDstQp_.value(),
      maybeSrcQp_.value());
}

void BaseHeader::write(folly::io::QueueAppender& appender) const {
  appender.write<uint8_t>(opcode_.value());
  appender.write<uint8_t>(0); // reserved
  uint16_t versionAndFlowId =
      static_cast<uint16_t>(version_) << kFlowIdBits | (flowId_ & kFlowIdMask);
  appender.writeBE<uint16_t>(versionAndFlowId);
  appender.writeBE<uint32_t>(dstQp_);
  appender.writeBE<uint32_t>(srcQp_);
}

} // namespace libsoftmetaroce
