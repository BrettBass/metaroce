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

#include "rdma_gen/CqeLogEntry.h"

#include <infiniband/verbs.h>

#include "rdma_gen/RdmaGenUtils.h"

namespace facebook::neteng::ai::rdma_gen {

folly::dynamic cqeLogToDynamic(
    const std::vector<CqeLogEntry>& cqeLog,
    OperationType opType) {
  folly::dynamic cqeArray = folly::dynamic::array;
  for (const auto& entry : cqeLog) {
    const WorkCompletionAdapter& cqe = entry.cqe;

    folly::dynamic cqeInfo = folly::dynamic::object;
    cqeInfo["operation_type"] = folly::dynamic(operationTypeToString(opType));
    cqeInfo["wr_id"] = cqe.wr_id;
    cqeInfo["seq_num"] = parseWrIdWithSeq(cqe.wr_id).seqNum;
    cqeInfo["byte_len"] = cqe.byte_len;
    cqeInfo["qp_num"] = cqe.qp_num;
    cqeInfo["end_time_us"] = entry.timestamp;

    if (entry.maybeImmData.has_value()) {
      cqeInfo["imm_data"] = entry.maybeImmData.value();
    }

    if (entry.maybeSendFlags.has_value()) {
      cqeInfo["send_flags"] = entry.maybeSendFlags.value();
    }

    if (cqe.status != IBV_WC_SUCCESS) {
      cqeInfo["status"] =
          cqe.status_str != nullptr ? cqe.status_str : "unknown";
    }

    cqeArray.push_back(cqeInfo);
  }
  return cqeArray;
}

} // namespace facebook::neteng::ai::rdma_gen
