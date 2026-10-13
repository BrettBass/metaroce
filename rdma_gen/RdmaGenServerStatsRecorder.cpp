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

#include "rdma_gen/RdmaGenServerStatsRecorder.h"
#include <folly/json/dynamic.h>
#include <folly/json/json.h>

namespace facebook::neteng::ai::rdma_gen {

void RdmaGenServerStatsRecorder::finalizeJsonReportWithCqe(
    OperationType opType,
    const std::vector<CqeLogEntry>& cqeLog) {
  if (jsonPath_.empty()) {
    LOG(INFO) << "No server-side json path provided, skipping json report";
    return;
  }

  folly::dynamic jsonRoot = folly::dynamic::object;
  jsonRoot["cqes"] = cqeLogToDynamic(cqeLog, opType);

  std::ofstream outFile(jsonPath_, std::ios::trunc);
  if (outFile.is_open()) {
    outFile << folly::toPrettyJson(jsonRoot);
    outFile.close();
  }
}

} // namespace facebook::neteng::ai::rdma_gen
