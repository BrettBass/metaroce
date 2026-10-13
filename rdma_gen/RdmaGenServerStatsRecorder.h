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

#include "rdma_gen/CqeLogEntry.h"
#include "rdma_gen/RdmaGenStatsRecorder.h"

namespace facebook::neteng::ai::rdma_gen {

class RdmaGenServerStatsRecorder : public RdmaGenStatsRecorder {
 public:
  explicit RdmaGenServerStatsRecorder(const std::string& jsonPath)
      : RdmaGenStatsRecorder(jsonPath) {}
  ~RdmaGenServerStatsRecorder() override = default;
  void finalizeJsonReportWithCqe(
      OperationType opType,
      const std::vector<CqeLogEntry>& cqeLog);
};

} // namespace facebook::neteng::ai::rdma_gen
