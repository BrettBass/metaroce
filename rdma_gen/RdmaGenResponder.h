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

#include <atomic>

#include "rdma_gen/RdmaGenControlFactory.h"
#include "rdma_gen/RdmaGenEndpoint.h"
#include "rdma_gen/RdmaGenServerStatsRecorder.h"

namespace facebook::neteng::ai::rdma_gen {

class RdmaGenResponder {
 public:
  explicit RdmaGenResponder(
      const Params& params,
      int terminateThreshold = 1,
      const std::string& jsonPath = "");

  virtual ~RdmaGenResponder() = default;

  virtual void stop() {
    setContinueFlag(false);
    cmServer_->stop();
  }

  // Should be called exactly once at start before any other methods.
  virtual void init(
      ITransportProvider& transportProvider,
      std::unique_ptr<CudaWrapper> cudaWrapper =
          std::make_unique<CudaWrapper>());

  // Getter for RC sender QP info.
  const RcQpInfo& getRemoteRcQpInfo(int idx) const {
    return remoteRcQpInfo_->at(idx);
  }

  const RcQpInfo& getLocalRcQpInfo(int idx) const {
    return endpoint_->getLocalRcQpInfo(idx);
  }

  RcQpContext* ctx() {
    return endpoint_->ctx();
  }

  const Params& params() const {
    return endpoint_->params();
  }

  // waits for terminations from clients
  void waitForStop() {
    auto locked = continueFlag_->flag.rlock();
    // wait until *locked is false
    continueFlag_->cv.wait(locked.as_lock(), [&] { return !*locked; });
  }

  void setContinueFlag(bool flag) {
    continueFlag_->set(flag);
  }

  bool continueFlag() {
    return continueFlag_->get();
  }

  void writeJsonReport(OperationType opType);

  // Sets metadata to be exchanged with clients via the control channel.
  void setControlMetadata(
      const std::map<std::string, std::string>& metadata,
      std::optional<MetadataCallback> cb = std::nullopt);

  // Wires the completion-barrier RX-complete waiter and timeout onto the
  // control server. Parallel to setControlMetadata.
  void setRxCompleteWaiter(RxCompleteWaiter waiter);
  void setBarrierTimeout(std::chrono::milliseconds timeout);
  void storeCqeWithTimestamp(
      const WorkCompletionAdapter& cqe,
      int64_t timestamp,
      std::optional<uint32_t> maybeImmData,
      std::optional<uint32_t> maybeSendFlags = std::nullopt);

  // Returns true if any WQE completed with an error
  bool hasWqeErrors() const {
    return firstWqeError_.load(std::memory_order_relaxed) != IBV_WC_SUCCESS;
  }

  // Returns the first WQE error status observed, or IBV_WC_SUCCESS if none
  int getFirstWqeError() const {
    return firstWqeError_.load(std::memory_order_relaxed);
  }

 protected:
  std::shared_ptr<RdmaGenEndpoint> endpoint_;
  std::unique_ptr<RdmaGenServerStatsRecorder> statsRecorder_;

  // Launches server to provide this receiver's RC QP info that senders can
  // route RC traffic to. This is in lieu of RDMA CM.
  virtual void launchRcCmServer() {
    LOG(INFO) << "Launching RC CM server";
    cmServer_->start();
  }

  // Records the first WQE error status. Only the first error is captured;
  // subsequent errors are ignored. Thread-safe.
  void setFirstWqeError(int status) {
    if (status == IBV_WC_SUCCESS) {
      return;
    }
    int expected = IBV_WC_SUCCESS;
    firstWqeError_.compare_exchange_strong(
        expected, status, std::memory_order_relaxed);
  }

 private:
  std::shared_ptr<std::vector<RcQpInfo>> remoteRcQpInfo_;
  std::unique_ptr<IControlServer> cmServer_;
  std::shared_ptr<ContinueFlag> continueFlag_;
  std::vector<CqeLogEntry> cqeLog_;
  std::atomic<int> firstWqeError_{IBV_WC_SUCCESS};

 protected:
  ContinueFlag& getContinueFlag() {
    return *continueFlag_;
  }
};

} // namespace facebook::neteng::ai::rdma_gen
