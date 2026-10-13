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

#include "rdma_gen/RdmaGenReceiver.h"

#include <numeric>

#include "rdma_gen/RdmaGenFlags.h"

namespace facebook::neteng::ai::rdma_gen {

RdmaGenReceiver::RdmaGenReceiver(
    const Params& params,
    int terminateThreshold,
    const std::string& jsonPath)
    : RdmaGenResponder(params, terminateThreshold, jsonPath) {
  CHECK(params.role == EndpointRole::Receiver)
      << "receiver role is not set correctly";
  const auto eqp = params.effectiveQpCap();
  poolManager_ = std::make_unique<RdmaGenPoolManager>(eqp);
  numCompletionsExpected_ = params.num_completions_expected * eqp;
  // Fixed total used by the completion barrier. When num_completions_expected
  // is negative the test is unbounded, so leave the total negative as well.
  // Matches the historical pre-barrier completeness check that compared
  // delivered against params.num_completions_expected as the per-test total
  // (NOT per-QP). Tests that set num_completions_expected as the global
  // expected count across all QPs (e.g. incast harnesses that scale the flag
  // by num_clients rather than by qp_cap) would otherwise be required to
  // wait for qp_cap-times more completions than they actually send, hanging
  // the barrier and surfacing as spurious "completion barrier timed out".
  numCompletionsExpectedTotal_ = params.num_completions_expected < 0
      ? -1
      : static_cast<int64_t>(params.num_completions_expected);
  if (params.validate_immediate_data) {
    expectedImmediateForQp_ = std::vector<uint32_t>(eqp, 0);
  }
  // Initialize per-QP sequence tracking
  nextRecvSeqNumPerQp_ = std::vector<uint16_t>(eqp, 0);
  lastCompletedSeqNumPerQp_ = std::vector<uint16_t>(eqp, 65535);
}

RdmaGenReceiver::~RdmaGenReceiver() {
  if (numSeqGaps_ > 0) {
    LOG(ERROR) << fmt::format(
        "RdmaGenReceiver destroyed with {} sequence gaps detected",
        numSeqGaps_.load());
  }
}

void RdmaGenReceiver::init(
    ITransportProvider& transportProvider,
    std::unique_ptr<CudaWrapper> cudaWrapper) {
  RdmaGenResponder::init(transportProvider, std::move(cudaWrapper));
  // Wire the completion barrier: the control server's barrier handler blocks on
  // this receiver draining all expected RX completions before the client is
  // allowed to send terminate. This must not touch the continue flag.
  setBarrierTimeout(
      std::chrono::seconds(FLAGS_completion_barrier_timeout_secs));
  setRxCompleteWaiter([this](std::chrono::milliseconds timeout) {
    return this->waitForRxComplete(timeout);
  });
  // Post RRs early to avoid lost RC messages due to out of sync.
  for (uint64_t qp_idx = 0; qp_idx < params().effectiveQpCap(); qp_idx++) {
    for (uint64_t j = 0; j < params().rx_depth; j++) {
      uint32_t buffer_idx = (qp_idx * params().rx_depth) + j;
      postRecv(buffer_idx, qp_idx);
    }
  }
  LOG(INFO) << "launchPollingThread()";
  launchPollingThread();
}

RcBuffer RdmaGenReceiver::fetchDataReadyBuffer() {
  const auto eqp = params().effectiveQpCap();
  std::vector<uint32_t> qpIds(eqp);
  std::iota(qpIds.begin(), qpIds.end(), 0);
  while (continueFlag()) {
    for (uint32_t i = 0; i < eqp; i++) {
      if (!poolManager_->freePoolEmpty(i)) {
        return poolManager_->moveFromFreeToPending(i);
      }
    }
    poolManager_->waitForAvailable(qpIds, &getContinueFlag());
  }
  return RcBuffer();
}

void RdmaGenReceiver::returnBuffer(const RcBuffer& rcBuf) {
  poolManager_->moveFromPendingToWaiting(rcBuf.qp_idx, rcBuf.token);
  postRecv(rcBuf.token, rcBuf.qp_idx);
}

void RdmaGenReceiver::runServerRcCqPollingInThread(int64_t maxCompletions) {
  const uint32_t batch_size = params().batch_size;
  while (continueFlag()) {
    std::vector<WorkCompletionAdapter> wc(batch_size);
    int ne = 0;
    auto ret = endpoint_->notifyCqChannelEvents();
    if (ret < 0) {
      setContinueFlag(false);
      LOG(ERROR) << fmt::format("notifyCqChannelEvents failed: {}", ret);
      break;
    } else if (ret == 0) {
      continue;
    }

    do {
      ne = ctx()->cq_adapter->poll(std::data(wc), batch_size);
      if (ne < 0) {
        LOG(ERROR) << fmt::format("poll RC CQ failed: {}", ne);
        setContinueFlag(false);
      }
      if (ne <= 0) {
        break;
      }
      auto timestamp = getCurrentTimestampUs();
      for (int i = 0; i < ne; ++i) {
        if (wc[i].status != IBV_WC_SUCCESS) {
          LOG(ERROR) << fmt::format(
              "server RC  wc failed: {} {}",
              wc[i].status_str != nullptr ? wc[i].status_str : "unknown",
              wc[i].status);
          setFirstWqeError(wc[i].status);
          continue;
        }

        --numRROutstanding_;
        ++numCompletionsDelivered_;

        const auto wrId = parseWrIdWithSeq(wc[i].wr_id);

        // Check for out-of-order or gap in completions (handles 16-bit
        // wraparound)
        uint16_t expectedSeqNum =
            static_cast<uint16_t>(lastCompletedSeqNumPerQp_[wrId.qpIdx] + 1);
        if (wrId.seqNum != expectedSeqNum) {
          ++numSeqGaps_;
          LOG_FIRST_N(ERROR, 10) << fmt::format(
              "Completion sequence gap/reorder: qpIdx={}, expected seqNum={}, got seqNum={}, "
              "bufferIdx={}, wr_id=0x{:x}",
              wrId.qpIdx,
              expectedSeqNum,
              wrId.seqNum,
              wrId.bufferIdx,
              wc[i].wr_id);
        }
        lastCompletedSeqNumPerQp_[wrId.qpIdx] = wrId.seqNum;

        std::optional<uint32_t> maybeImmData = wc[i].imm_data;
        if (maybeImmData.has_value()) {
          if (!expectedImmediateForQp_.empty()) {
            if (expectedImmediateForQp_[wrId.qpIdx] != maybeImmData.value()) {
              setContinueFlag(false);
              LOG(ERROR) << fmt::format(
                  "Immediate data mismatch. Expected: {}, Received: {}, wr_id: 0x{:x}",
                  expectedImmediateForQp_[wrId.qpIdx],
                  maybeImmData.value(),
                  wc[i].wr_id);
            } else {
              LOG_FIRST_N(INFO, 5) << fmt::format(
                  "Immediate data verified. Expected: {}, Received: {}, wr_id: 0x{:x}",
                  expectedImmediateForQp_[wrId.qpIdx],
                  maybeImmData.value(),
                  wc[i].wr_id);
              ++expectedImmediateForQp_[wrId.qpIdx];
            }
          }
        }

        storeCqeWithTimestamp(wc[i], timestamp, maybeImmData);

        poolManager_->moveFromWaitingToFree(
            endpoint_->getBuffers(),
            wc[i].byte_len,
            wrId.qpIdx,
            wrId.bufferIdx,
            maybeImmData);

        if (numCompletionsDelivered_.load() >= maxCompletions) {
          setContinueFlag(false);
        }
      }
    } while (ne == batch_size);
  }

  LOG(INFO) << "Exiting runServerRcCqPollingInThread().";
}

void RdmaGenReceiver::postRecv(uint32_t buffer_idx, uint32_t qp_idx) {
  if (numCompletionsExpected_ == 0) {
    // No more messages to receive.
    return;
  } else if (numCompletionsExpected_ > 0) {
    --numCompletionsExpected_;
  }

  const uintptr_t addr = (uintptr_t)endpoint_->getBuffer(buffer_idx);

  const auto key = params().use_block ? ctx()->mr_list[0]->lkey()
                                      : ctx()->mr_list[buffer_idx]->lkey();

  RecvWorkRequestAdapter wr{
      .wr_id =
          createWrIdWithSeq(nextRecvSeqNumPerQp_[qp_idx]++, qp_idx, buffer_idx),
      .local_addr = addr,
      .length = params().message_size[params().baseQpIndex(qp_idx)],
      .lkey = key,
  };

  if (ctx()->peer_list[qp_idx]->postRecv(wr) != 0) {
    LOG(ERROR) << fmt::format(
        "ibv_post_recv failed for message: {}", buffer_idx);
  }
  ++numRROutstanding_;
}

std::unique_ptr<RdmaGenPoolManager>& RdmaGenReceiver::getPoolManager() {
  return poolManager_;
}

// Returns the number of outstanding RR messages.
int RdmaGenReceiver::getNumRrOutstanding() const {
  return numRROutstanding_;
}

// Returns the number of completions delivered.
int RdmaGenReceiver::getNumCompletionsDelivered() const {
  return numCompletionsDelivered_.load();
}

// Returns the number of expected completions.
int RdmaGenReceiver::getNumCompletionsExpected() const {
  return numCompletionsExpected_;
}

// Returns the number of detected sequence gaps/reorderings.
uint64_t RdmaGenReceiver::getNumSeqGaps() const {
  return numSeqGaps_;
}

bool RdmaGenReceiver::waitForRxComplete(std::chrono::milliseconds timeout) {
  constexpr auto kPollInterval = std::chrono::milliseconds(1);
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!rxAccountingComplete()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      LOG(WARNING) << fmt::format(
          "waitForRxComplete timed out after {}ms: delivered={}, expectedTotal={}, rrOutstanding={}",
          timeout.count(),
          numCompletionsDelivered_.load(),
          numCompletionsExpectedTotal_,
          numRROutstanding_.load());
      return false;
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  return true;
}

} // namespace facebook::neteng::ai::rdma_gen
