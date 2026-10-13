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

#include "rdma_gen/RdmaGenRequester.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <numeric>
#include <stdexcept>

#include <folly/Conv.h>
#include <folly/String.h>
#include <folly/Subprocess.h>

#include "rdma_gen/RdmaGenFlags.h"

using namespace std::chrono;

namespace facebook::neteng::ai::rdma_gen {

RdmaGenRequester::RdmaGenRequester(
    const Params& params,
    const std::vector<int64_t>& numMessagesPerServer,
    bool equalMessagesPerServer,
    const std::vector<std::string>& rcReceivers,
    const std::vector<int>& rcCmPorts,
    bool logMessageLatencies,
    const std::vector<int>& rcCos,
    const std::vector<enum ibv_mtu>& mtus,
    const enum ibv_qp_state targetQPState,
    const std::string& jsonPath)
    : numMessagesPerServer_(numMessagesPerServer),
      equalMessagesPerServer_(equalMessagesPerServer),
      rcReceivers_(rcReceivers),
      rcCmPorts_(rcCmPorts),
      rcCos_(rcCos),
      mtus_(mtus),
      targetQPState_(targetQPState),
      maxCompletions_(
          static_cast<int>(
              std::reduce(
                  numMessagesPerServer_.begin(),
                  numMessagesPerServer_.end()) *
              static_cast<int64_t>(rcReceivers_.size()))) {
  CHECK(
      params.role == EndpointRole::Sender ||
      params.role == EndpointRole::WriteRequester ||
      params.role == EndpointRole::WriteImmediateRequester ||
      params.role == EndpointRole::ReadRequester ||
      params.role == EndpointRole::AtomicFetchAddRequester ||
      params.role == EndpointRole::AtomicCmpSwapRequester)
      << "requester role is not set correctly";
  CHECK_GT(rcReceivers_.size(), 0) << "No server name is gives to attach to";
  CHECK_EQ(rcCmPorts_.size(), rcReceivers_.size());
  CHECK_EQ(params.qp_cap, rcReceivers_.size());
  // these asserts are to keep the linter happy
  // its not on the fast path so thought it made sense
  assert(rcCmPorts_.size() != 0);

  if (!rcCos_.empty()) {
    CHECK_EQ(rcCos_.size(), rcReceivers_.size())
        << "Mismatch in number of CoS entries and servers assigned";
    assert(rcCos_.size() != 0);
  }

  if (!mtus_.empty()) {
    CHECK_EQ(mtus_.size(), rcReceivers_.size())
        << "Mismatch in number of MTU entries and servers assigned";
    assert(mtus_.size() != 0);
  }

  endpoint_ = std::make_shared<RdmaGenEndpoint>(params);
  remoteRcQpInfo_ = std::make_shared<std::vector<RcQpInfo>>();
  const auto numEffectiveQps = params.effectiveQpCap();
  statsRecorder_ = std::make_unique<RdmaGenClientStatsRecorder>(
      params.qp_cap,
      params.tx_depth,
      numMessagesPerServer_,
      logMessageLatencies,
      jsonPath,
      params.sqp_per_qp);
  poolManager_ = std::make_unique<RdmaGenPoolManager>(numEffectiveQps);

  clkSrc_ = libsoftmetaroce::getClockSourceInstance();

  // Initialize per-QP sequence tracking
  nextSendSeqNumPerQp_ = std::vector<uint16_t>(numEffectiveQps, 0);
  lastCompletedSeqNumPerQp_ = std::vector<uint16_t>(numEffectiveQps, 65535);

  completionCallbacks_ = params.completion_callbacks;
  const auto hooks = parseCompletionHooks(params.completion_hooks);
  if (!hooks.empty()) {
    LOG(INFO) << "Parsed " << hooks.size()
              << " completion hook(s) (per-hook timeout "
              << params.completion_hook_timeout_sec << "s):";
    for (const auto& hook : hooks) {
      LOG(INFO) << "  @" << hook.threshold << " -> " << hook.cmd;
      if (hook.threshold >= static_cast<uint64_t>(getMaxCompletions())) {
        LOG(WARNING) << "Completion hook threshold " << hook.threshold
                     << " >= expected max completions " << getMaxCompletions()
                     << "; it will never fire";
      }
      completionCallbacks_.push_back(
          {.after_completions = hook.threshold,
           .action = [this, hook] { runCompletionHook(hook); }});
    }
  }
  std::stable_sort(
      completionCallbacks_.begin(),
      completionCallbacks_.end(),
      [](const auto& a, const auto& b) {
        return a.after_completions < b.after_completions;
      });
}

RdmaGenRequester::~RdmaGenRequester() {
  if (numSeqGaps_ > 0) {
    LOG(ERROR) << fmt::format(
        "RdmaGenRequester destroyed with {} sequence gaps detected",
        numSeqGaps_.load());
  }
}

void RdmaGenRequester::init(
    int sample_int,
    ITransportProvider& transportProvider,
    std::unique_ptr<CudaWrapper> cudaWrapper) {
  endpoint_->init(
      params().tx_depth,
      transportProvider,
      std::move(cudaWrapper),
      targetQPState_);
  const auto poolSize = params().effectiveQpCap();

  // Expand message_size from qp_cap to effectiveQpCap entries so
  // requesterInit can index by flat QP index.
  std::vector<uint32_t> expandedMsgSize(poolSize);
  for (uint32_t i = 0; i < poolSize; i++) {
    expandedMsgSize[i] = params().message_size[params().baseQpIndex(i)];
  }
  poolManager_->requesterInit(
      poolSize, params().tx_depth, expandedMsgSize, endpoint_->getBuffers());

  signalAllServers(/* terminate = */ false);

  enum ibv_mtu portActiveMtu =
      endpoint_->ctx()->pd_adapter->getPortInfo().active_mtu;

  // Walk thru each base QP & set Cos & MTU.
  // remoteRcQpInfo_ has one entry per base QP; peer_list has sqp_per_qp
  // entries per base QP. Base QPs are at indices i * sqp_per_qp.
  for (int i = 0; i < remoteRcQpInfo_->size(); i++) {
    int rcCos = rcCos_.size() > i ? rcCos_[i] : 0;
    enum ibv_mtu mtu = mtus_.size() > i ? mtus_[i] : IBV_MTU_1024; // default
    // Limit MTU to what the port supports, but only if portinfo reports a
    // valid active_mtu (> 0). In simulated/virtual environments the port
    // may not report active_mtu correctly, so we skip the limit in that case.
    if (portActiveMtu > 0 && mtu > portActiveMtu) {
      LOG(INFO) << fmt::format(
          "Limiting MTU from {} to port active_mtu {}",
          static_cast<int>(mtu),
          static_cast<int>(portActiveMtu));
      mtu = portActiveMtu;
    }
    const int base_idx = i * params().sqp_per_qp;
    CHECK(endpoint_->modifyRcQpsRts(
        remoteRcQpInfo_->at(i), base_idx, rcCos, mtu, targetQPState_))
        << fmt::format("Failed to connect QP {} to its remote peer", base_idx);
  }

  if (!params().single_cq_thread) {
    cqInfoVec_.resize(params().cq_pcq_depth);

    for (int i = 0; i < cqInfoVec_.size(); i++) {
      cqInfoVec_[i].wcVec.resize(params().batch_size);
    }
  }

  runClientRcCqPolling(sample_int);
}

RcBuffer RdmaGenRequester::getBuffer(int startQpIdx) {
  uint32_t qpIdx = 0;

  const auto poolSize = params().effectiveQpCap();
  while (true) {
    std::vector<uint32_t> qpIds;
    for (int i = 0; i < poolSize; i++) {
      qpIdx = (startQpIdx + i) % poolSize;
      const auto baseQpIdx = params().baseQpIndex(qpIdx);
      if (equalMessagesPerServer_ &&
          statsRecorder_->getMsgTotalCnt(baseQpIdx) >=
              numMessagesPerServer_[baseQpIdx]) {
        continue;
      }
      if (!poolManager_->freePoolEmpty(qpIdx)) {
        return poolManager_->moveFromFreeToPending(qpIdx);
      } else {
        qpIds.push_back(qpIdx);
      }
    }
    poolManager_->waitForAvailable(qpIds);
  }
}

void RdmaGenRequester::postBuffer(const RcBuffer& rcBuf) {
  poolManager_->moveFromPendingToWaiting(rcBuf.qp_idx, rcBuf.token);
  if (postRequest(rcBuf) != 0) {
    // If post failed, return buffer to free pool and decrement expected
    // completions since no CQE will be generated for this failed request
    poolManager_->moveFromWaitingToFree(
        endpoint_->getBuffers(),
        params().message_size[params().baseQpIndex(rcBuf.qp_idx)],
        rcBuf.qp_idx,
        rcBuf.token);
    std::unique_lock lock(maxCompletionMutex_);
    maxCompletions_--;
  }
}

void RdmaGenRequester::runClientRcCqPolling(int sample_int) {
  LOG(INFO) << "Launching RC Client CQ polling";

  if (params().single_cq_thread) {
    LOG(INFO) << "Single threaded RC client CQ polling";
    cqPollThread_ = std::thread(
        &RdmaGenRequester::runClientRcCqPollingInThread, this, sample_int);
  } else {
    LOG(INFO) << "Multi-threaded RC client CQ polling";
    cqPollThread_ = std::thread(
        &RdmaGenRequester::runClientRcCqPollingMultiThread, this, sample_int);
  }

  setCqPollThreadAffinity(
      cqPollThread_,
      params().platform_type,
      CQThreadType::REQUESTER,
      params().gpu_idx);
}

void RdmaGenRequester::processCompletion(
    const WorkCompletionAdapter& wc,
    int64_t timestamp) {
  const auto wrId = parseWrIdWithSeq(wc.wr_id);
  const auto bufferIdx = wrId.bufferIdx;
  const auto qpIdx = wrId.qpIdx;

  // Check for out-of-order or gap in completions (handles 16-bit wraparound)
  uint16_t expectedSeqNum =
      static_cast<uint16_t>(lastCompletedSeqNumPerQp_[qpIdx] + 1);
  if (wrId.seqNum != expectedSeqNum) {
    ++numSeqGaps_;
    LOG_FIRST_N(ERROR, 10) << fmt::format(
        "Tx completion sequence gap/reorder: qpIdx={}, expected seqNum={}, got seqNum={}, "
        "bufferIdx={}, wr_id=0x{:x}",
        qpIdx,
        expectedSeqNum,
        wrId.seqNum,
        bufferIdx,
        wc.wr_id);
  }
  lastCompletedSeqNumPerQp_[qpIdx] = wrId.seqNum;

  const auto baseQpIdx = params().baseQpIndex(qpIdx);
  if (wc.status != IBV_WC_SUCCESS) {
    LOG(ERROR) << fmt::format(
        "WC to remote RC server managed at: {}:{} failed with ibv_wc_status code: {} [{}]",
        rcReceivers_[baseQpIdx],
        rcCmPorts_[baseQpIdx],
        wc.status,
        wc.status_str != nullptr ? wc.status_str : "unknown");
    setFirstWqeError(wc.status);
  }
  storeCqeWithTimestamp(
      wc,
      timestamp,
      /*maybeImmData=*/std::nullopt,
      /*maybeSendFlags=*/
      getSendFlags(qpIdx, params().message_size[baseQpIdx]));

  statsRecorder_->recordTimeTaken(bufferIdx, baseQpIdx, timestamp);

  poolManager_->moveFromWaitingToFree(
      endpoint_->getBuffers(),
      params().message_size[baseQpIdx],
      qpIdx,
      bufferIdx);
}

void RdmaGenRequester::processClientCqeInThread(int sample_int) {
  auto num_processed = 0;
  uint32_t vec_idx = 0;

  std::chrono::seconds interval(sample_int);
  std::chrono::time_point<libsoftmetaroce::ClockSource> end =
      clkSrc_->now() + interval;

  const int cq_prodconsq_depth = params().cq_pcq_depth;

  cqStartMutex_.lock();
  while (num_processed < getMaxCompletions()) {
    cqInfoVec_[vec_idx].wcMutex.lock();

    if (cqInfoVec_[vec_idx].wcFlag == 0) {
      cqInfoVec_[vec_idx].wcMutex.unlock();
      continue;
    }

    CHECK_GE(cqInfoVec_[vec_idx].ne, 0) << fmt::format(
        "RC poll CQ failed: {} {}", cqInfoVec_[vec_idx].ne, num_processed);

    for (int i = 0; i < cqInfoVec_[vec_idx].ne; ++i) {
      processCompletion(
          cqInfoVec_[vec_idx].wcVec[i], cqInfoVec_[vec_idx].wcTimeStamp);

      cqInfoVec_[vec_idx].wcFlag = false;
      cqInfoVec_[vec_idx].wcMutex.unlock();

      num_processed++;
    }

    if (clkSrc_->now() > end || num_processed == getMaxCompletions()) {
      statsRecorder_->printStats();
      statsRecorder_->addJsonEntry(rcReceivers_);
      statsRecorder_->resetStats();
      end = clkSrc_->now() + interval;
    }
    checkCompletionCount();
    vec_idx++;
    vec_idx = vec_idx % cq_prodconsq_depth;
  }
}

void RdmaGenRequester::runClientRcCqPollingMultiThread(int sample_int) {
  uint32_t vec_idx = 0;
  const int cq_prodconsq_depth = params().cq_pcq_depth;
  cqInfoVec_.resize(cq_prodconsq_depth);
  statsRecorder_->setupJson();

  cqStartMutex_.lock();
  // kick off thread for processing completions
  cqeProcessThread_ = std::thread(
      &RdmaGenRequester::processClientCqeInThread, this, sample_int);

  cqStartMutex_.unlock();
  while (numCompletions_ < getMaxCompletions()) {
    auto ret = endpoint_->notifyCqChannelEvents();

    CHECK_GE(ret, 0);

    if (ret == 0) {
      continue;
    }

    cqInfoVec_[vec_idx].wcMutex.lock();

    if (cqInfoVec_[vec_idx].wcFlag == 1) {
      cqInfoVec_[vec_idx].wcMutex.unlock();
      continue;
    }

    // cqInfoVec_[vec_idx].wcVec.capacity() is equivalent to the batch size
    cqInfoVec_[vec_idx].ne = ctx()->cq_adapter->poll(
        std::data(cqInfoVec_[vec_idx].wcVec),
        static_cast<int>(cqInfoVec_[vec_idx].wcVec.capacity()));
    cqInfoVec_[vec_idx].wcTimeStamp = getCurrentTimestampUs();
    if (cqInfoVec_[vec_idx].ne == 0) {
      cqInfoVec_[vec_idx].wcMutex.unlock();
      continue;
    }

    numCompletions_ += cqInfoVec_[vec_idx].ne;
    cqInfoVec_[vec_idx].wcFlag = true;
    cqInfoVec_[vec_idx].wcMutex.unlock();
    // Completion callbacks run after releasing the wc slot lock.
    checkCompletionTriggers();
    vec_idx++;
    vec_idx = vec_idx % cq_prodconsq_depth;
  }
}

std::vector<RdmaGenRequester::CompletionHook>
RdmaGenRequester::parseCompletionHooks(const std::string& spec) {
  std::vector<CompletionHook> hooks;
  if (spec.empty()) {
    return hooks;
  }
  std::vector<folly::StringPiece> entries;
  // ',' is the entry delimiter; a command therefore cannot contain a literal
  // comma (documented caveat in the flag help).
  folly::split(',', spec, entries);
  for (auto entry : entries) {
    // Tolerate shell-style whitespace and stray/duplicate/trailing commas:
    // trim each entry and skip ones that are empty (e.g. from "100:cmd," or
    // "100:cmd,,200:cmd") rather than reporting a misleading "missing ':'".
    entry = folly::trimWhitespace(entry);
    if (entry.empty()) {
      continue;
    }
    const auto colon = entry.find(':');
    if (colon == folly::StringPiece::npos) {
      throw std::invalid_argument(
          fmt::format(
              "Malformed --completion_hooks entry (missing ':'): '{}'",
              entry.str()));
    }
    // <threshold> is the leading integer up to the first ':'; <command> is
    // everything after it, passed verbatim to /bin/sh -c (may contain ':',
    // ';', spaces, pipes).
    const auto maybeThreshold =
        folly::tryTo<uint64_t>(folly::trimWhitespace(entry.subpiece(0, colon)));
    if (!maybeThreshold.hasValue() || maybeThreshold.value() == 0) {
      throw std::invalid_argument(
          fmt::format(
              "Malformed --completion_hooks threshold (want positive integer): '{}'",
              entry.str()));
    }
    std::string cmd = folly::trimWhitespace(entry.subpiece(colon + 1)).str();
    if (cmd.empty()) {
      throw std::invalid_argument(
          fmt::format(
              "Malformed --completion_hooks entry (empty command): '{}'",
              entry.str()));
    }
    hooks.push_back(CompletionHook{maybeThreshold.value(), std::move(cmd)});
  }
  // Ascending by threshold; stable so equal thresholds keep input order.
  std::stable_sort(
      hooks.begin(), hooks.end(), [](const auto& a, const auto& b) {
        return a.threshold < b.threshold;
      });
  return hooks;
}

void RdmaGenRequester::runCompletionHook(const CompletionHook& hook) {
  const int32_t timeoutSec = params().completion_hook_timeout_sec;
  // Running synchronously here deliberately pauses new CQE polls until the
  // command returns, making the transition happen at a deterministic
  // completion count.
  LOG(INFO) << "Running completion hook @" << hook.threshold << " ("
            << numCompletions_ << " completions): " << hook.cmd;
  // The child inherits our stdout/stderr, so its output streams straight to
  // the log; this also sidesteps any pipe-drain deadlock while we block.
  folly::Subprocess proc(std::vector<std::string>{"/bin/sh", "-c", hook.cmd});
  const folly::ProcessReturnCode rc = timeoutSec > 0
      ? proc.waitOrTerminateOrKill(
            std::chrono::seconds(timeoutSec), std::chrono::seconds(timeoutSec))
      : proc.wait();
  if (!rc.exited() || rc.exitStatus() != 0) {
    LOG(FATAL) << "Completion hook @" << hook.threshold << " failed ("
               << rc.str() << "): " << hook.cmd;
  }
  LOG(INFO) << "Completion hook @" << hook.threshold
            << " completed: " << hook.cmd;
}

void RdmaGenRequester::runDueCompletionCallbacks() {
  // A batched poll can carry the count past several callbacks at once, so run
  // every due callback in order.
  for (; nextCallbackIdx_ < completionCallbacks_.size() &&
       numCompletions_ >=
           completionCallbacks_[nextCallbackIdx_].after_completions;
       ++nextCallbackIdx_) {
    const auto& callback = completionCallbacks_[nextCallbackIdx_];
    try {
      callback.action();
    } catch (const std::exception& e) {
      LOG(FATAL) << "Completion callback due at " << callback.after_completions
                 << " threw: " << e.what();
    }
  }
}

void RdmaGenRequester::checkCompletionTriggers() {
  runDueCompletionCallbacks();
}

void RdmaGenRequester::runClientRcCqPollingInThread(int sample_int) {
  const uint32_t batch_size = params().batch_size;
  std::chrono::seconds interval(sample_int);
  std::chrono::time_point<libsoftmetaroce::ClockSource> end =
      clkSrc_->now() + interval;
  statsRecorder_->setupJson();

  std::vector<WorkCompletionAdapter> wc(batch_size);
  while (numCompletions_ < getMaxCompletions()) {
    auto ret = endpoint_->notifyCqChannelEvents();
    CHECK_GE(ret, 0);
    if (ret == 0) {
      continue;
    }

    int ne = 0;
    do {
      ne = ctx()->cq_adapter->poll(std::data(wc), batch_size);
      CHECK_GE(ne, 0) << fmt::format("RC poll CQ failed: {}", ne);

      for (int i = 0; i < ne; ++i) {
        processCompletion(wc[i], getCurrentTimestampUs());
        ++numCompletions_;
        checkCompletionTriggers();

        if (clkSrc_->now() > end || numCompletions_ == getMaxCompletions()) {
          statsRecorder_->printStats();
          statsRecorder_->addJsonEntry(rcReceivers_);
          statsRecorder_->resetStats();
          end = clkSrc_->now() + interval;
        }
      }
      checkCompletionCount();
    } while (ne == batch_size && numCompletions_ < getMaxCompletions());
  }
}

void RdmaGenRequester::writeJsonReport(
    double gbps,
    double mpps,
    OperationType opType,
    VerbsMode::Value verbsMode,
    std::optional<double> cpLatencyUsecs) {
  statsRecorder_->finalizeJsonReport(
      gbps,
      mpps,
      rcReceivers_,
      opType,
      verbsMode,
      *errCqeLog_.rlock(),
      *allCqeLog_.rlock(),
      cpLatencyUsecs);
}

void RdmaGenRequester::storeCqeWithTimestamp(
    const WorkCompletionAdapter& cqe,
    int64_t timestamp,
    std::optional<uint32_t> maybeImmData,
    std::optional<uint32_t> maybeSendFlags) {
  if (statsRecorder_ != nullptr) {
    if (cqe.status != IBV_WC_SUCCESS) {
      errCqeLog_.wlock()->emplace_back(
          CqeLogEntry{cqe, timestamp, maybeImmData, maybeSendFlags});
    }
    allCqeLog_.wlock()->emplace_back(
        CqeLogEntry{cqe, timestamp, maybeImmData, maybeSendFlags});
  }
}

std::unique_ptr<RdmaGenPoolManager>& RdmaGenRequester::getPoolManager() {
  return poolManager_;
}

void RdmaGenRequester::terminateSignal() {
  ControlMessage controlMsg = ControlMessage::Terminate;
  std::set<std::pair<std::string, int>> terminated;

  for (int qpIdx = 0; qpIdx < rcReceivers_.size(); qpIdx++) {
    const auto& servername = rcReceivers_[qpIdx];
    const auto port = rcCmPorts_[qpIdx];
    std::pair<std::string, int> receiver(servername, port);
    if (!terminated.contains(receiver)) {
      if (!cmClient_->signalServer(servername, port, qpIdx, controlMsg)) {
        LOG(WARNING) << fmt::format(
            "RDMA connection management error for {} at port: {}",
            servername,
            port);
      }
      terminated.insert(receiver);
    }
  }
}

void RdmaGenRequester::barrierAllServers(int completed) {
  if (!FLAGS_use_completion_barrier) {
    return;
  }
  std::set<std::pair<std::string, int>> barriered;

  for (int qpIdx = 0; qpIdx < rcReceivers_.size(); qpIdx++) {
    const auto& servername = rcReceivers_[qpIdx];
    const auto port = rcCmPorts_[qpIdx];
    std::pair<std::string, int> receiver(servername, port);
    if (!barriered.contains(receiver)) {
      bool ok = false;
      try {
        ok = cmClient_->completionBarrier(
            servername, port, qpIdx, static_cast<uint64_t>(completed));
      } catch (const std::exception& ex) {
        LOG(WARNING) << fmt::format(
            "Completion barrier threw for {} at port: {}: {}",
            servername,
            port,
            ex.what());
        ok = false;
      }
      if (!ok) {
        LOG(WARNING) << fmt::format(
            "Completion barrier failed/timed out for {} at port: {}; proceeding to terminate",
            servername,
            port);
      }
      barriered.insert(receiver);
    }
  }
}

void RdmaGenRequester::signalAllServers(bool terminate) {
  ControlMessage controlMsg;
  if (terminate) {
    terminateSignal();
    return;
  }

  switch (params().role) {
    case EndpointRole::Sender: {
      controlMsg = ControlMessage::GetRemoteInfo;
      break;
    }
    case EndpointRole::WriteRequester:
    case EndpointRole::WriteImmediateRequester: {
      controlMsg = ControlMessage::Write;
      break;
    }
    case EndpointRole::ReadRequester:
    case EndpointRole::AtomicFetchAddRequester:
    case EndpointRole::AtomicCmpSwapRequester: {
      controlMsg = ControlMessage::Read;
      break;
    }
    default: {
      LOG(ERROR) << "unsupported role for signalAllServers";
      return;
    }
  }

  bool success = false;
  for (int i = 0; i < rcReceivers_.size(); i++) {
    bool res =
        cmClient_->signalServer(rcReceivers_[i], rcCmPorts_[i], i, controlMsg);
    if (!res) {
      LOG(WARNING) << fmt::format(
          "RDMA connection management error for {} at port: {}",
          rcReceivers_[i],
          rcCmPorts_[i]);
    }
    success |= res;
  }

  if (!success) {
    throw std::runtime_error("Could not connect to any server");
  }
}

uint64_t RdmaGenRequester::getNumSeqGaps() const {
  return numSeqGaps_;
}

bool RdmaGenRequester::exchangeMetadata(
    const std::map<std::string, std::string>& localMetadata,
    std::vector<std::map<std::string, std::string>>& remoteMetadataPerServer) {
  remoteMetadataPerServer.clear();
  remoteMetadataPerServer.reserve(rcReceivers_.size());
  for (size_t i = 0; i < rcReceivers_.size(); ++i) {
    std::map<std::string, std::string> serverMetadata;
    if (!cmClient_->exchangeMetadata(
            rcReceivers_[i], rcCmPorts_[i], localMetadata, serverMetadata)) {
      LOG(ERROR) << fmt::format(
          "Failed to exchange metadata with {}:{}",
          rcReceivers_[i],
          rcCmPorts_[i]);
      return false;
    }
    remoteMetadataPerServer.push_back(std::move(serverMetadata));
  }
  return true;
}

} // namespace facebook::neteng::ai::rdma_gen
