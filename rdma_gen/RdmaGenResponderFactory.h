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
#include "rdma_gen/RdmaGenPoolManager.h"
#include "rdma_gen/RdmaGenReadResponder.h"
#include "rdma_gen/RdmaGenReceiver.h"
#include "rdma_gen/RdmaGenResponder.h"
#include "rdma_gen/RdmaGenStatsRecorder.h"
#include "rdma_gen/RdmaGenUtils.h"
#include "rdma_gen/RdmaGenWriteResponder.h"

namespace facebook::neteng::ai::rdma_gen {

struct ServerParams {
  int32_t numClients;
  std::string outputJsonPath;
};

class RdmaGenResponderFactory {
 public:
  /**
   * Create an initialize Responder
   */
  std::unique_ptr<RdmaGenResponder> makeResponder(
      Params& setupParams,
      const OperationType opType,
      const ServerParams& serverParams,
      const RdmaGenResources& resources);

 protected:
  std::unique_ptr<RdmaGenResponder> initResponder(
      Params& setupParams,
      OperationType opType,
      const ServerParams& serverParams);
  void initServer(
      const std::unique_ptr<RdmaGenResponder>& client,
      Params& setupParams,
      const RdmaGenResources& resources);
  std::unique_ptr<RdmaGenResponder> makeReceiver(
      Params& setupParams,
      const ServerParams& serverParams);
  std::unique_ptr<RdmaGenResponder> makeReadResponder(
      Params& setupParams,
      const ServerParams& serverParams);
  std::unique_ptr<RdmaGenResponder> makeWriteResponder(
      Params& setupParams,
      const ServerParams& serverParams);
  std::unique_ptr<RdmaGenResponder> makeAtomicResponder(
      Params& setupParams,
      const ServerParams& serverParams);
  std::unique_ptr<RdmaGenResponder> makeResponder(
      Params& setupParams,
      const ServerParams& serverParams);
};

} // namespace facebook::neteng::ai::rdma_gen
