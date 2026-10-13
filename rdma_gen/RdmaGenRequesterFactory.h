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

#include "rdma_gen/RdmaGenAtomicRequester.h"
#include "rdma_gen/RdmaGenEndpoint.h"
#include "rdma_gen/RdmaGenPoolManager.h"
#include "rdma_gen/RdmaGenReadRequester.h"
#include "rdma_gen/RdmaGenRequester.h"
#include "rdma_gen/RdmaGenSender.h"
#include "rdma_gen/RdmaGenStatsRecorder.h"
#include "rdma_gen/RdmaGenUtils.h"
#include "rdma_gen/RdmaGenWriteRequester.h"

struct ClientParams {
  std::vector<std::string> servers;
  std::vector<uint32_t> messageSize;
  std::vector<int> ports;
  std::vector<int> cos;
  std::vector<enum ibv_mtu> mtus;
  std::vector<int64_t> numMessages;
  ibv_qp_state targetQpState;
  bool logMsgLatencies{};
  bool equalMessagesPerServer{};
  std::string outputJsonPath;
  int sampleRate{};
};

namespace facebook::neteng::ai::rdma_gen {

class RdmaGenRequesterFactory {
 public:
  /**
   * Create an initialize Requester
   */
  std::unique_ptr<RdmaGenRequester> makeRequester(
      Params& setupParams,
      const OperationType opType,
      const ClientParams& clientParams,
      const RdmaGenResources& resources);

 protected:
  std::unique_ptr<RdmaGenRequester> initRequester(
      Params& setupParams,
      OperationType opType,
      const ClientParams& clientParams);
  void initClient(
      const std::unique_ptr<RdmaGenRequester>& client,
      const ClientParams& clientParams,
      const RdmaGenResources& resources);
  std::unique_ptr<RdmaGenRequester> makeSender(
      Params& setupParams,
      const ClientParams& clientParams);
  std::unique_ptr<RdmaGenRequester> makeReadRequester(
      Params& setupParams,
      const ClientParams& clientParams);
  std::unique_ptr<RdmaGenRequester> makeWriteRequester(
      Params& setupParams,
      const ClientParams& clientParams);
  std::unique_ptr<RdmaGenRequester> makeAtomicRequester(
      Params& setupParams,
      const ClientParams& clientParams);
};

} // namespace facebook::neteng::ai::rdma_gen
