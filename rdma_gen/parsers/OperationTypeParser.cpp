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

#include "rdma_gen/parsers/OperationTypeParser.h"

#include <unordered_map>

#include <folly/String.h>
#include <glog/logging.h>

#include "rdma_gen/RdmaGenFlags.h"
#include "rdma_gen/RdmaGenUtils.h"

namespace facebook::neteng::ai::rdma_gen {

namespace {

/**
 * Validates the operation type and related flags.
 *
 * @param op The operation type to validate
 * @return true if the operation type and related flags are valid, false
 * otherwise
 */
bool validateOperationType(const OperationType op) {
  if ((op == OperationType::Read || op == OperationType::AtomicFetchAdd ||
       op == OperationType::AtomicCmpSwap) &&
      FLAGS_max_rd == 0) {
    LOG(ERROR) << "max_rd must be non zero for read/atomic operation";
    return false;
  }

  if (FLAGS_writes_before_imm != 0 &&
      (op != OperationType::WriteImmediate &&
       op != OperationType::SendImmediate)) {
    LOG(ERROR) << "writes_before_imm may be set only "
               << "when op_type=write_imm or op_type=send_imm";
    return false;
  }

  if (op == OperationType::AtomicFetchAdd ||
      op == OperationType::AtomicCmpSwap) {
    std::vector<uint64_t> sizes;
    folly::split(',', FLAGS_message_size, sizes);
    for (const auto size : sizes) {
      if (size != kAtomicOperandBytes) {
        LOG(ERROR) << "message_size must be " << kAtomicOperandBytes
                   << " for atomic operations, got " << size;
        return false;
      }
    }
  }

  return true;
}

const std::unordered_map<std::string, OperationType> kSupportedOperationsMap = {
    {"send_recv", OperationType::SendReceive},
    {"sender_receiver", OperationType::SendReceive},
    {"write", OperationType::Write},
    {"write_imm", OperationType::WriteImmediate},
    {"read", OperationType::Read},
    {"send_imm", OperationType::SendImmediate},
    {"atomic_fetch_add", OperationType::AtomicFetchAdd},
    {"atomic_cmp_swap", OperationType::AtomicCmpSwap},
};

const auto kSupportedOperationCodesStr = []() {
  std::vector<std::string> opCodes;
  opCodes.reserve(kSupportedOperationsMap.size());
  for (const auto& [opCode, _] : kSupportedOperationsMap) {
    opCodes.push_back(opCode);
  }
  return folly::join(",", opCodes);
}();

} // namespace

OperationType parseOperationType(std::string op_type) {
  folly::toLowerAscii(op_type);
  const auto operation = kSupportedOperationsMap.find(op_type);
  if (operation == kSupportedOperationsMap.cend()) {
    LOG(ERROR) << fmt::format(
        "Unknown operation: {}, supported options: {}",
        op_type,
        kSupportedOperationCodesStr);
    exit(1);
  }

  const auto opType = operation->second;
  if (!validateOperationType(opType)) {
    exit(1);
  }
  return opType;
}

} // namespace facebook::neteng::ai::rdma_gen
