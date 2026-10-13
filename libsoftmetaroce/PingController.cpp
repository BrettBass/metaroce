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

#include <libsoftmetaroce/PacketSocketFactory.h>
#include <libsoftmetaroce/PingController.h>
#include <libsoftmetaroce/Qp.h>
#include <libsoftmetaroce/codec/ControlHeader.h>
#include <libsoftmetaroce/codec/Packet.h>

#include <fmt/core.h>
#include <folly/CancellationToken.h>
#include <folly/ScopeGuard.h>
#include <folly/String.h>
#include <folly/coro/Timeout.h>
#include <folly/coro/ViaIfAsync.h>
#include <gflags/gflags.h>
#include <glog/logging.h>

DEFINE_uint32(
    libsoftmetaroce_ping_default_timeout_ms,
    1000,
    "Default timeout for ping requests in milliseconds");

namespace libsoftmetaroce {

// ============================================================================
// PingController Implementation
// ============================================================================

PingController::PingController(
    SocketMaker socketMaker,
    uint16_t sendToPort,
    const std::atomic<uint16_t>* activePortSet)
    : socketMaker_(std::move(socketMaker)),
      sendToPort_(sendToPort),
      activePortSet_(activePortSet) {
  LOG(INFO) << "PingController initialized with sendToPort: " << sendToPort_;
}

PingController::~PingController() {
  cancelAllPendingPings();

  // Wait for all outstanding co_sendPing coroutines to complete before tearing
  // down sendSockets_; cancelAllPendingPings() above already resolved their
  // batons.
  {
    std::unique_lock<std::mutex> lock(outstandingMutex_);
    outstandingCv_.wait(lock, [this]() { return outstandingPings_ == 0; });
  }

  sendSockets_.lock()->sockets.clear();

  LOG(INFO) << "PingController destroyed";
}

PacketModifierSocket* PingController::getSocketForAddress(
    LockedSendSockets& sendSockets,
    folly::IPAddress localAddr,
    const ibv_gid_entry& entry) {
  if (auto it = sendSockets->sockets.find(localAddr);
      it != sendSockets->sockets.end()) {
    return it->second.get();
  }

  auto socket = makePacketModifierSocket(&evb_, entry, socketMaker_);
  sendSockets->sourcePorts.bindNext(*socket, localAddr);

  auto* rawPtr = socket.get();
  sendSockets->sockets.emplace(localAddr, std::move(socket));
  LOG(INFO) << "PingController send socket created on " << localAddr;
  return rawPtr;
}

folly::coro::Task<PingResult> PingController::co_sendPing(
    folly::IPAddress localAddr,
    folly::IPAddress remoteAddr,
    ibv_gid_entry entry,
    uint32_t sourceQpNum,
    uint32_t remoteQpNum,
    uint8_t tagNum,
    uint16_t flowId,
    uint16_t supportedFlowCount,
    std::chrono::milliseconds timeout) {
  {
    std::lock_guard<std::mutex> lock(outstandingMutex_);
    ++outstandingPings_;
  }
  auto outstandingGuard = folly::makeGuard([this]() {
    // Notify before releasing outstandingMutex_; the destructor reacquires this
    // mutex before destroying outstandingCv_.
    std::lock_guard<std::mutex> lock(outstandingMutex_);
    if (--outstandingPings_ == 0) {
      outstandingCv_.notify_all();
    }
  });

  uint32_t timestamp = getNowNs();
  uint16_t pingId = pingIdCounter_.fetch_add(1);

  auto pending = std::make_shared<PendingPing>();
  pending->sentTime = std::chrono::steady_clock::now();

  // Build the ping packet
  using OperationType = BaseHeader::Opcode::OperationType;
  auto baseHeader =
      BaseHeader::BaseHeaderBuilder()
          .setOpcode(BaseHeader::Opcode(OperationType::UNRELIABLE_CONTROL))
          .setVersion(0)
          .setFlowId(flowId)
          .setDstQp(remoteQpNum)
          .setSrcQp(sourceQpNum)
          .build();

  auto controlHeader =
      ControlHeader::ControlHeaderBuilder()
          .setType(static_cast<uint16_t>(ControlPacketType::PING))
          .setTimestamp(timestamp)
          .setPingId(pingId)
          .build();

  auto pingHeader = PingHeader::PingHeaderBuilder()
                        .setTag(tagNum)
                        .setErrorState(PingErrorState::NO_ERROR)
                        .setSupportedFlowCount(supportedFlowCount)
                        .setActivePortSet(loadActivePortSet())
                        .build();

  auto packet = std::make_unique<PingPacket>(
      std::move(baseHeader),
      std::move(controlHeader),
      std::move(pingHeader),
      nullptr,
      0);

  // Register pending ping
  {
    auto locked = pendingPings_.wlock();
    locked->emplace(pingId, pending);
  }

  // Send the ping synchronously (write() is a blocking sendmsg() call)
  try {
    auto sendSockets = sendSockets_.lock();
    auto* socket = getSocketForAddress(sendSockets, localAddr, entry);
    folly::SocketAddress destAddr(remoteAddr, sendToPort_);
    auto ret = socket->write(destAddr, std::move(packet));
    if (ret <= 0) {
      throw std::runtime_error(
          fmt::format(
              "Failed to send ping to {}: {}",
              destAddr.describe(),
              folly::errnoStr(errno)));
    }
    VLOG(1) << "Sent ping packet to " << destAddr;
  } catch (const std::exception& e) {
    LOG(ERROR) << "Failed to send ping (pingId=" << pingId << "): " << e.what();
    handlePingResponse(
        pingId, PingErrorState::NO_ERROR, PingOutcome::TIMED_OUT);
    co_return pending->result;
  }

  // Wait for response with timeout. The CancellationCallback bridges
  // folly::coro::timeout's cancellation to the Baton, which is not
  // natively cancellation-aware.
  try {
    co_await folly::coro::timeout(
        [&]() -> folly::coro::Task<void> {
          auto ct = co_await folly::coro::co_current_cancellation_token;
          folly::CancellationCallback cb(ct, [&]() { pending->baton.post(); });
          co_await pending->baton;
        }(),
        timeout);
  } catch (const folly::FutureTimeout&) {
    handlePingResponse(
        pingId, PingErrorState::NO_ERROR, PingOutcome::TIMED_OUT);
  }

  co_return pending->result;
}

void PingController::handlePingResponse(
    uint16_t pingId,
    PingErrorState errorState,
    PingOutcome outcome) {
  std::shared_ptr<PendingPing> pending;

  {
    auto locked = pendingPings_.wlock();
    auto it = locked->find(pingId);
    if (it == locked->end()) {
      if (outcome == PingOutcome::COMPLETED) {
        LOG(WARNING) << "Received ping response for unknown pingId: " << pingId
                     << " (may have already timed out)";
      }
      return;
    }
    pending = std::move(it->second);
    locked->erase(it);

    // Write result under the lock so the write is visible to co_sendPing's
    // read of pending->result after the timeout handler's lock cycle.
    if (outcome == PingOutcome::COMPLETED) {
      auto now = std::chrono::steady_clock::now();
      auto rtt = std::chrono::duration_cast<std::chrono::nanoseconds>(
          now - pending->sentTime);
      pending->result = {
          .outcome = PingOutcome::COMPLETED,
          .errorState = errorState,
          .rtt = rtt};
    }
  }

  if (outcome == PingOutcome::COMPLETED) {
    VLOG(1) << "Ping completed: rtt="
            << std::chrono::duration_cast<std::chrono::microseconds>(
                   pending->result.rtt)
                   .count()
            << "us"
            << ", status=" << errorState;
  } else {
    LOG(WARNING) << "Ping timed out for pingId: " << pingId;
  }

  pending->baton.post();
}

size_t PingController::getNumPendingPings() const {
  return pendingPings_.rlock()->size();
}

void PingController::cancelAllPendingPings() {
  std::vector<std::shared_ptr<PendingPing>> toCancel;

  {
    auto locked = pendingPings_.wlock();
    for (auto& [id, pending] : *locked) {
      pending->result = {
          .outcome = PingOutcome::CANCELLED,
          .errorState = PingErrorState::NO_ERROR,
          .rtt = std::chrono::nanoseconds::zero()};
      toCancel.push_back(std::move(pending));
    }
    locked->clear();
  }

  for (auto& pending : toCancel) {
    pending->baton.post();
  }
}

} // namespace libsoftmetaroce
