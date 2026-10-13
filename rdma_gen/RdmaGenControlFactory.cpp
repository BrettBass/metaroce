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

#include "rdma_gen/RdmaGenControlFactory.h"

#include <folly/coro/BlockingWait.h>
#include <glog/logging.h>

namespace facebook::neteng::ai::rdma_gen {

std::unique_ptr<IControlServer> RdmaGenControlFactory::createControlServer(
    uint16_t port,
    std::shared_ptr<RdmaGenEndpoint> endpoint,
    std::shared_ptr<std::vector<RcQpInfo>> remoteRcQpInfo,
    std::shared_ptr<ContinueFlag> continueFlag,
    int terminateThreshold,
    ControlTransport transport) {
  switch (transport) {
    case ControlTransport::Thrift: {
      LOG(INFO) << "Creating Thrift-based control server on port " << port;
      auto thriftServer = std::make_shared<RdmaGenThriftControlServer>(
          port, endpoint, remoteRcQpInfo, continueFlag, terminateThreshold);
      return std::make_unique<ThriftControlServerWrapper>(
          std::move(thriftServer));
    }
  }
  LOG(FATAL) << "Unknown ControlTransport";
}

std::unique_ptr<IControlClient> RdmaGenControlFactory::createControlClient(
    std::shared_ptr<RdmaGenEndpoint> endpoint,
    std::shared_ptr<std::vector<RcQpInfo>> remoteRcQpInfo,
    std::shared_ptr<std::vector<void*>> remoteAddrs,
    std::shared_ptr<std::vector<uint32_t>> rkeys,
    ControlTransport transport) {
  switch (transport) {
    case ControlTransport::Thrift: {
      LOG(INFO) << "Creating Thrift-based control client";
      auto thriftClient = std::make_unique<RdmaGenThriftControlClient>(
          endpoint, remoteRcQpInfo, remoteAddrs, rkeys);
      return std::make_unique<ThriftControlClientWrapper>(
          std::move(thriftClient));
    }
  }
  LOG(FATAL) << "Unknown ControlTransport";
}

bool ThriftControlClientWrapper::signalServer(
    const std::string& servername,
    int port,
    uint32_t qpIdx,
    ControlMessage controlMsg) {
  try {
    auto task = client_->signalServer(servername, port, qpIdx, controlMsg);

    auto& eventBase = client_->getEventBase();
    return folly::coro::blockingWait(
        folly::coro::co_withExecutor(&eventBase, std::move(task)));
  } catch (const std::exception& ex) {
    LOG(ERROR) << "Exception in signalServer: " << ex.what();
    return false;
  }
}

bool ThriftControlClientWrapper::exchangeMetadata(
    const std::string& servername,
    int port,
    const std::map<std::string, std::string>& localMetadata,
    std::map<std::string, std::string>& remoteMetadata) {
  try {
    auto task = client_->exchangeMetadata(
        servername, port, localMetadata, remoteMetadata);
    auto& eventBase = client_->getEventBase();
    return folly::coro::blockingWait(
        folly::coro::co_withExecutor(&eventBase, std::move(task)));
  } catch (const std::exception& ex) {
    LOG(ERROR) << "Exception in exchangeMetadata: " << ex.what();
    return false;
  }
}

bool ThriftControlClientWrapper::completionBarrier(
    const std::string& servername,
    int port,
    int qpIdx,
    uint64_t completed) {
  try {
    auto task = client_->completionBarrier(servername, port, qpIdx, completed);
    auto& eventBase = client_->getEventBase();
    return folly::coro::blockingWait(
        folly::coro::co_withExecutor(&eventBase, std::move(task)));
  } catch (const std::exception& ex) {
    LOG(ERROR) << "Exception in completionBarrier: " << ex.what();
    return false;
  }
}

} // namespace facebook::neteng::ai::rdma_gen
