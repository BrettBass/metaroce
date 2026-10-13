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

#include "rdma_gen/RdmaGenResponder.h"

#include <glog/logging.h>

#include "rdma_gen/RdmaGenControlFactory.h"
#include "rdma_gen/RdmaGenServerStatsRecorder.h"

namespace facebook::neteng::ai::rdma_gen {

RdmaGenResponder::RdmaGenResponder(
    const Params& params,
    int terminateThreshold,
    const std::string& jsonPath) {
  CHECK(
      params.role == EndpointRole::Receiver ||
      params.role == EndpointRole::WriteResponder ||
      params.role == EndpointRole::ReadResponder ||
      params.role == EndpointRole::AtomicResponder)
      << "responder role is not set correctly";
  endpoint_ = std::make_shared<RdmaGenEndpoint>(params);
  remoteRcQpInfo_ = std::make_shared<std::vector<RcQpInfo>>();
  continueFlag_ = std::make_shared<ContinueFlag>();

  // Use the factory to create the appropriate control server
  cmServer_ = RdmaGenControlFactory::createControlServer(
      params.rc_cm_port,
      endpoint_,
      remoteRcQpInfo_,
      continueFlag_,
      terminateThreshold);

  if (!jsonPath.empty()) {
    statsRecorder_ = std::make_unique<RdmaGenServerStatsRecorder>(jsonPath);
  }
}

void RdmaGenResponder::init(
    ITransportProvider& transportProvider,
    std::unique_ptr<CudaWrapper> cudaWrapper) {
  endpoint_->init(
      params().rx_depth,
      transportProvider,
      std::move(cudaWrapper),
      IBV_QPS_INIT);
  launchRcCmServer();
}

void RdmaGenResponder::writeJsonReport(OperationType opType) {
  if (statsRecorder_ != nullptr) {
    statsRecorder_->finalizeJsonReportWithCqe(opType, cqeLog_);
  }
}

void RdmaGenResponder::storeCqeWithTimestamp(
    const WorkCompletionAdapter& cqe,
    int64_t timestamp,
    std::optional<uint32_t> maybeImmData,
    std::optional<uint32_t> maybeSendFlags) {
  if (statsRecorder_ != nullptr) {
    cqeLog_.emplace_back(
        CqeLogEntry{cqe, timestamp, maybeImmData, maybeSendFlags});
  }
}

void RdmaGenResponder::setControlMetadata(
    const std::map<std::string, std::string>& metadata,
    std::optional<MetadataCallback> cb) {
  cmServer_->setMetadata(metadata);
  if (cb.has_value()) {
    cmServer_->setMetadataCallback(std::move(*cb));
  }
}

void RdmaGenResponder::setRxCompleteWaiter(RxCompleteWaiter waiter) {
  cmServer_->setRxCompleteWaiter(std::move(waiter));
}

void RdmaGenResponder::setBarrierTimeout(std::chrono::milliseconds timeout) {
  cmServer_->setBarrierTimeout(timeout);
}
} // namespace facebook::neteng::ai::rdma_gen
