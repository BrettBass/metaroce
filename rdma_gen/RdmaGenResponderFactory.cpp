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

#include "rdma_gen/RdmaGenResponderFactory.h"

namespace facebook::neteng::ai::rdma_gen {

std::unique_ptr<RdmaGenResponder> RdmaGenResponderFactory::makeResponder(
    Params& setupParams,
    const OperationType opType,
    const ServerParams& serverParams,
    const RdmaGenResources& resources) {
  std::unique_ptr<RdmaGenResponder> server =
      initResponder(setupParams, opType, serverParams);
  initServer(server, setupParams, resources);
  return server;
}

std::unique_ptr<RdmaGenResponder> RdmaGenResponderFactory::initResponder(
    Params& setupParams,
    const OperationType opType,
    const ServerParams& serverParams) {
  // rx_depth must be set in setupParams before calling this function.
  std::unique_ptr<RdmaGenResponder> server;
  switch (opType) {
    case OperationType::SendReceive:
    case OperationType::SendImmediate:
    case OperationType::WriteImmediate:
      LOG(INFO) << "server endpoint role is receiver";
      setupParams.role = EndpointRole::Receiver;
      server = makeReceiver(setupParams, serverParams);
      break;
    case OperationType::Write:
      LOG(INFO) << "server endpoint role is write responder";
      setupParams.role = EndpointRole::WriteResponder;
      server = makeWriteResponder(setupParams, serverParams);
      break;
    case OperationType::Read:
      LOG(INFO) << "server endpoint role is read responder";
      setupParams.role = EndpointRole::ReadResponder;
      server = makeReadResponder(setupParams, serverParams);
      break;
    case OperationType::AtomicFetchAdd:
    case OperationType::AtomicCmpSwap:
      LOG(INFO) << "server endpoint role is atomic responder";
      setupParams.role = EndpointRole::AtomicResponder;
      server = makeAtomicResponder(setupParams, serverParams);
      break;
    default:
      LOG(INFO) << "server endpoint role is unknown";
      return makeResponder(setupParams, serverParams);
  }

  return server;
}

void RdmaGenResponderFactory::initServer(
    const std::unique_ptr<RdmaGenResponder>& server,
    Params& /* setupParams */,
    const RdmaGenResources& resources) {
  CHECK(resources.transport_provider)
      << "RdmaGenResources::transport_provider must not be null";
  server->init(*resources.transport_provider);
}

std::unique_ptr<RdmaGenResponder> RdmaGenResponderFactory::makeReceiver(
    Params& setupParams,
    const ServerParams& serverParams) {
  return std::make_unique<RdmaGenReceiver>(
      setupParams, serverParams.numClients, serverParams.outputJsonPath);
}

std::unique_ptr<RdmaGenResponder> RdmaGenResponderFactory::makeReadResponder(
    Params& setupParams,
    const ServerParams& serverParams) {
  // As a testing tool, read responder only supports all requesters
  // reading from the same memory region for now, so rx_depth is 1.
  setupParams.rx_depth = 1;
  setupParams.role = EndpointRole::ReadResponder;
  return std::make_unique<RdmaGenReadResponder>(
      setupParams, serverParams.numClients);
}

std::unique_ptr<RdmaGenResponder> RdmaGenResponderFactory::makeWriteResponder(
    Params& setupParams,
    const ServerParams& serverParams) {
  // As a testing tool, write responder only supports all requesters
  // writing to the same memory region for now, so rx_depth is 1.
  setupParams.rx_depth = 1;
  setupParams.role = EndpointRole::WriteResponder;
  return std::make_unique<RdmaGenWriteResponder>(
      setupParams, serverParams.numClients);
}

std::unique_ptr<RdmaGenResponder> RdmaGenResponderFactory::makeAtomicResponder(
    Params& setupParams,
    const ServerParams& serverParams) {
  setupParams.rx_depth = 1;
  setupParams.role = EndpointRole::AtomicResponder;
  return std::make_unique<RdmaGenReadResponder>(
      setupParams, serverParams.numClients);
}

std::unique_ptr<RdmaGenResponder> RdmaGenResponderFactory::makeResponder(
    Params&,
    const ServerParams&) {
  return std::unique_ptr<RdmaGenResponder>();
}

} // namespace facebook::neteng::ai::rdma_gen
