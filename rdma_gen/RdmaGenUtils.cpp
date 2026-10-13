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

#include "rdma_gen/RdmaGenUtils.h"
#include "libsoftmetaroce/ClockSource.h"

#include <stdexcept>

namespace facebook::neteng::ai::rdma_gen {

std::string parseString(const std::string& value) {
  std::string trimmed = std::string(folly::trimWhitespace(value));
  if (trimmed.empty()) {
    throw std::runtime_error("Required field is empty");
  }
  return trimmed;
}

uint64_t parseU64(const std::string& value) {
  return parseToNumber(value);
}

uint32_t parseU32(const std::string& value) {
  return static_cast<uint32_t>(parseToNumber(value));
}

uint16_t parseU16(const std::string& value) {
  return static_cast<uint16_t>(parseToNumber(value));
}

std::string_view controlMessageToString(const ControlMessage& message) {
  switch (message) {
    case ControlMessage::GetRemoteInfo:
      return kCmGetRemoteInfo;
    case ControlMessage::Write:
      return kCmWrite;
    case ControlMessage::Read:
      return kCmRead;
    case ControlMessage::Terminate:
      return kCmTerminate;
    case ControlMessage::ExchangeMetadata:
      return kCmExchangeMetadata;
    case ControlMessage::CompletionBarrier:
      return kCmCompletionBarrier;
    default:
      LOG(FATAL) << "Unknown Control Message";
  }
}

std::string_view operationTypeToString(const OperationType& type) {
  switch (type) {
    case OperationType::SendReceive:
      return "SendReceive";
    case OperationType::SendImmediate:
      return "SendImmediate";
    case OperationType::Write:
      return "Write";
    case OperationType::WriteImmediate:
      return "WriteImmediate";
    case OperationType::Read:
      return "Read";
    case OperationType::AtomicFetchAdd:
      return "AtomicFetchAdd";
    case OperationType::AtomicCmpSwap:
      return "AtomicCmpSwap";
    default:
      return "Unknown";
  }
}

uint64_t parseToNumber(const std::string& str) {
  if (str.starts_with("0b")) {
    return std::stoul(str.substr(2), nullptr, 2);
  }

  return std::stoul(str, nullptr, 0);
}

size_t roundUpTo64X(size_t size) {
  return ((size + 64 - 1) / 64) * 64;
}

/*
 * Create a 32 bit pattern based on the rank.
 */
uint32_t getRankPattern(uint32_t rank) {
  return ((0x01 + (rank * 4)) << 24) + ((0x02 + (rank * 4)) << 16) +
      ((0x03 + (rank * 4)) << 8) + (0x04 + (rank * 4));
}

// Concatenate two 32-bit integers into one 64-bit integer
// expanding 32-bit a to 64-bit a -> leftshift a (8 bytes) -> bitwise or b
uint64_t pack(uint32_t a, uint32_t b) {
  return ((uint64_t)a << 32 | b);
}

// Split one 64-bit integer into two 32-bit integers
std::pair<uint32_t, uint32_t> unpack(uint64_t c) {
  uint32_t a = ((c & 0xFFFFFFFF00000000LL) >> 32);
  uint32_t b = (c & 0xFFFFFFFFLL);
  return std::make_pair(a, b);
}

uint64_t createWrId(uint32_t bufferIdx, uint32_t qpIdx) {
  return pack(bufferIdx, qpIdx);
}

IndexPair parseWrId(uint64_t wrId) {
  auto p = unpack(wrId);
  return {p.first, p.second};
}

constexpr std::string_view kZionEx = "zion_ex";
constexpr std::string_view kGrandTeton = "grand_teton";

PlatformType::Value platformTypeFromString(std::string type) {
  if (type.empty()) {
    return PlatformType::UNKNOWN;
  }

  static const std::unordered_map<std::string_view, PlatformType::Value>
      kPlatformTypeMap{
          {kZionEx, PlatformType::ZION_EX},
          {kGrandTeton, PlatformType::GRAND_TETON},
      };
  std::transform(type.begin(), type.end(), type.begin(), ::tolower);

  if (!kPlatformTypeMap.contains(type)) {
    throw std::invalid_argument(fmt::format("Unknown platform type: {}", type));
  }
  return kPlatformTypeMap.at(type);
}

std::string_view platformTypeToString(const PlatformType::Value& type) {
  switch (type) {
    case PlatformType::ZION_EX:
      return kZionEx;
    case PlatformType::GRAND_TETON:
      return kGrandTeton;
    default:
      LOG(FATAL) << "Unknown Platform Type";
  }
}

uint64_t getCurrentTimestampUs() {
  static auto clk_src = libsoftmetaroce::getClockSourceInstance();
  return std::chrono::duration_cast<std::chrono::microseconds>(
             clk_src->now().time_since_epoch())
      .count();
}

} // namespace facebook::neteng::ai::rdma_gen
