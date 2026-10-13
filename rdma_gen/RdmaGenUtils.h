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

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <folly/String.h>
#include <glog/logging.h>

#include <fmt/core.h>
#include <re2/re2.h>

namespace facebook::neteng::ai::rdma_gen {

#define INVALID_CPU_ID -1

/**
 * Parse a string value, trimming whitespace.
 * @throws std::runtime_error if the value is empty after trimming
 */
std::string parseString(const std::string& value);

/**
 * Parse a 64-bit unsigned integer from a string.
 */
uint64_t parseU64(const std::string& value);

/**
 * Parse a 32-bit unsigned integer from a string.
 */
uint32_t parseU32(const std::string& value);

/**
 * Parse a 16-bit unsigned integer from a string.
 */
uint16_t parseU16(const std::string& value);

class PlatformType {
 public:
  using Value = unsigned int;
  enum {
    UNKNOWN = 0,
    ZION_EX,
    GRAND_TETON,
    Last,
  };
};

PlatformType::Value platformTypeFromString(std::string type);
std::string_view platformTypeToString(const PlatformType::Value& type);

enum class CQThreadType {
  REQUESTER,
  RECEIVER,
};

uint64_t parseToNumber(const std::string& str);
size_t roundUpTo64X(size_t size);
uint32_t getRankPattern(uint32_t rank);

// Role type of this endpoint
enum class EndpointRole {
  Unknown = 0,
  Sender = 1,
  Receiver = 2,
  WriteRequester = 3,
  ReadRequester = 4,
  WriteResponder = 5,
  ReadResponder = 6,
  SenderReceiver = 7,
  WriteImmediateRequester = 8,
  AtomicFetchAddRequester = 9,
  AtomicCmpSwapRequester = 10,
  AtomicResponder = 11,
};

// control messages between RC sender/receiver. These will be used
// to categorize what kind of data is being sent between sender and
// receiver

constexpr std::string_view kCmGetRemoteInfo = "GetRemoteInfo";
constexpr std::string_view kCmTerminate = "Terminate";
constexpr std::string_view kCmWrite = "Write";
constexpr std::string_view kCmRead = "Read";
constexpr std::string_view kCmExchangeMetadata = "ExchangeMetadata";
constexpr std::string_view kCmCompletionBarrier = "CompletionBarrier";

enum class ControlMessage {
  // get remote info
  GetRemoteInfo = 0,
  // used to signal the remote server to terminate
  Terminate = 1,
  // used to signal write parameters
  Write = 2,
  // used to signal read parameters
  Read = 3,
  // used to exchange metadata key-value pairs
  ExchangeMetadata = 4,
  // used to block until the server has drained all expected RX completions
  CompletionBarrier = 5,
};

std::string_view controlMessageToString(const ControlMessage& message);

enum class OperationType {
  SendReceive = 0,
  Write = 1,
  WriteImmediate = 2,
  Read = 3,
  SendImmediate = 4,
  AtomicFetchAdd = 5,
  AtomicCmpSwap = 6,
};

// IB-spec atomic operand width: atomics RMW exactly one 64-bit word.
inline constexpr uint32_t kAtomicOperandBytes = 8;

struct LogHelper {
  static std::string logPrefixServer() {
    return fmt::format("- SERVER");
  }

  static std::string logPrefixClient() {
    return fmt::format("- CLIENT");
  }
};

inline int getNearestCPUId(
    PlatformType::Value platform_type,
    CQThreadType cq_thread_type,
    int gpu_idx) {
  int nearestCpuId = INVALID_CPU_ID;
  const std::vector<std::pair<int, int>> gpuToCPU = {
      {0, 23},
      {0, 23},
      {24, 47},
      {24, 47},
      {48, 71},
      {48, 71},
      {72, 95},
      {72, 95},
  };

  if (platform_type == PlatformType::ZION_EX &&
      (gpu_idx >= 0 && gpu_idx <= 7)) {
    nearestCpuId =
        gpuToCPU[gpu_idx].first + gpu_idx + static_cast<int>(cq_thread_type);
  }

  LOG(INFO) << fmt::format("getNearestCpuId: nearest CPU ID: {}", nearestCpuId);
  return nearestCpuId;
}

inline void setCqPollThreadAffinity(
    std::thread& cqPollThread,
    PlatformType::Value platform_type,
    CQThreadType cq_thread_type,
    int gpu_idx) {
  int cpuId = getNearestCPUId(platform_type, cq_thread_type, gpu_idx);
  if (cpuId == INVALID_CPU_ID) {
    return;
  }

  cpu_set_t cpu_set;
  CPU_ZERO(&cpu_set);
  CPU_SET(cpuId, &cpu_set);
  LOG(INFO) << "setCqPollThreadAffinity: before setting affinity, CQ thread id="
            << std::this_thread::get_id() << " for GPU idx=" << gpu_idx
            << " is running on CPU id=" << sched_getcpu();

  int ret = pthread_setaffinity_np(
      cqPollThread.native_handle(), sizeof(cpu_set_t), &cpu_set);
  LOG_IF(INFO, ret != 0)
      << "setCqPollThreadAffinity: pthread_setaffinity_np returns with error "
      << ret;

  ret = pthread_getaffinity_np(
      cqPollThread.native_handle(), sizeof(cpu_set_t), &cpu_set);
  if (ret != 0) {
    LOG(INFO)
        << "setCqPollThreadAffinity: pthread_getaffinity_np returns with error "
        << ret;
  }
  for (int i = 0; i < CPU_SETSIZE; i++) {
    if (CPU_ISSET(i, &cpu_set)) {
      LOG(INFO)
          << "setCqPollThreadAffinity: after setting affinity, CQ thread id="
          << std::this_thread::get_id() << " for GPU idx=" << gpu_idx
          << " is running on CPU id=" << i;
    }
  }
}

uint64_t getCurrentTimestampUs();

std::string_view operationTypeToString(const OperationType& type);

struct IndexPair {
  uint32_t bufferIdx;
  uint32_t qpIdx;
};

uint64_t pack(uint32_t a, uint32_t b);

std::pair<uint32_t, uint32_t> unpack(uint64_t c);

uint64_t createWrId(uint32_t bufferIdx, uint32_t qpIdx);

IndexPair parseWrId(uint64_t wrId);

// Extended wr_id encoding with sequence number for debugging completion
// ordering. Layout: [seqNum:16][qpIdx:16][bufferIdx:32]
struct WrIdWithSeq {
  uint16_t seqNum;
  uint16_t qpIdx;
  uint32_t bufferIdx;
};

inline uint64_t
createWrIdWithSeq(uint16_t seqNum, uint16_t qpIdx, uint32_t bufferIdx) {
  return (static_cast<uint64_t>(seqNum) << 48) |
      (static_cast<uint64_t>(qpIdx) << 32) | static_cast<uint64_t>(bufferIdx);
}

inline WrIdWithSeq parseWrIdWithSeq(uint64_t wrId) {
  return {
      .seqNum = static_cast<uint16_t>(wrId >> 48),
      .qpIdx = static_cast<uint16_t>((wrId >> 32) & 0xFFFF),
      .bufferIdx = static_cast<uint32_t>(wrId & 0xFFFFFFFF)};
}

} // namespace facebook::neteng::ai::rdma_gen
