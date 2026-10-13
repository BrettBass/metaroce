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
#include <chrono>
#include <memory>
#include <thread>

#include "rdma_gen/RdmaGenPoolManager.h"
#include "rdma_gen/RdmaGenResponder.h"

namespace facebook::neteng::ai::rdma_gen {

// Represents a RC receiver (the UD Sender).
// Usage:
//     rc::Params params = ...;
//     RcReceiver receiver(params)
//     receiver.init(...);
//     auto b = receiver.fetchDataReadyBuffer();
//     // Process received data in b.
//     // b.token must not change.
//     receiver.returnBuffer(b);
class RdmaGenReceiver : public RdmaGenResponder {
 public:
  explicit RdmaGenReceiver(
      const Params& params,
      int terminateThreshold = 1,
      const std::string& jsonPath = "");

  ~RdmaGenReceiver() override;

  RdmaGenReceiver(const RdmaGenReceiver&) = delete;
  RdmaGenReceiver& operator=(const RdmaGenReceiver&) = delete;
  RdmaGenReceiver(RdmaGenReceiver&&) = delete;
  RdmaGenReceiver& operator=(RdmaGenReceiver&&) = delete;

  void stop() override {
    RdmaGenResponder::stop();
    if (pollingThread_.joinable()) {
      pollingThread_.join();
    }
  }

  // Should be called exactly once at start before any other methods.
  void init(
      ITransportProvider& transportProvider,
      std::unique_ptr<CudaWrapper> cudaWrapper =
          std::make_unique<CudaWrapper>()) override;

  // Fetches a RcBuffer that is filled with received control data. Blocking call
  // if no data is available. RcBuffer's token field should not be modified.
  RcBuffer fetchDataReadyBuffer();

  // Returns RcBuffer back to the receiver's pool for RC recv WR.
  void returnBuffer(const RcBuffer& rcBuf);

  // Returns the associated pool manager.
  std::unique_ptr<RdmaGenPoolManager>& getPoolManager();

  // Returns the number of outstanding RRs.
  int getNumRrOutstanding() const;

  // Returns the number of delivered completions.
  int getNumCompletionsDelivered() const;

  // Returns the number of messages left to receive
  int getNumCompletionsExpected() const;

  // Returns the total number of RX completions expected across all QPs for the
  // whole test (fixed at construction). Negative means unbounded.
  int64_t getNumCompletionsExpectedTotal() const {
    return numCompletionsExpectedTotal_;
  }

  // Returns true if RX accounting is complete: either all expected completions
  // have been delivered (bounded case) or there are no outstanding RRs
  // (unbounded case).
  bool rxAccountingComplete() const {
    return numCompletionsExpectedTotal_ >= 0
        ? (static_cast<int64_t>(numCompletionsDelivered_.load()) >=
           numCompletionsExpectedTotal_)
        : (numRROutstanding_.load() == 0);
  }

  // Blocks (with short sleeps) until rxAccountingComplete() is satisfied or the
  // timeout expires. Returns whether RX accounting completed within the
  // timeout. Does NOT touch the continue flag, so the poll thread keeps
  // draining while we wait.
  bool waitForRxComplete(std::chrono::milliseconds timeout);

  // Returns the number of detected sequence gaps/reorderings
  uint64_t getNumSeqGaps() const;

  // Runs CQ polling in a separate thread.
  void runServerRcCqPollingInThread(int64_t maxCompletions);

 private:
  virtual void launchPollingThread() {
    pollingThread_ = std::thread(
        &RdmaGenReceiver::runServerRcCqPollingInThread, this, LLONG_MAX);
    setCqPollThreadAffinity(
        pollingThread_,
        params().platform_type,
        CQThreadType::RECEIVER,
        params().gpu_idx);
  }

  // Wrapper method of ibv_post_recv().
  void postRecv(uint32_t buffer_idx, uint32_t qp_idx);

  std::thread pollingThread_;
  std::unique_ptr<RdmaGenPoolManager> poolManager_;
  std::atomic<int> numRROutstanding_ = 0;
  // Polling thread writes; main thread and completion barrier read.
  std::atomic<int> numCompletionsDelivered_ = 0;
  int numCompletionsExpected_ = -1;
  // Fixed total of RX completions expected for the whole test (does not count
  // down). Negative means unbounded. Used by the completion barrier to decide
  // when RX accounting is complete.
  int64_t numCompletionsExpectedTotal_ = -1;
  std::vector<uint32_t> expectedImmediateForQp_;

  // Sequence number tracking for debugging completion ordering per QP
  // (16-bit, wraps at 65536). lastCompletedSeqNumPerQp_ initialized to 65535
  // so first expected seqNum (65535+1 wrapped) equals 0.
  std::vector<uint16_t> nextRecvSeqNumPerQp_;
  std::vector<uint16_t> lastCompletedSeqNumPerQp_;
  std::atomic<uint64_t> numSeqGaps_ = 0;
};

} // namespace facebook::neteng::ai::rdma_gen
