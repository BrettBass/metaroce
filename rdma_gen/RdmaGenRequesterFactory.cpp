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

#include "rdma_gen/RdmaGenRequesterFactory.h"

namespace facebook::neteng::ai::rdma_gen {

std::unique_ptr<RdmaGenRequester> RdmaGenRequesterFactory::makeRequester(
    Params& setupParams,
    const OperationType opType,
    const ClientParams& clientParams,
    const RdmaGenResources& resources) {
  std::unique_ptr<RdmaGenRequester> client =
      initRequester(setupParams, opType, clientParams);
  initClient(client, clientParams, resources);
  return client;
}

std::unique_ptr<RdmaGenRequester> RdmaGenRequesterFactory::initRequester(
    Params& setupParams,
    const OperationType opType,
    const ClientParams& clientParams) {
  switch (opType) {
    case OperationType::SendReceive:
    case OperationType::SendImmediate:
      LOG(INFO) << "client operates as a sender";
      setupParams.role = EndpointRole::Sender;
      return makeSender(setupParams, clientParams);
    case OperationType::Write:
      LOG(INFO) << "client operates as a write requester";
      setupParams.role = EndpointRole::WriteRequester;
      return makeWriteRequester(setupParams, clientParams);
    case OperationType::WriteImmediate:
      LOG(INFO) << "client operates as a write immediate requester";
      setupParams.role = EndpointRole::WriteImmediateRequester;
      return makeWriteRequester(setupParams, clientParams);
    case OperationType::Read:
      LOG(INFO) << "client operates as a read requester";
      setupParams.role = EndpointRole::ReadRequester;
      return makeReadRequester(setupParams, clientParams);
    case OperationType::AtomicFetchAdd:
      LOG(INFO) << "client operates as an atomic fetch-add requester";
      setupParams.role = EndpointRole::AtomicFetchAddRequester;
      return makeAtomicRequester(setupParams, clientParams);
    case OperationType::AtomicCmpSwap:
      LOG(INFO) << "client operates as an atomic compare-swap requester";
      setupParams.role = EndpointRole::AtomicCmpSwapRequester;
      return makeAtomicRequester(setupParams, clientParams);
    default:
      LOG(INFO) << "client role type unknown";
      return std::unique_ptr<RdmaGenRequester>();
  }
}

void RdmaGenRequesterFactory::initClient(
    const std::unique_ptr<RdmaGenRequester>& client,
    const ClientParams& clientParams,
    const RdmaGenResources& resources) {
  CHECK(resources.transport_provider)
      << "RdmaGenResources::transport_provider must not be null";
  client->init(clientParams.sampleRate, *resources.transport_provider);
}

std::unique_ptr<RdmaGenRequester> RdmaGenRequesterFactory::makeSender(
    Params& setupParams,
    const ClientParams& clientParams) {
  std::unique_ptr<RdmaGenRequester> client = std::make_unique<RdmaGenSender>(
      setupParams,
      clientParams.numMessages,
      clientParams.equalMessagesPerServer,
      clientParams.servers,
      clientParams.ports,
      clientParams.logMsgLatencies,
      clientParams.cos,
      clientParams.mtus,
      clientParams.targetQpState,
      clientParams.outputJsonPath);
  return client;
}

std::unique_ptr<RdmaGenRequester> RdmaGenRequesterFactory::makeReadRequester(
    Params& setupParams,
    const ClientParams& clientParams) {
  std::unique_ptr<RdmaGenRequester> client =
      std::make_unique<RdmaGenReadRequester>(
          setupParams,
          clientParams.numMessages,
          clientParams.equalMessagesPerServer,
          clientParams.servers,
          clientParams.ports,
          clientParams.logMsgLatencies,
          clientParams.cos,
          clientParams.mtus,
          clientParams.targetQpState,
          clientParams.outputJsonPath);
  return client;
}

std::unique_ptr<RdmaGenRequester> RdmaGenRequesterFactory::makeWriteRequester(
    Params& setupParams,
    const ClientParams& clientParams) {
  std::unique_ptr<RdmaGenRequester> client =
      std::make_unique<RdmaGenWriteRequester>(
          setupParams,
          clientParams.numMessages,
          clientParams.equalMessagesPerServer,
          clientParams.servers,
          clientParams.ports,
          clientParams.logMsgLatencies,
          clientParams.cos,
          clientParams.mtus,
          clientParams.targetQpState,
          clientParams.outputJsonPath);
  return client;
}

std::unique_ptr<RdmaGenRequester> RdmaGenRequesterFactory::makeAtomicRequester(
    Params& setupParams,
    const ClientParams& clientParams) {
  std::unique_ptr<RdmaGenRequester> client =
      std::make_unique<RdmaGenAtomicRequester>(
          setupParams,
          clientParams.numMessages,
          clientParams.equalMessagesPerServer,
          clientParams.servers,
          clientParams.ports,
          clientParams.logMsgLatencies,
          clientParams.cos,
          clientParams.mtus,
          clientParams.targetQpState,
          clientParams.outputJsonPath);
  return client;
}

} // namespace facebook::neteng::ai::rdma_gen
