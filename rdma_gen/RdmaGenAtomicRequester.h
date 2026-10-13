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

#include "rdma_gen/RdmaGenRequester.h"

namespace facebook::neteng::ai::rdma_gen {

class RdmaGenAtomicRequester : public RdmaGenRequester {
 public:
  RdmaGenAtomicRequester(
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

  ~RdmaGenAtomicRequester() override = default;

  std::shared_ptr<std::vector<uint32_t>>& getRkeys() {
    return rkeys_;
  }

  std::shared_ptr<std::vector<void*>>& getRemoteAddrs() {
    return remoteAddrs_;
  }

 private:
  int postRequest(const RcBuffer& rcBuf) override;

  std::shared_ptr<std::vector<void*>> remoteAddrs_;
  std::shared_ptr<std::vector<uint32_t>> rkeys_;
  const bool isFetchAdd_;
};

} // namespace facebook::neteng::ai::rdma_gen
