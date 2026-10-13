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

#include "rdma_gen/RdmaGenEndpoint.h"
#include "rdma_gen/RdmaGenRequester.h"
#include "rdma_gen/RdmaGenStatsRecorder.h"

namespace facebook::neteng::ai::rdma_gen {

class RdmaGenSender : public RdmaGenRequester {
 public:
  RdmaGenSender(
      const Params& params,
      const std::vector<int64_t>& numMessagesPerServer,
      bool equalMessagesPerServer,
      const std::vector<std::string>& rcReceivers,
      const std::vector<int>& rcCmPorts,
      bool logMessageLatencies,
      const std::vector<int>& rcCos = {},
      const std::vector<enum ibv_mtu>& mtus = {},
      const enum ibv_qp_state targetQPState = IBV_QPS_RTS,
      const std::string& jsonPath = "");

  ~RdmaGenSender() override = default;

 private:
  int postRequest(const RcBuffer& rcBuf) override;
};

} // namespace facebook::neteng::ai::rdma_gen
